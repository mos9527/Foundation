#pragma once
#include <RenderCore/Bindless.hpp>
#include <RenderCore/ImmediateContext.hpp>
#include "Precompute.hpp"
#include "Curve.hpp"
#include "Mesh.hpp"
#include "Texture.hpp"
using namespace Math;
using Foundation::RenderCore::BindlessPool;
using Foundation::RenderCore::ImmediateContext;
using Foundation::RenderCore::ImmediateSubmitDesc;
using Foundation::RenderCore::ImmediateUpload;
namespace Foundation::Core { class AllocatorStack; }

// Must match the procedural hit-group bindings in Render/Pathtracer.cpp.
inline constexpr uint32_t kRectLightSBTOffset = 3u;
inline constexpr uint32_t kDiskLightSBTOffset = 4u;
inline constexpr uint32_t kCurveSBTOffset = 5u;
inline constexpr uint32_t kGSInstanceTypeMesh = 0u;
inline constexpr uint32_t kGSInstanceTypeCurve = 1u;

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
    uint32_t resourceOffset; // Mesh or curve set offset in Primitive buffer (bytes)
    uint32_t materialIndex; // In Material buffer (offset)
    uint32_t resourceIndex; // Debug use
    uint32_t type{kGSInstanceTypeMesh};
};
struct GSCurveSet
{
    uint32_t pointOffset; // GSCurvePoint, in Primitive buffer (bytes)
    uint32_t pointCount;
    uint32_t segmentOffset; // GSCurveSegment, in Primitive buffer (bytes)
    uint32_t segmentCount;
    uint32_t aabbOffset; // RHIAccelerationStructureAABB, in curve AABB buffer (bytes)
    uint32_t materialIndex;
};
struct GSCurvePoint
{
    float3 position;
    float radius;
};
struct GSCurveSegment
{
    uint32_t p0;
    uint32_t p1;
    float u0;
    float u1;
};
struct GSMaterial
{
    uint32_t baseColorTexture = UINT32_MAX;
    uint32_t emissiveTexture = UINT32_MAX;
    uint32_t metallicRoughnessTexture = UINT32_MAX;
    uint32_t normalTexture = UINT32_MAX;
    uint32_t transmissionTexture = UINT32_MAX;
    uint32_t specularTexture = UINT32_MAX;
    uint32_t specularColorTexture = UINT32_MAX;
    uint32_t anisotropyTexture = UINT32_MAX;
    uint32_t sheenColorTexture = UINT32_MAX;
    uint32_t sheenRoughnessTexture = UINT32_MAX;
    uint32_t clearcoatTexture = UINT32_MAX;
    uint32_t clearcoatRoughnessTexture = UINT32_MAX;
    float4 baseColorFactor;
    float3 emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
    float transmissionFactor;
    float ior;
    float specularFactor;
    float3 specularColorFactor;
    float anisotropyStrength;
    float anisotropyRotation;
    float3 sheenColorFactor;
    float sheenRoughnessFactor;
    float clearcoatFactor;
    float clearcoatRoughnessFactor;
    float subsurfaceFactor;
    float subsurfaceScale;
    float3 subsurfaceColor;
    float3 subsurfaceRadius;
    uint32_t shaderBlockID;
    float hairBetaM;
    float hairBetaN;
    float hairAlpha;
};
struct GSLight
{
    uint32_t type{0u};       // 0=Directional, 1=Point, 2=Spot, 3=Disk, 4=Rect
    float3 color{1,1,1};    // Normalized RGB color
    float power{1.0f};      // Radiant power (type-dependent unit)
    float3 position{0,0,0};
    float3 direction{0,0,-1};
    float range{0.0f};
    float spotInnerCosAngle{1.0f};
    float spotOuterCosAngle{0.7071f};
    // Area lights (Disk / Rect)
    float3 dpdu{1,0,0};     // tangent u-axis (Rect: half-extent u)
    float3 dpdv{0,1,0};     // tangent v-axis (Rect: half-extent v)
    float2 radius{0.5f, 0.5f}; // Disk radius (x, y for ellipse)
    uint32_t twoSided{0u};
    float selectionWeight{0.0f};
};
#pragma pack(pop)
static_assert(sizeof(GSMesh) == 44);
static_assert(sizeof(GSInstance) == 56);
static_assert(sizeof(GSCurveSet) == 24);
static_assert(sizeof(GSCurvePoint) == 16);
static_assert(sizeof(GSCurveSegment) == 16);
static_assert(sizeof(GSMaterial) == 188);
static_assert(sizeof(GSLight) == 96);

