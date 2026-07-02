#pragma once
#include "GPUScene.hpp"
#include <Core/JobGraph.hpp>
#include <limits>

// One deforming mesh's CPU-resident rest pose + skin/morph source data, dequantized once by
// FAnimationRuntime::Setup and re-skinned/morphed into the GPUScene dynamic ring every active frame.
struct FDynamicMeshRuntime
{
    GeometryHandle handle{};
    size_t meshIndex{std::numeric_limits<size_t>::max()};
    Vector<FVertex> bind;
    int32_t skeleton{-1};
    Vector<FSkinBinding> binding;
    int32_t morphTrack{-1};
    uint32_t morphTargetCount{0};
    Vector<float3> morphDeltas; // POSITION deltas, target-major (t*vtxCount + v)
    Vector<uint32_t> indices; // LOD0 topology, for recomputing normals after morphing
    explicit FDynamicMeshRuntime(Allocator* alloc = GLOBAL_ALLOC)
        : bind(alloc), binding(alloc), morphDeltas(alloc), indices(alloc)
    {
    }
};

// A named group of clips coming from one source glTF animation. A single glTF animation can split
// into a skin clip plus a rigid (scene-node) clip; they share a name and so land in the same set,
// letting the UI stat them as one animation. Built by FAnimationRuntime::Setup; NLA strips on
// `FSceneTables::nlaTracks` reference these sets by name.
struct FAnimationSet
{
    FUUID name{};                            // interned name id (resolve via FImportedScene::GetName)
    Vector<uint32_t> clips{GLOBAL_ALLOC};    // indices into FSceneTables::clips
    Vector<uint32_t> instances{GLOBAL_ALLOC}; // scene instances this set animates (skin or rigid)
    float duration{0.0f};                    // longest clip in the set, i.e. its natural length (seconds)
    uint32_t channelCount{0};                // total animated TRS channels across the set's clips
    bool hasSkin{false};                     // drives a skin skeleton (deforming mesh)
    bool hasRigid{false};                    // drives the scene-node (rigid articulation) skeleton
    bool drivesCamera{false};                // animates the scene's first camera (view) node
    explicit FAnimationSet(Allocator* alloc = GLOBAL_ALLOC) : clips(alloc), instances(alloc) {}
};

// Portable CPU skinning/morph/rigid-node animation runtime for one FImportedScene. Owns no
// GPUScene/Editor state directly: Begin() takes an optional GPUScene* and only touches dynamic
// geometry when one is given, so this is equally usable headless or from a non-editor viewer.
// Rebuild with Setup() whenever the scene (or its resident mesh handles) changes.
struct FAnimationRuntime
{
    Vector<FDynamicMeshRuntime> meshes{GLOBAL_ALLOC};
    Vector<FPose> poses{GLOBAL_ALLOC}; // one per scene skeleton
    Vector<Vector<uint32_t>> skeletonClips{GLOBAL_ALLOC}; // clip indices, bucketed per skeleton
    Vector<FAnimationSet> animations{GLOBAL_ALLOC}; // clips grouped by source glTF animation
    Vector<int32_t> clipAnimation{GLOBAL_ALLOC}; // scene clip index -> animation set index
    HashMap<FUUID, uint32_t> setByName{GLOBAL_ALLOC}; // animation-group name id -> set index
    Vector<uint8_t> animatedMask{GLOBAL_ALLOC}; // per scene-instance: driven by an active NLA strip
    Vector<uint32_t> animatedList{GLOBAL_ALLOC}; // instances set in animatedMask, for UI listing
    bool drivesCamera{false}; // some strip on a non-mute track references a camera-driving set
    // Per-worker scratch (outer index == ParallelFor workerId), pre-sized by Setup().
    Vector<Vector<mat4>> skins{GLOBAL_ALLOC};
    Vector<Vector<FVertex>> morphs{GLOBAL_ALLOC};
    Vector<Vector<float>> morphWeights{GLOBAL_ALLOC};
    int32_t sceneNodeSkeleton{-1};
    Vector<uint8_t> nodeAffected{GLOBAL_ALLOC};
    bool rigidDrivesTransforms{false};
    int32_t cameraNode{-1}; // scene-node index of the first camera, if it's animated
    float time{0.0f};
    float duration{0.0f};   // NLA timeline length: max stripEnd across non-mute tracks (or fullDuration)
    float fullDuration{0.0f}; // longest of all clips/morph tracks (used when there are no NLA tracks)
    float speed{1.0f};      // playback rate multiplier
    bool playing{true};
    bool loop{true};        // wrap at `duration`; otherwise clamp and pause at the end
    bool dirty{false}; // a scrub requested a one-shot pose apply even while paused

    // Per-frame job state, valid between Begin() and End().
    UniquePtr<JobGraph> frameGraph;
    JobHandle frameDone;
    bool frameActive{false};
    bool frameChanged{false};

    [[nodiscard]] bool HasRigid() const { return sceneNodeSkeleton >= 0; }
    [[nodiscard]] bool HasData() const { return !meshes.empty() || HasRigid(); }

    // True when an active (non-mute, influence>0) strip actually drives the scene's first camera and
    // playback is currently advancing (playing or scrubbed). Without a live camera strip the view
    // stays user-controlled. Query before Begin(), which consumes the scrub `dirty` flag.
    [[nodiscard]] bool CameraDrivesView() const { return drivesCamera && cameraNode >= 0 && (playing || dirty); }

    // Rebuilds this runtime from `scene`'s skeletons/clips/morph tracks and the resident mesh
    // handles in `resources`. `lanes` sizes the per-worker skin/morph scratch (pass
    // ThreadPool::GetParallelForConcurrency()).
    void Setup(FImportedScene const& scene, FSceneGPUResources const& resources, size_t lanes);

    // Recomputes `duration` and the animated-instance highlight (`animatedMask`/`animatedList`) from
    // `scene.mTables.nlaTracks`. Call after the editor mutates NLA tracks/strips.
    void RefreshAnimatedInstances(FImportedScene const& scene);

    [[nodiscard]] bool IsInstanceAnimated(uint32_t index) const
    {
        return index < animatedMask.size() && animatedMask[index] != 0;
    }

    // Builds and submits this frame's animation JobGraph on `jobs` (non-blocking) and advances the
    // clock; pair with End(). No-op while paused/held or with nothing to update. `gpu` may be null
    // to evaluate rigid node transforms without touching any dynamic geometry.
    void Begin(FImportedScene& scene, GPUScene* gpu, float dt, ThreadPool& jobs, Allocator* frameScratch,
              ExecutionPolicy policy = ExecutionPolicy::Par);

    // Waits on this frame's animation graph. Returns true if it changed anything a GPUScene commit
    // must pick up (rigid transforms and/or dynamic geometry).
    bool End();

    // Fills `outTransform` with the scene's first camera's current transform if it's animated (as
    // of the last End()). Returns false, leaving `outTransform` unchanged, otherwise.
    bool GetCameraTransform(FImportedScene const& scene, FTransform& outTransform) const;
};
