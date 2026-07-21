#pragma once
#include <Renderer/Animation.hpp>
#include <Renderer/Renderer.hpp>
#include "GPUScene.hpp"

struct FAnimationPlayback
{
    uint32_t skeletonIndex{UINT32_MAX};
    uint32_t clipIndex{UINT32_MAX};
    float time{0.0f};
    float speed{1.0f};
    bool playing{false};
    bool loop{true};
    bool dirty{true};
};

struct FAnimationRuntimeBudget
{
    size_t inputBytes{0};
    size_t paletteBytes{0};
    [[nodiscard]] size_t TotalBytes() const { return inputBytes * 2u + paletteBytes; }
};

[[nodiscard]] FAnimationRuntimeBudget CalculateAnimationRuntimeBudget(FImportedScene const& scene);

class FAnimationRuntime
{
public:
    explicit FAnimationRuntime(Allocator* alloc = GLOBAL_ALLOC);

    void Initialize(FImportedScene& scene, GPUScene& gpu, RHIDevice* device, FSceneGPUResources& resources);
    void Reset(FSceneGPUResources* resources = nullptr);
    void RemoveInstance(uint32_t instanceIndex, FSceneGPUResources& resources);
    [[nodiscard]] bool Tick(float dt, uint64_t frame);
    void BuildGraph(Renderer* renderer, ResourceHandle dynamicPrimitiveBuffer);

    [[nodiscard]] bool HasSkinning() const { return !mOutputs.empty(); }
    [[nodiscard]] Span<FAnimationPlayback> GetPlaybacks() { return {mPlaybacks.data(), mPlaybacks.size()}; }
    [[nodiscard]] Span<const FAnimationPlayback> GetPlaybacks() const
    {
        return {mPlaybacks.data(), mPlaybacks.size()};
    }
    [[nodiscard]] Span<const uint32_t> GetSkeletonClips(uint32_t skeletonIndex) const;

    static void BuildGraphCallback(void* context, Renderer* renderer, ResourceHandle dynamicPrimitiveBuffer);

private:
    static constexpr uint32_t kPaletteFrames = 4u;

    struct MeshInput
    {
        uint32_t bindVertexOffset{0};
        uint32_t skinBindingOffset{0};
        uint32_t indexOffset{0};
        uint32_t vertexCount{0};
        uint32_t indexCount{0};
        uint32_t skeletonIndex{UINT32_MAX};
        bool valid{false};
    };
    struct Output
    {
        uint32_t instanceIndex{0};
        uint32_t meshIndex{0};
        GeometryHandle geometry{};
        bool seedIndices{true};
    };
    struct Dispatch
    {
        GeometryHandle geometry{};
        uint32_t bindVertexOffset{0};
        uint32_t skinBindingOffset{0};
        uint32_t indexSourceOffset{0};
        uint32_t vertexDestinationOffset{0};
        uint32_t indexDestinationOffset{0};
        uint32_t paletteOffset{0};
        uint32_t vertexCount{0};
        uint32_t indexCount{0};
        uint32_t jointCount{0};
        bool seedIndices{false};
    };

    Allocator* mAllocator{GLOBAL_ALLOC};
    FImportedScene* mScene{nullptr};
    GPUScene* mGPU{nullptr};
    RHIDevice* mDevice{nullptr};
    Vector<MeshInput> mMeshes;
    Vector<Output> mOutputs;
    Vector<FAnimationPlayback> mPlaybacks;
    Vector<Vector<uint32_t>> mSkeletonClips;
    Vector<FPose> mPoses;
    Vector<uint32_t> mPaletteSkeletonOffsets;
    Vector<Dispatch> mDispatches;
    RHIDeviceScopedHandle<RHIBuffer> mInputBuffer;
    RHIDeviceScopedHandle<RHIBuffer> mInputStagingBuffer;
    RHIDeviceScopedHandle<RHIBuffer> mPaletteBuffer;
    mat4* mPaletteMapped{nullptr};
    uint32_t mInputSize{0};
    uint32_t mPaletteFrameMatrices{0};
    bool mInputUploadPending{false};
    bool mForceUpdate{false};
};