struct GPUSceneDesc
{
    uint32_t primitiveBudget = 16 * (1u << 20); // 16MB
    uint32_t curveAABBBudget = 16 * (1u << 20); // 16MB
    uint32_t instanceBudget = static_cast<uint32_t>(1e4); // # of GSInstance elements (ring)
    uint32_t materialBudget = static_cast<uint32_t>(1e3); // # of materials (ring)
    uint32_t lightBudget = static_cast<uint32_t>(1e4); // # of lights (ring)
    uint32_t texturesBudget = static_cast<uint32_t>(1e3); // # of textures
    uint32_t tlasInstanceBudget = static_cast<uint32_t>(1e4); // # of TLAS instances (ring)
    uint32_t tlasBudget = 16 * (1u << 20); // 16MB
    uint32_t tlasScratchBudget = 32 * (1u << 20); // 32MB (ring)
};

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
        CHECK_MSG(count <= Capacity(), "GPU upload ring allocation overflow: requested {} elements, capacity {}", count, Capacity());
        T* begin = mRing;
        if (static_cast<size_t>(mEnd - begin) < count) // Wrap
            begin = mRing = mBegin;
        uint32_t offset = static_cast<uint32_t>(begin - mBegin);
        mPrevRing = begin, mRing = begin + count;
        return {begin, offset};
    }
    void Reset() { mRing = mBegin; }
    [[nodiscard]] uint32_t Used() const { return mRing - mPrevRing; }
    [[nodiscard]] uint32_t Capacity() const { return static_cast<uint32_t>(mEnd - mBegin); }
};
/**
 * @brief Async GPU scene data storage for Editor.
 */
