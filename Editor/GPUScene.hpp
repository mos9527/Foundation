#pragma once
#include <RenderCore/Bindless.hpp>
#include <RenderCore/ImmediateContext.hpp>
#include "Context.hpp"
#include "Mesh.hpp"
#include "Texture.hpp"
using namespace Math;

BITMASK_ENUM_BEGIN(GSData, uint8_t)
Mesh = 1 << 0,
    BITMASK_ENUM_END()

#pragma pack(push, 1)
        struct GSMesh
{
    // Offsets are absolute, and are in Primitive buffer (bytes).
    // @ref FVertex
    uint32_t vtxOffset;
    uint32_t vtxCount;
    // -- DAG LOD Group @ref FLODGroup
    uint32_t groupOffset;
    uint32_t groupCount;
    // -- DAG Meshlets @ref FMeshlet
    uint32_t meshletOffset;
    uint32_t meshletCount;
    uint32_t meshletVtxOffset;
    uint32_t meshletTriOffset;
    // -- Global index (amongst all loaded meshes)
    uint32_t meshletGlobalIndex;
};
struct GSInstance
{
    // TRS
    float3 transform{0, 0, 0};
    quat rotation{0, 0, 0, 1};
    float3 scale{1, 1, 1};
    uint32_t meshOffset; // In Primitive buffer (bytes)
    uint32_t materialIndex; // In Material buffer (offset)
};
struct GSMaterial
{
    uint32_t baseColorTexture;
    uint32_t emissiveTexture;
    uint32_t metallicRoughnessTexture;
    uint32_t normalTexture;
    float4 baseColorFactor;
    float3 emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
};
#pragma pack(pop)
static_assert(sizeof(GSMesh) == 36);
static_assert(sizeof(GSInstance) == 48);

template <typename T>
struct GPURingBuffer
{
    RHIDeviceScopedHandle<RHIBuffer> mBuffer;
    T *mBegin, *mPrevRing, *mRing, *mEnd;

    GPURingBuffer(RHIDevice* device, size_t budget)
    {
        mBuffer = device->CreateBuffer({.resource = {.heap = RHIDeviceHeapType::Upload,
                                                     .hostAccess = RHIResourceHostAccess::WriteOnly,
                                                     .coherent = true},
                                        .usage = RHIBufferUsageBits::StorageBuffer,
                                        .size = budget * sizeof(T)});
        mBegin = mRing = mPrevRing = mBuffer->Map<T>();
        mEnd = mBegin + budget;
    }
    /**
     * @breif Allocates `count` T elements in the ring buffer, returning a Span to the allocated memory.
     *        It's up to the caller to fill in the data, which is usually write-only.
     * @note There's no guard against allocating potentially still in-flight memory range.
     *       Ensure enough memory budget to avoid overwriting.
     * @return Raw mapped memory ptr, offset (element wise) in buffer.
     */
    Pair<T*, uint32_t> Allocate(uint32_t count)
    {
        T* begin = mRing;
        if (begin + count >= mEnd) // Wrap
            begin = mRing = mBegin;
        uint32_t offset = mRing - mBegin;
        mPrevRing = mRing, mRing += count;
        return {begin, offset};
    }
    void Reset() { mRing = mBegin; }
    [[nodiscard]] uint32_t Used() const { return mRing - mPrevRing; }
    [[nodiscard]] uint32_t Capacity() const { return mEnd - mBegin; }
};
/**
 * @brief Async GPU scene data storage for Editor.
 */
class GPUScene
{
    FContext* mContext;

    RHIDeviceScopedHandle<RHIBuffer> mPrimitiveBuffer;
    // XXX: Linear allocation. GPA would be needed if we'd upload & free
    //      at will. Not needed for Editor use-case currently.
    size_t mPrimitiveOffset{0};

    // For @ref meshletGlobalIndex
    uint32_t mMeshletGlobalCounter{0};

    GPURingBuffer<GSInstance> mInstanceBuffer;
    GPURingBuffer<GSMaterial> mMaterialBuffer;

    BindlessPool mTexturePool;

public:
    struct GPUSceneDesc
    {
        uint32_t primitiveBudget = 16 * (1u << 20); // 16MB
        uint32_t instanceBudget = static_cast<uint32_t>(1e4); // # of instances
        uint32_t materialBudget = static_cast<uint32_t>(1e3); // # of materials
        uint32_t texturesBudget = static_cast<uint32_t>(1e3); // # of textures
    };
    GPUScene(FContext* ctx, GPUSceneDesc const& desc);

    Pair<GSInstance*, uint32_t> AllocateInstance(uint32_t count);
    Pair<GSMaterial*, uint32_t> AllocateMaterial(uint32_t count);

    [[nodiscard]] String DbgGetBufferStatistics() const;

    size_t Upload(ImmediateUpload* ctx, FMesh const& source, GSMesh& outData, uint32_t& outOffset);
    size_t Upload(ImmediateUpload* ctx, FTexture2D const& source, uint32_t& outIndex);

    [[nodiscard]] RHIBuffer* GetPrimitiveBuffer() const { return mPrimitiveBuffer.Get(); }
    [[nodiscard]] RHIBuffer* GetInstanceBuffer() const { return mInstanceBuffer.mBuffer.Get(); }
    [[nodiscard]] RHIBuffer* GetMaterialBuffer() const { return mMaterialBuffer.mBuffer.Get(); }
    [[nodiscard]] BindlessPool* GetTexturePool() { return &mTexturePool; }
    void Reset();
};
