#include "Animation.hpp"
#include <Math/Decompose.hpp>
#include <tracy/Tracy.hpp>
#include <algorithm>
#include <cmath>

namespace
{
FSerializedBounds BoundsFromVertices(Span<const FVertex> vertices)
{
    if (vertices.empty())
        return {};
    FSerializedBounds bounds = FSerializedBounds::Empty();
    for (FVertex const& vertex : vertices)
        bounds += vertex.position;
    return bounds;
}

// Writes one animated node's world transform onto a target, if that node is animated. Reads the
// (already-evaluated) scene-node pose read-only; each call writes a disjoint target, so this is
// safe to invoke concurrently across instances.
void ApplyAnimatedNode(FAnimationRuntime const& runtime, int32_t node, FTransform& dst)
{
    auto const& affected = runtime.nodeAffected;
    if (node < 0 || static_cast<size_t>(node) >= affected.size() || !affected[node])
        return;
    FPose const& nodePose = runtime.poses[static_cast<size_t>(runtime.sceneNodeSkeleton)];
    decompose(nodePose.globals[node], dst.scale, dst.rotation, dst.transform);
}

// Maps the master clock to a strip's clip-local time (timescale, clip window, optional cyclic restart).
float StripClipLocalTime(FNlaStrip const& strip, float time)
{
    float const span = std::max(strip.clipEnd - strip.clipStart, 0.0f);
    float const u = (time - strip.stripStart) * strip.timeScale;
    return strip.cyclic && span > 0.0f ? strip.clipStart + std::fmod(u, span)
                                       : std::clamp(strip.clipStart + u, strip.clipStart, strip.clipEnd);
}

// Invokes fn(setIndex, clipLocalTime, influence) for every active NLA strip (non-mute track, within
// its time window, influence > 0) at runtime.time, walking tracks low->high. Shared by the pose and
// morph passes so both blend strips identically.
template <class Fn>
void ForEachActiveStrip(FImportedScene const& scene, FAnimationRuntime const& runtime, Fn&& fn)
{
    for (FNlaTrack const& track : scene.mTables.nlaTracks)
    {
        if (track.mute)
            continue;
        for (FNlaStrip const& strip : track.strips)
        {
            if (strip.influence <= 0.0f || runtime.time < strip.stripStart || runtime.time > strip.stripEnd)
                continue;
            auto it = runtime.setByName.find(strip.source);
            if (it == runtime.setByName.end())
                continue;
            fn(it->second, StripClipLocalTime(strip, runtime.time), strip.influence);
        }
    }
}

// CPU deformation for one dynamic mesh: morph (POSITION deltas) then skin, written into the
// mesh's dynamic ring slot. Uses the calling worker's scratch lane; each mesh's ring region +
// output is disjoint, so this is race-free across workers.
void DeformMesh(FAnimationRuntime& runtime, FImportedScene* scene, GPUScene* gpu, FDynamicMeshRuntime const& rt,
                size_t worker)
{
    ZoneScopedN("Skin Mesh");
    if (gpu->Query(rt.handle) != GPUScene::Result::Ready)
        return;
    auto const& skeletons = scene->mTables.skeletons;
    size_t n = rt.bind.size();
    const FVertex* base = rt.bind.data();

    // Morph: base' = base + Σ weightₜ · deltaₜ (POSITION only), then rebuild normals from the
    // morphed surface (skinning, applied next, rotates those fresh normals correctly). Weights are
    // resolved through the NLA exactly like the skeletal pose: reset to rest (all-zero weights), then
    // blend each active strip's morph channels for this mesh by influence.
    if (rt.morphTargetCount > 0 && !rt.morphDeltas.empty())
    {
        uint32_t const tc = rt.morphTargetCount;
        Vector<float>& acc = runtime.morphAccum[worker];
        Vector<float>& smp = runtime.morphWeights[worker];
        std::fill_n(acc.begin(), tc, 0.0f);
        ForEachActiveStrip(*scene, runtime, [&](uint32_t setIdx, float cl, float influence)
        {
            FAnimationSet const& set = runtime.animations[setIdx];
            for (uint32_t clipIdx : set.clips)
            {
                FAnimationClip const& clip = scene->mTables.clips[clipIdx];
                for (FMorphChannel const& mc : clip.morphChannels)
                {
                    if (mc.mesh != rt.meshId || mc.targetCount == 0)
                        continue;
                    uint32_t const w = std::min(mc.targetCount, tc);
                    SampleTrack(Span<const float>(mc.times.data(), mc.times.size()),
                                Span<const float>(mc.values.data(), mc.values.size()), mc.targetCount, mc.interp, cl,
                                Span<float>(smp.data(), mc.targetCount));
                    for (uint32_t i = 0; i < w; ++i)
                        acc[i] += (smp[i] - acc[i]) * influence;
                }
            }
        });
        Vector<FVertex>& morphed = runtime.morphs[worker];
        for (size_t v = 0; v < n; ++v)
        {
            FVertex mv = rt.bind[v];
            for (uint32_t tt = 0; tt < tc; ++tt)
                mv.position += acc[tt] * rt.morphDeltas[static_cast<size_t>(tt) * n + v];
            morphed[v] = mv;
        }
        RecomputeNormals(Span<FVertex>(morphed.data(), n),
                         Span<const uint32_t>(rt.indices.data(), rt.indices.size()));
        base = morphed.data();
    }

    Span<std::byte> dst = gpu->UpdateDynamicGeometry(rt.handle);
    Span<FQVertex> out(reinterpret_cast<FQVertex*>(dst.data()), dst.size() / sizeof(FQVertex));
    FSerializedBounds deformedBounds{};
    if (rt.skeleton >= 0)
    {
        FSkeleton const& skel = skeletons[rt.skeleton];
        Span<mat4> palette(runtime.skins[worker].data(), skel.Count());
        ComputeSkinningMatrices(skel, runtime.poses[rt.skeleton], palette);
        SkinVertices(Span<const FVertex>(base, n),
                     Span<const FSkinBinding>(rt.binding.data(), rt.binding.size()),
                     Span<const mat4>(palette.data(), palette.size()), out, &deformedBounds);
    }
    else
    {
        deformedBounds = BoundsFromVertices(Span<const FVertex>(base, n));
        for (size_t v = 0; v < n && v < out.size(); ++v)
            out[v] = FQVertex::Pack(base[v]);
    }
    if (rt.meshIndex < scene->mTables.meshes.size())
        scene->mTables.meshes[rt.meshIndex].bounds = deformedBounds;
}
} // namespace