class GPUScene
{
    RHIDevice* mDevice{nullptr};
    Allocator* mAllocator{GLOBAL_ALLOC};
    AllocatorStack* mFrameScratch{nullptr};
    /* Geometry */
    RHIDeviceScopedHandle<RHIBuffer> mPrimitiveBuffer;
    char* mPrimitiveMapped{nullptr};
    bool mDirectGeometryUpload{false};
    // XXX: Linear allocation. GPA would be needed if we'd upload & free
    //      at will. Not needed for Editor use-case currently.
    size_t mPrimitiveOffset{0};
    // For @ref meshletGlobalIndex
    uint32_t mMeshletGlobalCounter{0};
    UploadGPURingBuffer<GSInstance> mInstanceBuffer;
    UploadGPURingBuffer<GSMaterial> mMaterialBuffer;
    UploadGPURingBuffer<GSLight> mLightBuffer;
    UploadGPURingBuffer<Alias> mLightAliasTableBuffer;
    /* Textures */
    BindlessPool mTexture2DPool;
    BindlessPool mTexture3DPool;
    // Precomputed LUTs (stored in texture2D pool)
    uint32_t mLUTGGXEIndex{UINT32_MAX};
    uint32_t mLUTSheenLTCIndex{UINT32_MAX};
    // Display transform 3D LUTs (stored in texture3D pool)
    uint32_t mLUTViewSdrIndex{UINT32_MAX}, mLUTViewHdrIndex{UINT32_MAX};
    // Shared default resources (stored in texture2D pool).
    uint32_t mFoundationDefaultTexture2DIndex{UINT32_MAX};
    uint32_t mFoundationDefaultTexture2DFloatIndex{UINT32_MAX};
    RHIDeviceScopedHandle<RHIBuffer> mFoundationDefaultBufferFloat;
    // Environment map (stored in texture2D pool)
    uint32_t mEnvMapIndex{UINT32_MAX};
    uint32_t mEnvMapMarginalCDFIndex{UINT32_MAX};
    uint32_t mEnvMapConditionalCDFIndex{UINT32_MAX};
    [[nodiscard]] BindlessPool& SelectTexturePool(RHITextureDimension viewDimension);
    [[nodiscard]] BindlessPool const& SelectTexturePool(RHITextureDimension viewDimension) const;
    size_t UploadOrUpdateTexture(ImmediateUpload* ctx, FTexture const& source, uint32_t& index,
                                 const char* debugName = nullptr);
    /* AS */
    // BLAS
    Vector<RHIDeviceScopedHandle<RHIAccelerationStructure>> mBLASes;
    Vector<RHIDeviceScopedHandle<RHIBuffer>> mBLASBuffers;
    Vector<RHIDeviceScopedHandle<RHIAccelerationStructure>> mCurveBLASes;
    Vector<RHIDeviceScopedHandle<RHIBuffer>> mCurveBLASBuffers;
    RHIDeviceScopedHandle<RHIBuffer> mCurveAABBBuffer;
    char* mCurveAABBMapped{nullptr};
    size_t mCurveAABBOffset{0};
    // TLAS
    uint32_t mTLASInstanceStride{0}; // In bytes, read only once
    uint32_t mLastTLASInstancesCount{0};
    RHIDeviceScopedHandle<RHIBuffer> mTLASBuffer, mScratchBufferTLAS;
    RHIDeviceScopedHandle<RHIAccelerationStructure> mTLAS;
    UploadGPURingBuffer<char> mTLASInstances;
    uint32_t CountTLASInstances(Span<const GSInstance> instances, Span<const GSLight> lights) const;
    bool EnsureTLASCapacity(uint32_t totalInstances, bool allowRecreate);
    // Samplers
    RHIDeviceScopedHandle<RHIBuffer> mSobolMatricesBuffer;
    
    // Light BLAS
    RHIDeviceScopedHandle<RHIBuffer> mLightGeometryBuffer;
    RHIDeviceScopedHandle<RHIAccelerationStructure> mRectBLAS;
    RHIDeviceScopedHandle<RHIAccelerationStructure> mDiskBLAS;
    RHIDeviceScopedHandle<RHIBuffer> mLightBLASBuffer;
public:
    enum class LightSamplerType
    {
        Uniform,
        Power
    };
    LightSamplerType mLightSamplerType = LightSamplerType::Power;

    [[nodiscard]] static size_t CalculateMeshPrimitiveSize(FSerializedMesh const& src);
    [[nodiscard]] static size_t CalculateCurvePrimitiveSize(FSerializedCurve const& src);
    [[nodiscard]] static size_t CalculateCurveAABBSize(FSerializedCurve const& src);

    GPUScene(RHIDevice* device, Allocator* allocator, GPUSceneDesc const& desc,
             AllocatorStack* frameScratch = nullptr);

    Pair<GSInstance*, uint32_t> AllocateInstance(uint32_t count);
    Pair<GSMaterial*, uint32_t> AllocateMaterial(uint32_t count);
    Pair<GSLight*, uint32_t> AllocateLight(uint32_t count);
    Pair<Alias*, uint32_t> AllocateLightAliasTable(uint32_t count);

