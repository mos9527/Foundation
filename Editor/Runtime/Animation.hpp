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

// Portable CPU skinning/morph/rigid-node animation runtime for one FImportedScene. Owns no
// GPUScene/Editor state directly: Begin() takes an optional GPUScene* and only touches dynamic
// geometry when one is given, so this is equally usable headless or from a non-editor viewer.
// Rebuild with Setup() whenever the scene (or its resident mesh handles) changes.
struct FAnimationRuntime
{
    Vector<FDynamicMeshRuntime> meshes{GLOBAL_ALLOC};
    Vector<FPose> poses{GLOBAL_ALLOC}; // one per scene skeleton
    Vector<Vector<uint32_t>> skeletonClips{GLOBAL_ALLOC}; // clip indices, bucketed per skeleton
    // Per-worker scratch (outer index == ParallelFor workerId), pre-sized by Setup().
    Vector<Vector<mat4>> skins{GLOBAL_ALLOC};
    Vector<Vector<FVertex>> morphs{GLOBAL_ALLOC};
    Vector<Vector<float>> morphWeights{GLOBAL_ALLOC};
    int32_t sceneNodeSkeleton{-1};
    Vector<uint8_t> nodeAffected{GLOBAL_ALLOC};
    bool rigidDrivesTransforms{false};
    int32_t cameraNode{-1}; // scene-node index of the first camera, if it's animated
    float time{0.0f};
    float duration{0.0f};
    bool playing{true};
    bool dirty{false}; // a scrub requested a one-shot pose apply even while paused

    // Per-frame job state, valid between Begin() and End().
    UniquePtr<JobGraph> frameGraph;
    JobHandle frameDone;
    bool frameActive{false};
    bool frameChanged{false};

    [[nodiscard]] bool HasRigid() const { return sceneNodeSkeleton >= 0; }
    [[nodiscard]] bool HasData() const { return !meshes.empty() || HasRigid(); }

    // True when the scene's first camera is animated and currently advancing (playing or
    // scrubbed). Query before Begin(), which consumes the scrub `dirty` flag.
    [[nodiscard]] bool CameraDrivesView() const { return cameraNode >= 0 && (playing || dirty); }

    // Rebuilds this runtime from `scene`'s skeletons/clips/morph tracks and the resident mesh
    // handles in `resources`. `lanes` sizes the per-worker skin/morph scratch (pass
    // ThreadPool::GetParallelForConcurrency()).
    void Setup(FImportedScene const& scene, FSceneGPUResources const& resources, size_t lanes);

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