void FAnimationRuntime::Setup(FImportedScene const& scene, FSceneGPUResources const& resources, size_t lanes)
{
    *this = FAnimationRuntime{};
    auto const& skeletons = scene.mTables.skeletons;

    FBlobDeserializer blobs = scene.GetBlobDeserializer();
    auto sceneMeshes = scene.GetMeshes();
    uint32_t maxJoints = 0;
    for (auto const& skel : skeletons)
        maxJoints = std::max(maxJoints, skel.Count());
    poses.resize(skeletons.size());

    // Joint-local indices stay indices; resolve skeleton ids via scene index.
    auto skeletonIndex = [&](FUUID id) { return scene.SkeletonIndex(id); };

    // Rigid node animation: mark nodes that are animated or descend from an animated node, so the
    // per-frame override only touches moving instances/lights (and a held pose lets PT converge).
    sceneNodeSkeleton = skeletonIndex(scene.mTables.sceneNodeSkeleton);
    if (sceneNodeSkeleton >= 0)
    {
        FSkeleton const& nodes = skeletons[sceneNodeSkeleton];
        nodeAffected.assign(nodes.Count(), 0u);
        for (FAnimationClip const& clip : scene.mTables.clips)
            if (skeletonIndex(clip.skeleton) == sceneNodeSkeleton)
                for (FAnimChannel const& channel : clip.channels)
                    if (channel.joint < nodes.Count())
                        nodeAffected[channel.joint] = 1u;
        // Topological storage (parent < child) lets one forward pass propagate to descendants.
        for (uint32_t i = 0; i < nodes.Count(); ++i)
        {
            int32_t parent = nodes.joints[i].parent;
            if (parent >= 0 && nodeAffected[static_cast<uint32_t>(parent)])
                nodeAffected[i] = 1u;
        }
        // Whether rigid animation actually moves any committed transform is static, so resolve it
        // once here (rather than racing on a shared flag while applying transforms in parallel).
        auto isAffected = [&](int32_t node)
        { return node >= 0 && static_cast<size_t>(node) < nodeAffected.size() && nodeAffected[node]; };
        for (FInstance const& instance : scene.mTables.instances)
            if ((rigidDrivesTransforms = isAffected(instance.node)))
                break;
        if (!rigidDrivesTransforms)
            for (FLight const& light : scene.mTables.lights)
                if ((rigidDrivesTransforms = isAffected(light.node)))
                    break;
        // Camera motion drives the caller's view (via GetCameraTransform), not a GPUScene commit,
        // so it stays out of rigidDrivesTransforms. Only the first camera is tracked.
        if (!scene.mTables.cameras.empty() && isAffected(scene.mTables.cameras.front().node))
            cameraNode = scene.mTables.cameras.front().node;
    }

    for (size_t m = 0; m < sceneMeshes.size(); ++m)
    {
        FSerializedMesh const& mesh = sceneMeshes[m];
        int32_t const skelIdx = skeletonIndex(mesh.skeleton);
        bool const skinned = mesh.skinBinding.count != 0 && skelIdx >= 0;
        bool const morphed = mesh.morphTargetCount > 0 && mesh.morphPositions.count > 0;
        if ((!skinned && !morphed) || m >= resources.meshGeometry.size())
            continue;
        FDynamicMeshRuntime rt(GLOBAL_ALLOC);
        rt.handle = resources.meshGeometry[m];
        rt.meshIndex = m;
        rt.meshId = mesh.id;
        Vector<FQVertex> quantized = blobs.ReadArray<FQVertex>(mesh.vertices, GLOBAL_ALLOC);
        rt.bind.resize(quantized.size());
        for (size_t i = 0; i < quantized.size(); ++i)
            rt.bind[i] = FQVertex::Unpack(quantized[i]);
        if (skinned)
        {
            rt.skeleton = skelIdx;
            rt.binding = blobs.ReadArray<FSkinBinding>(mesh.skinBinding, GLOBAL_ALLOC);
        }
        if (morphed)
        {
            rt.morphTargetCount = mesh.morphTargetCount;
            rt.morphDeltas = blobs.ReadArray<float3>(mesh.morphPositions, GLOBAL_ALLOC);
            // Topology is needed to recompute normals from the morphed positions each frame.
            if (!mesh.lods.empty())
                rt.indices = blobs.ReadArray<uint32_t>(mesh.lods[0].indices, GLOBAL_ALLOC);
        }
        meshes.push_back(std::move(rt));
    }

    // Per-worker scratch, pre-sized to the largest mesh/skeleton so skinning jobs never reallocate.
    uint32_t maxVerts = 0, maxTargets = 0;
    for (FDynamicMeshRuntime const& rt : meshes)
    {
        maxVerts = std::max(maxVerts, static_cast<uint32_t>(rt.bind.size()));
        maxTargets = std::max(maxTargets, rt.morphTargetCount);
    }
    skins.resize(lanes, Vector<mat4>{GLOBAL_ALLOC});
    morphs.resize(lanes, Vector<FVertex>{GLOBAL_ALLOC});
    morphWeights.resize(lanes, Vector<float>{GLOBAL_ALLOC});
    morphAccum.resize(lanes, Vector<float>{GLOBAL_ALLOC});
    for (size_t l = 0; l < lanes; ++l)
    {
        skins[l].assign(maxJoints, mat4(1.0f));
        morphs[l].resize(maxVerts);
        morphWeights[l].resize(maxTargets);
        morphAccum[l].resize(maxTargets);
    }

    // Bucket clips by the skeleton they drive so the per-skeleton pose pass is independent, and
    // group them into named animation sets (skin + rigid clips of one glTF animation share a name).
    skeletonClips.assign(skeletons.size(), Vector<uint32_t>{GLOBAL_ALLOC});
    clipAnimation.assign(scene.mTables.clips.size(), -1);
    auto findOrAddSet = [&](FUUID name) -> uint32_t
    {
        // Named clips merge into one set; unnamed clips each get their own so unrelated ones stay
        // distinct (the importer assigns fallback names, so this is a rare safety net).
        if (!name.IsNil())
            for (uint32_t i = 0; i < animations.size(); ++i)
                if (animations[i].name == name)
                    return i;
        animations.push_back(FAnimationSet{GLOBAL_ALLOC});
        animations.back().name = name;
        return static_cast<uint32_t>(animations.size() - 1);
    };
    for (uint32_t ci = 0; ci < scene.mTables.clips.size(); ++ci)
    {
        FAnimationClip const& clip = scene.mTables.clips[ci];
        int32_t const sk = skeletonIndex(clip.skeleton);
        if (sk >= 0 && static_cast<size_t>(sk) < skeletons.size())
            skeletonClips[static_cast<size_t>(sk)].push_back(ci);

        uint32_t const set = findOrAddSet(clip.name);
        clipAnimation[ci] = static_cast<int32_t>(set);
        FAnimationSet& as = animations[set];
        as.clips.push_back(ci);
        as.duration = std::max(as.duration, clip.duration);
        as.channelCount += static_cast<uint32_t>(clip.channels.size() + clip.morphChannels.size());
        if (sk >= 0 && sk == sceneNodeSkeleton)
            as.hasRigid = true;
        else if (sk >= 0)
            as.hasSkin = true;
    }

    // Per-set affected scene instances (instances on rigidly animated nodes + their descendants, and
    // instances whose mesh is bound to a skin skeleton the set drives), for the editor highlight/list.
    uint32_t const instanceCount = static_cast<uint32_t>(scene.mTables.instances.size());
    for (FAnimationSet& set : animations)
    {
        Vector<uint8_t> rigidNodes(GLOBAL_ALLOC);
        Vector<int32_t> skinSkels(GLOBAL_ALLOC);
        Vector<FUUID> morphMeshes(GLOBAL_ALLOC); // meshes this set drives via morph channels
        bool setDrivesRigid = false;
        if (sceneNodeSkeleton >= 0)
            rigidNodes.assign(skeletons[static_cast<size_t>(sceneNodeSkeleton)].Count(), 0u);
        for (uint32_t ci : set.clips)
        {
            FAnimationClip const& clip = scene.mTables.clips[ci];
            for (FMorphChannel const& mc : clip.morphChannels)
                if (!mc.mesh.IsNil() && std::find(morphMeshes.begin(), morphMeshes.end(), mc.mesh) == morphMeshes.end())
                    morphMeshes.push_back(mc.mesh);
            int32_t const sk = skeletonIndex(clip.skeleton);
            if (sk < 0)
                continue;
            if (sk == sceneNodeSkeleton)
            {
                setDrivesRigid = true;
                for (FAnimChannel const& channel : clip.channels)
                    if (channel.joint < rigidNodes.size())
                        rigidNodes[channel.joint] = 1u;
            }
            else if (std::find(skinSkels.begin(), skinSkels.end(), sk) == skinSkels.end())
                skinSkels.push_back(sk);
        }
        if (setDrivesRigid) // topological storage: one forward pass propagates to descendants
        {
            FSkeleton const& nodes = skeletons[static_cast<size_t>(sceneNodeSkeleton)];
            for (uint32_t i = 0; i < nodes.Count(); ++i)
            {
                int32_t const parent = nodes.joints[i].parent;
                if (parent >= 0 && rigidNodes[static_cast<uint32_t>(parent)])
                    rigidNodes[i] = 1u;
            }
        }
        set.drivesCamera = setDrivesRigid && cameraNode >= 0 &&
                           static_cast<size_t>(cameraNode) < rigidNodes.size() && rigidNodes[cameraNode];
        for (uint32_t ii = 0; ii < instanceCount; ++ii)
        {
            FInstance const& inst = scene.mTables.instances[ii];
            bool affected = setDrivesRigid && inst.node >= 0 &&
                            static_cast<size_t>(inst.node) < rigidNodes.size() && rigidNodes[inst.node];
            if (!affected && !skinSkels.empty() && inst.type == FInstanceType::Mesh)
            {
                int const meshIdx = scene.MeshIndex(inst.resource);
                if (meshIdx >= 0)
                {
                    int32_t const msk = skeletonIndex(scene.mTables.meshes[static_cast<size_t>(meshIdx)].skeleton);
                    affected = msk >= 0 && std::find(skinSkels.begin(), skinSkels.end(), msk) != skinSkels.end();
                }
            }
            if (!affected && !morphMeshes.empty() && inst.type == FInstanceType::Mesh)
                affected = std::find(morphMeshes.begin(), morphMeshes.end(), inst.resource) != morphMeshes.end();
            if (affected)
                set.instances.push_back(ii);
        }
    }

    for (FAnimationClip const& clip : scene.mTables.clips)
        fullDuration = std::max(fullDuration, clip.duration);
    // Name -> set index, so an NLA strip's `source` resolves to its clip group at runtime.
    setByName.clear();
    for (uint32_t i = 0; i < animations.size(); ++i)
        if (!animations[i].name.IsNil())
            setByName.emplace(animations[i].name, i);
    // Derive the timeline length + animated-instance highlight from the imported NLA tracks.
    animatedMask.assign(instanceCount, 0u);
    RefreshAnimatedInstances(scene);
    // Skinned-only imports (no clips) should start paused: there's deformable data, but no
    // time-varying source to advance.
    playing = !scene.mTables.clips.empty();

    LOG(Editor, LogInfo,
        "Animation runtime: {} deforming mesh(es), {} skeleton(s), {} clip(s), {} animation(s), {} NLA track(s)",
        meshes.size(), skeletons.size(), scene.mTables.clips.size(), animations.size(),
        scene.mTables.nlaTracks.size());
}