    /**
     * @brief Result of UpdateGPUScene: ring-buffer offsets and element counts
     *        for instances and materials, ready to be written into the UBO.
     */
    struct UpdateResult
    {
        uint32_t firstInstance;
        uint32_t numInstances;
        uint32_t firstMaterial;
        uint32_t numMaterials;
        uint32_t firstLight;
        uint32_t firstLightAliasTable;
        uint32_t numLights;
        float sceneLightWeightSum;
    };
    /**
     * @brief Bulk-copies GS instance and material arrays into the GPU ring buffers.
     * @return Offsets / counts to populate the UBO with.
     */
    UpdateResult UpdateGPUScene(Span<const GSInstance> instances, Span<const GSMaterial> materials, Span<const GSLight> lights);

    struct MemoryStat
    {
        String name;
        size_t bytes;
    };
    void DbgGetMemoryStatistics(Vector<MemoryStat>& outStats) const;
    [[nodiscard]] String DbgGetBufferStatistics() const;
    [[nodiscard]] bool UsesDirectGeometryUpload() const { return mDirectGeometryUpload; }
    [[nodiscard]] uint32_t GetLightCapacity() const { return mLightBuffer.Capacity(); }
    void FlushDirectGeometryUpload();

    struct StagedUploadJob
    {
        enum class Kind : uint32_t
        {
            None,
            MeshHeader,
            CurveHeader,
            Blob,
        } kind{Kind::None};
        FBlobRef blob{};
        char* ptr{nullptr};
        size_t size{0};
        GSMesh meshData{};
        GSCurveSet curveData{};

        [[nodiscard]] bool NeedsScratch() const;
        void Write(FBlobDeserializer const& blobs, Allocator* scratchAlloc = nullptr) const;
    };

    struct TextureUpload
    {
        FTextureHeader metadata{};
        RHIDeviceScopedHandle<RHITexture> texture;

        [[nodiscard]] bool IsValid() const { return texture.IsValid(); }
    };

    size_t BeginUpload(ImmediateUpload* ctx, FSerializedMesh const& source, GSMesh& outData,
                       uint32_t& outOffset, Vector<StagedUploadJob>& outJobs);
    size_t BeginUpload(ImmediateUpload* ctx, FSerializedCurve const& source,
                       GSCurveSet& outData, uint32_t& outOffset, Vector<StagedUploadJob>& outJobs);
    size_t Upload(ImmediateUpload* ctx, FTextureHeader const& metadata, Span<const unsigned char> data, uint32_t& outIndex, const char* debugName = nullptr);
    size_t Upload(ImmediateUpload* ctx, FTexture const& source, uint32_t& outIndex, const char* debugName = nullptr);
    TextureUpload BeginTextureUpload(ImmediateUpload* ctx, FSerializedTexture const& source,
                                     const char* debugName = nullptr);
    size_t BeginTextureSubresourceUpload(ImmediateUpload* ctx, FSerializedTexture const& source,
                                         TextureUpload& upload, uint32_t layer, uint32_t mip,
                                         Vector<StagedUploadJob>& outJobs);
    void EndTextureUpload(ImmediateUpload* ctx, TextureUpload&& upload, uint32_t& outIndex);
    size_t BeginUpload(ImmediateUpload* ctx, FSerializedTexture const& source, uint32_t& outIndex,
                       Vector<StagedUploadJob>& outJobs, const char* debugName = nullptr);

    void BuildBLAS(ImmediateContext* ctx, Span<const GSMesh> meshes, Span<uint32_t> outBLASIndices,
                   ImmediateSubmitDesc const& firstSubmitDesc = {});
    void BuildCurveBLAS(ImmediateContext* ctx, Span<const GSCurveSet> curves, Span<uint32_t> outBLASIndices,
                        ImmediateSubmitDesc const& firstSubmitDesc = {});
    enum class TLASBuildResult
    {
        Built,
        Empty,
        NeedsRendererRebuild
    };
    [[nodiscard]] bool EnsureTLASCapacity(Span<const GSInstance> instances, Span<const GSLight> lights);
    [[nodiscard]] TLASBuildResult BuildTLAS(RHICommandList* cmd, Span<const GSInstance> instances, Span<const uint32_t> blasIndices, Span<const uint32_t> curveBLASIndices, Span<const GSLight> lights, bool update = false);

