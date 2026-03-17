#pragma once
#include <RenderCore/Bindless.hpp>
#include <RenderCore/ImmediateContext.hpp>
#include "Context.hpp"
#include "Mesh.hpp"
#include "Texture.hpp"
using namespace Math;

BITMASK_ENUM_BEGIN(GSData, uint8_t)
    Mesh = 1 << 0
BITMASK_ENUM_END()

#pragma pack(push, 1)
struct GSMesh
{
    // Offsets are absolute, and are in Primitive buffer (bytes).
    // @ref FVertex
    uint32_t vtxOffset;
    uint32_t vtxCount;
    // LOD0 UINT32 indices
    uint32_t idxOffset;
    uint32_t idxCount;
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
    uint32_t meshIndex; // Debug use
};
struct GSMaterial
{
    uint32_t baseColorTexture = UINT32_MAX;
    uint32_t emissiveTexture = UINT32_MAX;
    uint32_t metallicRoughnessTexture = UINT32_MAX;
    uint32_t normalTexture = UINT32_MAX;
    float4 baseColorFactor;
    float3 emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
    float transmissionFactor;
    float anisotropy;
};
#pragma pack(pop)
static_assert(sizeof(GSMesh) == 44);
static_assert(sizeof(GSInstance) == 52);

template <typename T>
struct UploadGPURingBuffer
{
    RHIDeviceScopedHandle<RHIBuffer> mBuffer;
    T *mBegin, *mPrevRing, *mRing, *mEnd;

    UploadGPURingBuffer(RHIDevice* device, size_t budget)
    {
        mBuffer = device->CreateBuffer({.resource = {.heap = RHIDeviceHeapType::Upload,
                                                     .hostAccess = RHIResourceHostAccess::WriteOnly,
                                                     .coherent = true},
                                        .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress | RHIBufferUsageBits::AccelerationStructureBuildReadOnly,
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
    /* Geometry */
    RHIDeviceScopedHandle<RHIBuffer> mPrimitiveBuffer;
    // XXX: Linear allocation. GPA would be needed if we'd upload & free
    //      at will. Not needed for Editor use-case currently.
    size_t mPrimitiveOffset{0};
    // For @ref meshletGlobalIndex
    uint32_t mMeshletGlobalCounter{0};
    UploadGPURingBuffer<GSInstance> mInstanceBuffer;
    UploadGPURingBuffer<GSMaterial> mMaterialBuffer;
    /* Textures */
    BindlessPool mTexturePool;
    // Precomputed LUTs (stored in texture pool)
    uint32_t mGGXlutEIndex{UINT32_MAX}, mGGXlutEavgIndex{UINT32_MAX};
    // Environment map (stored in texture pool)
    uint32_t mEnvMapIndex{UINT32_MAX};
    /* AS */
    // BLAS
    Vector<RHIDeviceScopedHandle<RHIAccelerationStructure>> mBLASes;
    Vector<RHIDeviceScopedHandle<RHIBuffer>> mBLASBuffers;
    size_t blasOffset{0};
    // TLAS
    uint32_t mTLASInstanceStride{0}; // In bytes, read only once
    RHIDeviceScopedHandle<RHIBuffer> mTLASBuffer, mScratchBufferTLAS;
    RHIDeviceScopedHandle<RHIAccelerationStructure> mTLAS;
    UploadGPURingBuffer<char> mTLASInstances;
public:
    struct GPUSceneDesc
    {
        uint32_t primitiveBudget = 16 * (1u << 20); // 16MB
        uint32_t instanceBudget = static_cast<uint32_t>(1e4); // # of instances (ring)
        uint32_t materialBudget = static_cast<uint32_t>(1e3); // # of materials (ring)
        uint32_t texturesBudget = static_cast<uint32_t>(1e3); // # of textures
        uint32_t blasBudget = 64 * (1u << 20); // 64MB
        uint32_t tlasBudget = 16 * (1u << 20); // 16MB
        uint32_t tlasScratchBudget = 32 * (1u << 20); // 32MB (ring)
    };
    GPUScene(FContext* ctx, GPUSceneDesc const& desc);

    Pair<GSInstance*, uint32_t> AllocateInstance(uint32_t count);
    Pair<GSMaterial*, uint32_t> AllocateMaterial(uint32_t count);

    [[nodiscard]] String DbgGetBufferStatistics() const;

    size_t Upload(ImmediateUpload* ctx, FMesh const& source, GSMesh& outData, uint32_t& outOffset);
    size_t Upload(ImmediateUpload* ctx, FTexture2D const& source, uint32_t& outIndex);

    void BuildBLAS(ImmediateContext* ctx, Span<const GSMesh> meshes, Span<uint32_t> outBLASIndices);
    void BuildTLAS(RHICommandList* cmd, Span<const GSInstance> instances, Span<const uint32_t> blasIndices, bool update = false);

    /* Geometry */
    [[nodiscard]] RHIBuffer* GetPrimitiveBuffer() const { return mPrimitiveBuffer.Get(); }
    [[nodiscard]] RHIBuffer* GetInstanceBuffer() const { return mInstanceBuffer.mBuffer.Get(); }
    [[nodiscard]] RHIBuffer* GetMaterialBuffer() const { return mMaterialBuffer.mBuffer.Get(); }
    /* Textures */
    [[nodiscard]] BindlessPool* GetTexturePool() { return &mTexturePool; }
    [[nodiscard]] RHITexture* GetGGXlutE() const;
    [[nodiscard]] RHITexture* GetGGXlutEavg() const;
    // Environment map
    void UploadEnvMap(ImmediateUpload* ctx, FTexture2D const& source);
    [[nodiscard]] RHITexture* GetEnvMap() const;
    /* AS */
    [[nodiscard]] RHIAccelerationStructure* GetTLAS() const { return mTLAS ? mTLAS.Get() : nullptr; }
    void Reset();
};