void FAnimationRuntime::RefreshAnimatedInstances(FImportedScene const& scene)
{
    std::fill(animatedMask.begin(), animatedMask.end(), uint8_t{0});
    animatedList.clear();
    duration = 0.0f;
    drivesCamera = false;
    for (FNlaTrack const& track : scene.mTables.nlaTracks)
    {
        if (track.mute)
            continue;
        for (FNlaStrip const& strip : track.strips)
        {
            duration = std::max(duration, strip.stripEnd);
            auto it = setByName.find(strip.source);
            if (it == setByName.end())
                continue;
            FAnimationSet const& set = animations[it->second];
            if (set.drivesCamera && strip.influence > 0.0f)
                drivesCamera = true;
            for (uint32_t inst : set.instances)
                if (inst < animatedMask.size() && !animatedMask[inst])
                {
                    animatedMask[inst] = 1u;
                    animatedList.push_back(inst);
                }
        }
    }
    // No NLA tracks (e.g. a morph-only import) fall back to the whole timeline.
    if (scene.mTables.nlaTracks.empty())
        duration = fullDuration;
}

// Builds a small dependency graph (pose -> rigid/lights/cameras, pose -> begin-dynamic -> deform
// -> end-dynamic, all joined by a "done" barrier) and submits it, so the caller can do independent
// per-frame work while the pose pass runs on the pool. End() waits on the done barrier - pumping
// the graph's main-thread nodes (the dynamic ring window open/close, the lights/cameras apply) on
// the calling thread - before the caller commits the scene.
void FAnimationRuntime::Begin(FImportedScene& scene, GPUScene* gpu, float dt, ThreadPool& jobs, Allocator* frameScratch,
                             ExecutionPolicy policy)
{
    frameActive = false;
    frameChanged = false;
    frameDone = JobHandle{};
    frameGraph.reset(); // drop the previous frame's graph (drains it if a caller skipped End)
    if (!HasData())
        return;
    bool const doDynamic = gpu && !meshes.empty() && gpu->HasDynamicGeometry();
    bool const doRigid = HasRigid();
    if (!doDynamic && !doRigid)
        return;
    // Paused and not scrubbed: skip so a held pose lets the path tracer keep converging.
    if (!playing && !dirty)
        return;
    ZoneScoped;

    if (playing)
    {
        // Advance the master clock (scaled by `speed`); individual clips/tracks shorter than
        // `duration` already wrap via their own fmod in the pose/deform passes below.
        time += dt * speed;
        if (duration > 0.0f)
        {
            if (loop)
                time = std::fmod(time, duration);
            else if (time >= duration)
            {
                time = duration; // hold the final pose and stop
                playing = false;
            }
        }
    }
    dirty = false;
    frameActive = true;

    auto const& skeletons = scene.mTables.skeletons;

    frameGraph = ConstructUnique<JobGraph>(frameScratch, jobs, frameScratch);
    JobGraph& graph = *frameGraph;
    JobHandle const done = graph.AddBarrier("Animation Done");
    frameDone = done;

    // Pose: evaluate every skeleton's pose into world matrices. Each skeleton writes its own FPose
    // and samples only its pre-bucketed clips, so the skeletons are independent and run in parallel.
    // With no skeletons it degenerates to a no-op source the rest of the graph can hang off of.
    JobHandle const pose = skeletons.empty()
        ? graph.AddBarrier("Anim Pose")
        : graph.AddParallelFor("Anim Pose", policy, skeletons.size(),
            [this, &scene](size_t s)
            {
                ZoneScopedN("Anim Pose");
                auto const& skels = scene.mTables.skeletons;
                ResetToRest(skels[s], poses[s]);
                // NLA: walk tracks low->high, blending each active strip into the running pose by
                // its influence (per-channel lerp/slerp, so stacked strips on shared joints
                // accumulate instead of the top track clobbering the rest). Only clips driving this
                // skeleton contribute; morph-only clips fall out here (no channels for skeleton s).
                ForEachActiveStrip(scene, *this, [&](uint32_t setIdx, float cl, float influence)
                {
                    FAnimationSet const& set = animations[setIdx];
                    for (uint32_t clipIdx : set.clips)
                    {
                        if (clipAnimation[clipIdx] != static_cast<int32_t>(setIdx))
                            continue;
                        if (skeletonClips[s].empty() ||
                            std::find(skeletonClips[s].begin(), skeletonClips[s].end(), clipIdx) ==
                                skeletonClips[s].end())
                            continue; // clip drives a different skeleton
                        BlendClip(scene.mTables.clips[clipIdx], cl, influence, poses[s]);
                    }
                });
                ComputeGlobals(skels[s], poses[s]);
            });

    // Rigid node animation: write animated nodes' world transforms onto their instances/lights/
    // cameras. The instance pass (potentially large) fans out across the pool; lights and cameras
    // are few and run as main-thread nodes, concurrently (disjoint writes) with the instance jobs.
    if (doRigid)
    {
        auto& instances = scene.mTables.instances;
        JobHandle const rigid = graph.AddParallelFor("Anim Rigid", policy, instances.begin(), instances.end(),
            [this](FInstance& instance) { ApplyAnimatedNode(*this, instance.node, instance.transform); });
        JobHandle const lights = graph.AddMain("Anim Lights",
            [this, &scene]
            {
                for (FLight& light : scene.mTables.lights)
                    ApplyAnimatedNode(*this, light.node, light.transform);
            });
        // GetCameraTransform reads this pass's result; no GPUScene commit is needed for a camera
        // move, so it doesn't touch frameChanged.
        JobHandle const cameras = graph.AddMain("Anim Cameras",
            [this, &scene]
            {
                for (FCamera& camera : scene.mTables.cameras)
                    ApplyAnimatedNode(*this, camera.node, camera.transform);
            });
        graph.DependsOn(rigid, pose);
        graph.DependsOn(lights, pose);
        graph.DependsOn(cameras, pose);
        graph.DependsOn(done, rigid, lights, cameras);
        frameChanged |= rigidDrivesTransforms;
    }

    // CPU deformation into the dynamic ring. The window opens on a main-thread node (advances the
    // slot), the per-mesh skinning fans out across the pool, then the window closes on a
    // main-thread node once the skinning completes - strictly ordered begin -> deform -> end.
    if (doDynamic)
    {
        JobHandle const beginDynamic =
            graph.AddMain("Begin Dynamic Geometry", [gpu] { gpu->BeginDynamicGeometryUpdate(); });
        JobHandle const deform =
            graph.AddParallelFor("Anim Deform", policy, meshes.begin(), meshes.end(),
                [this, &scene, gpu](FDynamicMeshRuntime const& rt, size_t worker)
                { DeformMesh(*this, &scene, gpu, rt, worker); });
        JobHandle const endDynamic =
            graph.AddMain("End Dynamic Geometry", [gpu] { gpu->EndDynamicGeometryUpdate(); });
        graph.DependsOn(beginDynamic, pose);
        graph.DependsOn(deform, beginDynamic);
        graph.DependsOn(endDynamic, deform);
        graph.DependsOn(done, endDynamic);
        frameChanged = true;
    }

    graph.Submit();
}

bool FAnimationRuntime::End()
{
    if (!frameActive)
        return false;
    ZoneScoped;
    CHECK(frameGraph);
    frameGraph->Wait(frameDone);
    frameGraph.reset();
    return frameChanged;
}

bool FAnimationRuntime::GetCameraTransform(FImportedScene const& scene, FTransform& outTransform) const
{
    if (cameraNode < 0)
        return false;
    auto cameras = scene.GetCameras();
    if (cameras.empty())
        return false;
    outTransform = cameras.front().transform;
    return true;
}