    /* Geometry */
    [[nodiscard]] RHIBuffer* GetPrimitiveBuffer() const { return mPrimitiveBuffer.Get(); }
    [[nodiscard]] RHIBuffer* GetInstanceBuffer() const { return mInstanceBuffer.mBuffer.Get(); }
    [[nodiscard]] RHIBuffer* GetMaterialBuffer() const { return mMaterialBuffer.mBuffer.Get(); }
    [[nodiscard]] RHIBuffer* GetLightBuffer() const { return mLightBuffer.mBuffer.Get(); }
    [[nodiscard]] RHIBuffer* GetLightAliasTableBuffer() const { return mLightAliasTableBuffer.mBuffer.Get(); }
    /* Textures */
    [[nodiscard]] BindlessPool* GetTexture2DPool() { return &mTexture2DPool; }
    [[nodiscard]] BindlessPool* GetTexture3DPool() { return &mTexture3DPool; }
    [[nodiscard]] BindlessPool* GetTexturePool() { return GetTexture2DPool(); }
    [[nodiscard]] uint32_t GetGGXLutEIndex() const { return mLUTGGXEIndex; }
    [[nodiscard]] uint32_t GetSheenLtcIndex() const { return mLUTSheenLTCIndex; }
    [[nodiscard]] RHITexture* GetGGXlutE() const;
    [[nodiscard]] RHITexture* GetSheenLtc() const;
    void UploadViewLUTs(ImmediateUpload* ctx, FTexture const& sdr, FTexture const& hdr);
    [[nodiscard]] uint32_t GetViewLutSdrIndex() const { return mLUTViewSdrIndex; }
    [[nodiscard]] uint32_t GetViewLutHdrIndex() const { return mLUTViewHdrIndex; }
    [[nodiscard]] RHITexture* GetViewLutSdr() const;
    [[nodiscard]] RHITexture* GetViewLutHdr() const;
    [[nodiscard]] RHITexture* GetFoundationDefaultTexture2D() const;
    [[nodiscard]] RHITexture* GetFoundationDefaultTexture2DFloat() const;
    [[nodiscard]] RHIBuffer* GetFoundationDefaultBufferFloat() const { return mFoundationDefaultBufferFloat.Get(); }
    // Environment map
    void UploadEnvMap(ImmediateUpload* ctx, FTexture const& source);
    [[nodiscard]] bool HasEnvMap() const { return mEnvMapIndex != UINT32_MAX; }
    [[nodiscard]] uint32_t GetEnvMapIndexOrDefault() const
    {
        return HasEnvMap() ? mEnvMapIndex : mFoundationDefaultTexture2DIndex;
    }
    [[nodiscard]] uint32_t GetEnvMapMarginalCDFIndexOrDefault() const
    {
        return mEnvMapMarginalCDFIndex != UINT32_MAX ? mEnvMapMarginalCDFIndex : mFoundationDefaultTexture2DFloatIndex;
    }
    [[nodiscard]] uint32_t GetEnvMapConditionalCDFIndexOrDefault() const
    {
        return mEnvMapConditionalCDFIndex != UINT32_MAX ? mEnvMapConditionalCDFIndex : mFoundationDefaultTexture2DFloatIndex;
    }
    [[nodiscard]] RHITexture* GetEnvMap() const;
    [[nodiscard]] RHITexture* GetEnvMapMarginalCDF() const;
    [[nodiscard]] RHITexture* GetEnvMapConditionalCDF() const;
    /* AS */
    [[nodiscard]] RHIAccelerationStructure* GetTLAS() const
    {
        return mTLAS.IsValid() && mLastTLASInstancesCount > 0 ? mTLAS.Get() : nullptr;
    }
    /* Samplers */
    [[nodiscard]] RHIBuffer* GetSobolMatricesBuffer() const;

    void Reset();
};