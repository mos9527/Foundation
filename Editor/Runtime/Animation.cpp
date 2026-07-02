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
    // morphed surface (skinning, applied next, rotates those fresh normals correctly).
    if (rt.morphTrack >= 0 && rt.morphTargetCount > 0 &&
        static_cast<size_t>(rt.morphTrack) < scene->mTables.morphTracks.size())
    {
        FMorphTrack const& track = scene->mTables.morphTracks[rt.morphTrack];
        Vector<float>& weights = runtime.morphWeights[worker];
        Vector<FVertex>& morphed = runtime.morphs[worker];
        float t = track.duration > 0.0f ? std::fmod(runtime.time, track.duration) : 0.0f;
        SampleTrack(Span<const float>(track.times.data(), track.times.size()),
                    Span<const float>(track.values.data(), track.values.size()), rt.morphTargetCount,
                    track.interp, t, Span<float>(weights.data(), rt.morphTargetCount));
        for (size_t v = 0; v < n; ++v)
        {
            FVertex mv = rt.bind[v];
            for (uint32_t tt = 0; tt < rt.morphTargetCount; ++tt)
                mv.position += weights[tt] * rt.morphDeltas[static_cast<size_t>(tt) * n + v];
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

    // Joint-local indices stay indices; resolve skeleton/morph-track ids via scene index.
    auto skeletonIndex = [&](FUUID id) { return scene.SkeletonIndex(id); };
    auto morphTrackIndex = [&](FUUID id) { return scene.MorphTrackIndex(id); };

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
        int32_t const morphIdx = morphTrackIndex(mesh.morphTrack);
        bool const skinned = mesh.skinBinding.count != 0 && skelIdx >= 0;
        bool const morphed = morphIdx >= 0 && mesh.morphTargetCount > 0;
        if ((!skinned && !morphed) || m >= resources.meshGeometry.size())
            continue;
        FDynamicMeshRuntime rt(GLOBAL_ALLOC);
        rt.handle = resources.meshGeometry[m];
        rt.meshIndex = m;
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
            rt.morphTrack = morphIdx;
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
    for (size_t l = 0; l < lanes; ++l)
    {
        skins[l].assign(maxJoints, mat4(1.0f));
        morphs[l].resize(maxVerts);
        morphWeights[l].resize(maxTargets);
    }

    // Bucket clips by the skeleton they drive so the per-skeleton pose pass is independent.
    skeletonClips.assign(skeletons.size(), Vector<uint32_t>{GLOBAL_ALLOC});
    for (uint32_t ci = 0; ci < scene.mTables.clips.size(); ++ci)
    {
        int32_t const sk = skeletonIndex(scene.mTables.clips[ci].skeleton);
        if (sk >= 0 && static_cast<size_t>(sk) < skeletons.size())
            skeletonClips[static_cast<size_t>(sk)].push_back(ci);
    }

    for (FAnimationClip const& clip : scene.mTables.clips)
        duration = std::max(duration, clip.duration);
    for (FMorphTrack const& track : scene.mTables.morphTracks)
        duration = std::max(duration, track.duration);
    // Skinned-only imports (no clip/morph tracks) should start paused: there's deformable data,
    // but no time-varying source to advance.
    playing = !scene.mTables.clips.empty() || !scene.mTables.morphTracks.empty();

    LOG(Editor, LogInfo, "Animation runtime: {} deforming mesh(es), {} skeleton(s), {} clip(s), {} morph track(s)",
        meshes.size(), skeletons.size(), scene.mTables.clips.size(), scene.mTables.morphTracks.size());
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
        // Loop the master clock; individual clips/tracks shorter than `duration` already wrap via
        // their own fmod in the pose/deform passes below.
        time += dt;
        if (duration > 0.0f)
            time = std::fmod(time, duration);
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
                for (uint32_t clipIdx : skeletonClips[s])
                {
                    FAnimationClip const& clip = scene.mTables.clips[clipIdx];
                    float t = clip.duration > 0.0f ? std::fmod(time, clip.duration) : 0.0f;
                    SampleClip(clip, t, poses[s]);
                }
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
