#include "GPUScene.hpp"
#include <Core/Allocator.hpp>
#include <Core/AllocatorStack.hpp>
#include <Core/Atomic.hpp>
#include <Core/AtomicQueue.hpp>
#include <Core/Hash.hpp>
#include <Core/Thread.hpp>
#include <Core/JobSystem.hpp>
#include <algorithm>
#include <bit>
#include <condition_variable>
#include <cstddef>
#include "LightBVH.hpp"
#include "Precompute.hpp"
#include "Renderer.hpp"
#include "Tables/GGX.hpp"
#include "Tables/GGX_IOR.hpp"
#include "Tables/LTCSheen.hpp"
#include "Tables/Sobol.hpp"

static constexpr size_t kUploadBudgetSlack = 1ull * (1ull << 20);
static constexpr size_t kUploadStagingBuffers = 3u;
static constexpr size_t kGPUSceneBufferQueueCapacity = 256u;
static constexpr uint32_t kGPUSceneDynamicRebuildRate = 60u; // frames

static bool IsIntersectableLight(GSLight const& light)
{
    uint32_t type = light.flags & kGSLightTypeMask;
    return type == kGSLightTypeDisk || type == kGSLightTypeRect ||
        ((type == kGSLightTypePoint || type == kGSLightTypeSpot) && light.params.x > 0.0f);
}

static bool IsSunDiskLight(GSLight const& light)
{
    return GSLightTypeCPU(light) == kGSLightTypeDirectional && light.params.x > 0.0f;
}

static size_t GPUSceneTextureSubresourceFootprint(FTextureHeader const& metadata, uint32_t layer, uint32_t mip)
{
    uint32_t const alignment = std::max(metadata.GetBpp() / 8, metadata.GetBlockSize());
    CHECK_MSG(alignment != 0, "Unsupported texture format {}", metadata.GetFormat());
    size_t const size = metadata.GetSubresourceSize(layer, mip);
    CHECK_MSG(size <= std::numeric_limits<size_t>::max() - (alignment - 1u),
              "Texture subresource staging footprint exceeds addressable range");
    return size + alignment - 1u;
}

static constexpr size_t kMinDirectGeometryUploadHeapSize = 512ull * (1ull << 20);
static constexpr uint32_t kGPUScenePersistentTexture3DBindings = 3u; // GGX IOR/Inv IOR + sheen LTC LUTs.
static constexpr uint32_t kGPUSceneTextureBindingSlack = 8u;
static constexpr size_t kGPUSceneByteBudgetSlack = 64u << 10u;

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
                                        .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress |
                                            RHIBufferUsageBits::AccelerationStructureBuildReadOnly,
                                        .size = budget * sizeof(T)});
        mBegin = mRing = mPrevRing = mBuffer->Map<T>();
        mEnd = mBegin + budget;
    }
    Pair<T*, uint32_t> Allocate(uint32_t count)
    {
        CHECK_MSG(count <= Capacity(), "GPU upload ring allocation overflow: requested {} elements, capacity {}", count,
                  Capacity());
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

template <typename T>
uint32_t FreelistPop(Vector<T>& storage, Vector<uint32_t>& freelist)
{
    if (!freelist.empty())
    {
        uint32_t slot = freelist.back();
        freelist.pop_back();
        return slot;
    }
    else
    {
        storage.emplace_back();
        return static_cast<uint32_t>(storage.size() - 1);
    }
}

void FreelistPush(Vector<uint32_t>& freelist, uint32_t slot) { freelist.push_back(slot); }

struct GPUSceneImpl
{
    using Result = GPUScene::Result;
    using UpdateResult = GPUScene::UpdateResult;
    using GPUSceneTables = GPUScene::GPUSceneTables;
    using MemoryStat = GPUScene::MemoryStat;
    using TLASBuildResult = GPUScene::TLASBuildResult;
    GPUScene& owner;
    RHIDevice* mDevice{nullptr};
    JobSystem* mJobs{nullptr};
    Allocator* mAllocator{GLOBAL_ALLOC};
    AllocatorStack* mStackAlloc{nullptr};

    enum class ResourceState : uint8_t
    {
        Queued,
        Uploading,
        Ready,
        Failed
    };
    /* Geometry */
    struct Geometry
    {
        uint32_t type{kGSInstanceTypeMesh};
        uint32_t version{0};
        uint32_t offset{0}; // into respective primitive buffer of type in bytes
        uint32_t blas{UINT32_MAX}; // see @ref ResolveBLAS
        ResourceState state{ResourceState::Queued};
        /* --- */
        GSMesh mesh{};
        GSCurveSet curve{};
        bool dynamic{false};
        bool isGpu{false};
        bool dynamicIsBuilt{false};
        bool dynamicIndicesDirty{false};
        uint32_t dynamicLastRebuildFrame{0};
        uint32_t dynamicVtxBytes{0};
        RHIDeviceScopedHandle<RHIBuffer> dynamicBLASBuffer;
        RHIDeviceScopedHandle<RHIBuffer> dynamicBLASScratch;
        /* --- */
        bool live{false};
        bool dirty{false}; // BLAS rebuild/refit pending
        bool uploadHeader{false};
    };
    Vector<Geometry> mGeometry;
    Vector<uint32_t> mGeometryFreelist;
    RHIDeviceScopedHandle<RHIVirtualAllocator> mPrimitiveAlloc;
    // Fast path for UMA devices?
    char* mPrimitiveMapped{nullptr};
    bool mDirectGeometryUpload{false};
    // Dynamic geometry
    // Topology for these do NOT change (e.g. skinning, morphing), otherwise
    // rebuilding BLASes is required.
    bool mDynamicIsUpdate{false};
    char* mDynamicStagingMapped{nullptr};
    RHIDeviceScopedHandle<RHIBuffer> mDynamicPrimitiveBuffer;
    RHIDeviceScopedHandle<RHIBuffer> mDynamicStagingBuffer;
    RHIDeviceScopedHandle<RHIVirtualAllocator> mDynamicPrimitiveAlloc;
    uint32_t mDynamicStagingFrames{0};
    uint32_t mDynamicStagingFrameIndex{0};
    uint32_t mDynamicStagingFrameSize{0};
    uint32_t mDynamicStagingCursor{0};
    Vector<RHICommandList::CopyBufferRegion> mDynamicUploadRegions;
    Vector<uint32_t> mDynamicGeometries; // index into mGeometry
    uint32_t mLastRefitCount{0};
    uint32_t mLastRebuildCount{0};
    uint32_t mMeshletGlobalCounter{0};
    UploadGPURingBuffer<GSInstance> mInstanceBuffer;
    UploadGPURingBuffer<GSMaterial> mMaterialBuffer;

    // Light BVH
    bool mLightBVHNeedsRefit{false};
    UploadGPURingBuffer<GSLight> mLightBuffer;
    UploadGPURingBuffer<GSLightBVHNode> mLightBVHNodeBuffer;
    Vector<LightBVHRefitLevel> mLightBVHRefitLevels;
    UploadGPURingBuffer<uint32_t> mLightBVHLightIndexBuffer;
    UploadGPURingBuffer<uint2> mLightBVHBitmaskBuffer;
    UploadGPURingBuffer<uint32_t> mLightBVHGlobalIndexBuffer;
    UploadGPURingBuffer<uint32_t> mLightBVHNodeIndexBuffer;

    // BLAS
    Vector<RHIDeviceScopedHandle<RHIAccelerationStructure>> mBLASes;
    Vector<RHIDeviceScopedHandle<RHIBuffer>> mBLASBuffers;
    Vector<uint32_t> mBLASFreelist;
    Vector<RHIDeviceScopedHandle<RHIAccelerationStructure>> mCurveBLASes;
    Vector<RHIDeviceScopedHandle<RHIBuffer>> mCurveBLASBuffers;
    Vector<uint32_t> mCurveBLASFreelist;

    // Light BSDF BLAS (procedural)
    RHIDeviceScopedHandle<RHIBuffer> mLightGeometryBuffer;
    RHIDeviceScopedHandle<RHIAccelerationStructure> mRectBLAS;
    RHIDeviceScopedHandle<RHIAccelerationStructure> mDiskBLAS;
    RHIDeviceScopedHandle<RHIAccelerationStructure> mSphereBLAS;
    RHIDeviceScopedHandle<RHIBuffer> mLightBLASBuffer;

    // TLAS
    uint32_t mTLASInstanceStride{0};
    RHIDeviceScopedHandle<RHIBuffer> mTLASBuffer, mScratchBufferTLAS;
    UploadGPURingBuffer<char> mTLASInstances;
    void EnsureTLASCapacity(uint32_t totalInstances);
    uint32_t CountLiveInstances() const;
    uint32_t CountTLASInstances() const;

    [[nodiscard]] Geometry* ResolveGeometry(GeometryHandle handle);
    [[nodiscard]] Geometry const* ResolveGeometry(GeometryHandle handle) const;
    void FreeGeometry(uint32_t slot);

    /* Textures */
    BindlessPool mTexture2DPool;
    BindlessPool mTexture3DPool;
    struct Texture
    {
        uint32_t version{0};
        bool live{false};
        bool pinned{false}; // do not recycle. used for LUTs, defaults, and env map.
        bool resident{false}; // readable by device
    };
    Vector<Texture> mTexture2DSlots;
    Vector<Texture> mTexture3DSlots;
    [[nodiscard]] Vector<Texture>& TextureSlots(bool is3D) { return is3D ? mTexture3DSlots : mTexture2DSlots; }
    [[nodiscard]] Vector<Texture> const& TextureSlots(bool is3D) const
    {
        return is3D ? mTexture3DSlots : mTexture2DSlots;
    }
    [[nodiscard]] BindlessPool& TexturePool(bool is3D) { return is3D ? mTexture3DPool : mTexture2DPool; }
    void FreeTextureSlot(bool is3D, uint32_t slot);
    [[nodiscard]] BindlessPool& SelectTexturePool(RHITextureDimension viewDimension);
    [[nodiscard]] BindlessPool const& SelectTexturePool(RHITextureDimension viewDimension) const;

    // Uploads
    // Uploads are always queued, and we bump upload/mapped staging/UMA direct addresses for uploaders to memcpy into
    struct PendingGeometryUpload
    {
        GeometryHandle handle{};
        FBlobDeserializer blobs{Span<const unsigned char>{}};
        const FSerializedMesh* mesh{nullptr};
        const FSerializedCurve* curve{nullptr};
        size_t footprint{0}; // staging bytes (primitive); 0/1 when direct-mapped.
    };
    struct PendingTextureUpload
    {
        uint32_t index{UINT32_MAX};
        FBlobDeserializer blobs{Span<const unsigned char>{}};
        const FSerializedTexture* source{nullptr};
    };
    struct PendingBufferUpload
    {
        RHIBuffer* dst{nullptr};
        uint32_t dstOffset{0};
        Vector<unsigned char> data{GLOBAL_ALLOC};
    };

    struct BlobCopyTask
    {
        FBlobRef blob{};
        char* dst{nullptr};
        size_t size{0};
        FBlobDeserializer const* blobs{};
    };

    struct UploadBatchState
    {
        explicit UploadBatchState(Allocator* allocator) :
            allocator(allocator), geometry(allocator), textures(allocator), buffers(allocator), writes(allocator),
            scratchArenas(allocator), scratchAllocators(allocator), geometryHandles(allocator),
            geometryBLAS(allocator), uploadedTextures(allocator), uncompactedBLAS(allocator),
            meshGeometryIndices(allocator)
        {
        }
        ~UploadBatchState()
        {
            if (batch.IsValid())
                batch.Abort();
            if (scratchArena.memory)
                allocator->DeallocateArena(scratchArena);
        }

        Allocator* allocator{};
        Vector<PendingGeometryUpload> geometry;
        Vector<PendingTextureUpload> textures;
        Vector<PendingBufferUpload> buffers;
        ImmediateUpload::UploadBatch batch;
        Vector<BlobCopyTask> writes;
        Arena scratchArena{};
        Vector<Arena> scratchArenas;
        Vector<UniquePtr<AllocatorStack>> scratchAllocators;
        Vector<GeometryHandle> geometryHandles;
        Vector<uint32_t> geometryBLAS;
        Vector<std::pair<uint32_t, bool>> uploadedTextures;
        RHIDeviceScopedHandle<RHIDeviceSemaphore> completionTimeline;
        RHIDeviceScopedHandle<RHIDeviceQueryPool> compactionQueries;
        RHIDeviceScopedHandle<RHIBuffer> uncompactedBLASBuffer;
        Vector<RHIDeviceScopedHandle<RHIAccelerationStructure>> uncompactedBLAS;
        Vector<size_t> meshGeometryIndices;
        UniquePtr<ImmediateContext> blasContext;
        RHIDeviceScopedHandle<RHIBuffer> blasScratch;
        JobHandle prepareJob;
        JobHandle decodeJob;
        JobHandle submitJob;
        JobHandle commitJob;
        JobDependency prepareGate;
        JobDependency publishGate;
        RHIDeviceSemaphore* finalTimeline{};
        size_t finalTimelineValue{};
        size_t itemCount{};
        Atomic<bool> submitted{false};
        Atomic<bool> failed{false};
        bool needsCompaction{false};
        bool gpuComplete{false};
        bool publicationReleased{false};
    };

    // Async upload queues
    MPMCQueue<PendingGeometryUpload> mUploadGeometryQueue;
    MPMCQueue<PendingTextureUpload> mUploadTextureQueue;
    MPMCQueue<PendingBufferUpload> mUploadBufferQueue;
    Atomic<size_t> mUploadPending{0};
    Atomic<bool> mUploadFailed{false};
    UniquePtr<ImmediateUpload> mImmediateUpload;
    size_t mImmediateUploadCapacity{};
    UniquePtr<UploadBatchState> mActiveUpload;
    Mutex mUploadDriveMutex;
    mutable Mutex mUploadStateMutex;
    template <typename T>
    void EnqueueUpload(MPMCQueue<T>& queue, T&& item);
    bool StartUploadBatch();
    bool DriveUploadBatch(size_t timeout);
    void PrepareUploads(UploadBatchState& state);
    void DecodeUploads(UploadBatchState& state, size_t begin, size_t end, JobContext& context);
    void SubmitUploads(UploadBatchState& state);
    void CommitUploads(UploadBatchState& state); // mark as ready
    void SubmitBLAS(UploadBatchState& state, ImmediateSubmitDesc const& submitDesc);
    void SubmitBLASCompaction(UploadBatchState& state);
    Result ReserveMesh(FSerializedMesh const& src, GSMesh& outHeader, uint32_t& outOffset);
    Result ReserveCurve(FSerializedCurve const& src, GSCurveSet& outHeader, uint32_t& outOffset);
    size_t StageMesh(ImmediateUpload::UploadBatch* batch, FSerializedMesh const& src, GSMesh const& header,
                     uint32_t offset, Vector<BlobCopyTask>& outWrites);
    size_t StageCurve(ImmediateUpload::UploadBatch* batch, FSerializedCurve const& src, GSCurveSet const& header,
                      uint32_t offset, Vector<BlobCopyTask>& outWrites);
    size_t StageTextureSubresource(ImmediateUpload::UploadBatch* batch, FSerializedTexture const& source,
                                   RHITexture* texture, uint32_t layer, uint32_t mip, Vector<BlobCopyTask>& outWrites);
    void FlushDirectGeometryUpload();

    // Allocation in ring buffers
    Span<GSInstance> AllocateInstance(uint32_t count, uint32_t& outOffset);
    Span<GSMaterial> AllocateMaterial(uint32_t count, uint32_t& outOffset);
    Span<GSLight> AllocateLight(uint32_t count, uint32_t& outOffset);


    GPUSceneImpl(GPUScene& owner, RHIDevice* device, JobSystem* jobs, Allocator* allocator, GPUSceneDesc const& desc,
                 AllocatorStack* frameScratch);
    ~GPUSceneImpl();

    /* Mesh */
    Result Upload(FBlobDeserializer* blobs, FSerializedMesh const& source, GeometryHandle& outHandle);
    Result Upload(FImportedMesh const& source, GeometryHandle& outHandle, FUUID skeleton);
    Result Allocate(uint32_t vertexCount, uint32_t indexCount, GeometryHandle& outHandle, bool isGpu);
    /* Curve */
    Result Upload(FBlobDeserializer* blobs, FSerializedCurve const& source, GeometryHandle& outHandle);
    /* Texture */
    Result Upload(FBlobDeserializer* blobs, FSerializedTexture const& source, TextureHandle& outTexture,
                  const char* debugName = nullptr, bool pinned = false);
    Result Upload(FTexture const& source, TextureHandle& outTexture, const char* debugName = nullptr,
                  bool pinned = false);
    Result UploadEnvironmentMap(FTexture const& source);
    /* Buffer */
    Result Upload(RHIBuffer* dst, Span<const unsigned char> data, uint32_t dstOffset = 0);

    [[nodiscard]] Result Query(GeometryHandle handle) const;
    [[nodiscard]] Result Query(TextureHandle texture) const;
    void Join();
    [[nodiscard]] Result Poll();

    GPUSceneTables BeginScene(uint32_t instanceCount, uint32_t materialCount, uint32_t lightCount);
    UpdateResult EndScene(GPUSceneTables& tables);

    void DbgGetMemoryStatistics(Vector<MemoryStat>& outStats) const;
    [[nodiscard]] String DbgGetBufferStatistics() const;

    [[nodiscard]] TLASBuildResult BuildTLAS(RHICommandList* cmd, bool update);
    void Collect();
    void Reset();
    // Dynamic updates
    [[nodiscard]] uint32_t AllocateDynamicStaging(uint32_t size, uint32_t alignment);
    void AllocateDynamicBLAS(Geometry& g);
    [[nodiscard]] bool HasDynamicGeometry() const { return !mDynamicGeometries.empty(); }
    [[nodiscard]] bool HasCurveGeometry() const
    {
        for (Geometry const& g : mGeometry)
        {
            if (g.live && g.type == kGSInstanceTypeCurve && g.state == ResourceState::Ready)
                return true;
        }
        return false;
    }
    void BeginDynamicGeometryUpdate();
    void UpdateDynamicGeometryGPU(GeometryHandle handle, bool updateVertices, bool updateIndices);
    void UpdateDynamicGeometryCPU(GeometryHandle handle, Span<const FQVertex> vertices, Span<const uint32_t> indices);
    void EndDynamicGeometryUpdate();
    void UploadDynamicGeometryCPU(RHICommandList* cmd);
    void BuildBLAS(RHICommandList* cmd);
};

size_t GPUScene::CalculateMeshPrimitiveSize(FSerializedMesh const& src)
{
    uint64_t lod0Size = src.lods.empty() ? 0 : src.lods[0].indices.decodedSize;
    return sizeof(GSMesh) + src.vertices.decodedSize + lod0Size + src.dagGroups.decodedSize +
        src.dagMeshlets.decodedSize + src.dagMeshletVtx.decodedSize + src.dagMeshletTri.decodedSize;
}

size_t GPUScene::CalculateCurvePrimitiveSize(FSerializedCurve const& src)
{
    return sizeof(GSCurveSet) + src.vertices.decodedSize + src.indices.decodedSize + src.leaves.decodedSize;
}

void GPUSceneImpl::FlushDirectGeometryUpload()
{
    if (!mDirectGeometryUpload)
        return;
    if (mPrimitiveAlloc->GetPeakUsage())
        owner.mPrimitiveBuffer->Flush(0, mPrimitiveAlloc->GetPeakUsage());
}

GPUScene::GPUScene(RHIDevice* device, JobSystem* jobs, Allocator* allocator, GPUSceneDesc const& desc,
                   AllocatorStack* frameScratch) :
    mCommittedInstances(allocator), mCommittedLights(allocator), mCommittedMaterials(allocator),
    mTLASInstanceMap(allocator)
{
    mImpl = ConstructUnique<GPUSceneImpl>(allocator, *this, device, jobs, allocator, desc, frameScratch);
}

GPUSceneImpl::GPUSceneImpl(GPUScene& owner, RHIDevice* device, JobSystem* jobs, Allocator* allocator,
                           GPUSceneDesc const& desc,
                           AllocatorStack* frameScratch) :
    owner(owner), mDevice(device), mJobs(jobs), mAllocator(allocator), mStackAlloc(frameScratch),
    mInstanceBuffer(device, desc.instanceBudget), mMaterialBuffer(device, desc.materialBudget),
    mLightBuffer(device, desc.lightBudget), mLightBVHNodeBuffer(device, desc.lightBudget * 2u),
    mLightBVHLightIndexBuffer(device, desc.lightBudget), mLightBVHBitmaskBuffer(device, desc.lightBudget),
    mLightBVHGlobalIndexBuffer(device, desc.lightBudget), mLightBVHNodeIndexBuffer(device, desc.lightBudget * 2u),
    mTexture2DPool(device, allocator, {.maxBindings = desc.texturesBudget}),
    mTexture3DPool(device, allocator,
                   {.maxBindings = kGPUScenePersistentTexture3DBindings + kGPUSceneTextureBindingSlack}),
    mTexture2DSlots(allocator), mTexture3DSlots(allocator), mBLASes(allocator), mBLASBuffers(allocator),
    mBLASFreelist(allocator), mCurveBLASes(allocator), mCurveBLASBuffers(allocator), mCurveBLASFreelist(allocator),
    mGeometry(allocator), mGeometryFreelist(allocator), mDynamicUploadRegions(allocator), mDynamicGeometries(allocator),
    mUploadGeometryQueue(std::bit_ceil(static_cast<size_t>(desc.geometryBudget) + 1), allocator),
    mUploadTextureQueue(std::bit_ceil(static_cast<size_t>(desc.texturesBudget) + 1), allocator),
    mUploadBufferQueue(std::bit_ceil(static_cast<size_t>(kGPUSceneBufferQueueCapacity)), allocator),
    mLightBVHRefitLevels(allocator), mTLASInstanceStride(mDevice->WriteAccelerationStructureInstanceData({}, nullptr)),
    mTLASInstances(device, desc.tlasInstanceBudget * mTLASInstanceStride)
{
    CHECK(mDevice != nullptr);
    CHECK(mJobs != nullptr);
    CHECK(mAllocator != nullptr);
    CHECK_MSG(desc.dynamicGeometryBudget != 0 || desc.dynamicStagingBudget == 0,
              "dynamicStagingBudget requires dynamicGeometryBudget");

    static_assert(sizeof(GSLightBVHNode) == 48);
    mGeometry.reserve(desc.geometryBudget);
    mBLASes.reserve(desc.geometryBudget);
    mBLASBuffers.reserve(desc.geometryBudget);
    mCurveBLASes.reserve(desc.geometryBudget);
    mCurveBLASBuffers.reserve(desc.geometryBudget);
    mTexture2DSlots.reserve(desc.texturesBudget);
    mTexture3DSlots.reserve(kGPUScenePersistentTexture3DBindings + kGPUSceneTextureBindingSlack);
    mPrimitiveAlloc = mDevice->CreateVirtualAllocator(desc.primitiveBudget);
    auto caps = mDevice->GetCapabilities();
    size_t directGeometryBudget = static_cast<size_t>(desc.primitiveBudget);
    size_t minDirectGeometryHeapSize = std::max(directGeometryBudget, kMinDirectGeometryUploadHeapSize);
    mDirectGeometryUpload = caps.integratedGPU && caps.deviceLocalHostVisibleBuffers &&
        caps.deviceLocalHostVisibleHeapSize >= minDirectGeometryHeapSize;
    RHIResourceDesc geoDesc{
        .heap = RHIDeviceHeapType::Local,
        .hostAccess = mDirectGeometryUpload ? RHIResourceHostAccess::WriteOnly : RHIResourceHostAccess::Invisible,
        .shared = true,
    };
    owner.mPrimitiveBuffer = mDevice->CreateBuffer(
        {.resource = geoDesc,
         .usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::StorageBuffer |
             RHIBufferUsageBits::DeviceAddress | RHIBufferUsageBits::AccelerationStructureBuildReadOnly |
             RHIBufferUsageBits::IndexBuffer,
         .size = desc.primitiveBudget});
    if (mDirectGeometryUpload)
    {
        mPrimitiveMapped = owner.mPrimitiveBuffer->Map<char>();
        LOG(GPUScene, LogInfo, "Direct GPU Memory Access available ({} MiB budget used). Uploading via direct copy.",
            directGeometryBudget / (1u << 20));
    }
    if (desc.dynamicGeometryBudget != 0)
    {
        mDynamicPrimitiveBuffer = mDevice->CreateBuffer(
            {.resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
             .usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::StorageBuffer |
                 RHIBufferUsageBits::DeviceAddress | RHIBufferUsageBits::AccelerationStructureBuildReadOnly |
                 RHIBufferUsageBits::IndexBuffer,
             .size = desc.dynamicGeometryBudget});
        mDynamicPrimitiveBuffer->DebugSetObjectName("Dynamic Primitive Buffer");
        mDynamicPrimitiveAlloc = mDevice->CreateVirtualAllocator(desc.dynamicGeometryBudget);
        if (desc.dynamicStagingBudget != 0)
        {
            CHECK_MSG(desc.dynamicStagingFramesInFlight >= 1, "dynamicStagingFramesInFlight must be >= 1");
            mDynamicStagingFrames = desc.dynamicStagingFramesInFlight + 1u;
            mDynamicStagingFrameSize = desc.dynamicStagingBudget;
            size_t const stagingBytes = static_cast<size_t>(desc.dynamicStagingBudget) * mDynamicStagingFrames;
            mDynamicStagingBuffer = mDevice->CreateBuffer({.resource = {.heap = RHIDeviceHeapType::Upload,
                                                                        .hostAccess = RHIResourceHostAccess::WriteOnly,
                                                                        .coherent = true,
                                                                        .staging = true},
                                                           .usage = RHIBufferUsageBits::TransferSource,
                                                           .size = stagingBytes});
            mDynamicStagingBuffer->DebugSetObjectName("Dynamic Primitive Staging");
            mDynamicStagingMapped = mDynamicStagingBuffer->Map<char>();
        }
        LOG(GPUScene, LogInfo, "Dynamic geometry: {} MiB device + {} MiB staging per frame.",
            desc.dynamicGeometryBudget / (1u << 20), desc.dynamicStagingBudget / (1u << 20));
    }
    mTLASBuffer =
        mDevice->CreateBuffer({.resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
                               .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress |
                                   RHIBufferUsageBits::AccelerationStructureStorage,
                               .size = desc.tlasBudget});
    mScratchBufferTLAS = mDevice->CreateBuffer({
        .resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
        .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress,
        .size = desc.tlasScratchBudget,
        .alignment = 256 // Aligned to Vulkan spec. Should be large enough for other APIs as well?
    });
    CHECK_MSG(mTLASBuffer->mDesc.size <= UINT32_MAX, "TLAS budget {} exceeds uint32_t range", mTLASBuffer->mDesc.size);
    RHIAccelerationStructureDesc tlasDesc{.type = RHIAccelerationStructureType::TopLevel,
                                          .buffer = mTLASBuffer.Get(),
                                          .size = static_cast<uint32_t>(mTLASBuffer->mDesc.size)};
    owner.mTLAS = mDevice->CreateAccelerationStructure(tlasDesc);

    owner.mSobolMatricesBuffer =
        mDevice->CreateBuffer({.resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
                               .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination,
                               .size = sizeof(kSobolMatrices32)});
    owner.mSobolMatricesBuffer->DebugSetObjectName("Sobol Matrices");
    // Procedural AABBs
    {
        struct LightAABBs
        {
            RHIAccelerationStructureAABB rect;
            RHIAccelerationStructureAABB disk;
            RHIAccelerationStructureAABB sphere;
        } geo;
        constexpr float kLightAABBThickness = 1e-3f;
        geo.rect = RHIAccelerationStructureAABB{-1.0f, -1.0f, -kLightAABBThickness, 1.0f, 1.0f, kLightAABBThickness};
        geo.disk = geo.rect;
        geo.sphere = RHIAccelerationStructureAABB{-1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};

        mLightGeometryBuffer = mDevice->CreateBuffer(
            {.resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
             .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination |
                 RHIBufferUsageBits::DeviceAddress | RHIBufferUsageBits::AccelerationStructureBuildReadOnly,
             .size = sizeof(LightAABBs)});

        ImmediateUpload upload(mDevice, sizeof(LightAABBs));
        {
            ImmediateUpload::UploadBatch batch = upload.BeginBatch();
            char* ptr = batch.Upload(mLightGeometryBuffer.Get(), sizeof(LightAABBs), 0);
            std::memcpy(ptr, &geo, sizeof(LightAABBs));
            batch.End();
        }
        upload.WaitIdle();

        // Build BLAS
        RHIAccelerationStructureGeometryInfo rectGeoInfo{.type = RHIAccelerationGeometryType::AABBs,
                                                         .aabbData = {.aabbBuffer = mLightGeometryBuffer.Get(),
                                                                      .offset = offsetof(LightAABBs, rect),
                                                                      .count = 1,
                                                                      .stride = sizeof(RHIAccelerationStructureAABB)}};
        RHIAccelerationStructureBuildRangeInfo rectRange{.primitiveCount = 1};

        RHIAccelerationStructureGeometryInfo diskGeoInfo{.type = RHIAccelerationGeometryType::AABBs,
                                                         .aabbData = {.aabbBuffer = mLightGeometryBuffer.Get(),
                                                                      .offset = offsetof(LightAABBs, disk),
                                                                      .count = 1,
                                                                      .stride = sizeof(RHIAccelerationStructureAABB)}};
        RHIAccelerationStructureBuildRangeInfo diskRange{.primitiveCount = 1};
        RHIAccelerationStructureGeometryInfo sphereGeoInfo{
            .type = RHIAccelerationGeometryType::AABBs,
            .aabbData = {.aabbBuffer = mLightGeometryBuffer.Get(),
                         .offset = offsetof(LightAABBs, sphere),
                         .count = 1,
                         .stride = sizeof(RHIAccelerationStructureAABB)}};
        RHIAccelerationStructureBuildRangeInfo sphereRange{.primitiveCount = 1};

        RHIAccelerationStructureBuildDesc rectDesc{
            .type = RHIAccelerationStructureType::BottomLevel,
            .flags = RHIAccelerationStructureBuildFlagsBits::PreferFastTrace,
            .operation = RHIAccelerationStructureBuildOp::Build,
            .geometries = Span<const RHIAccelerationStructureGeometryInfo>{&rectGeoInfo, 1},
            .ranges = Span<const RHIAccelerationStructureBuildRangeInfo>{&rectRange, 1}};
        RHIAccelerationStructureBuildDesc diskDesc{
            .type = RHIAccelerationStructureType::BottomLevel,
            .flags = RHIAccelerationStructureBuildFlagsBits::PreferFastTrace,
            .operation = RHIAccelerationStructureBuildOp::Build,
            .geometries = Span<const RHIAccelerationStructureGeometryInfo>{&diskGeoInfo, 1},
            .ranges = Span<const RHIAccelerationStructureBuildRangeInfo>{&diskRange, 1}};
        RHIAccelerationStructureBuildDesc sphereDesc{
            .type = RHIAccelerationStructureType::BottomLevel,
            .flags = RHIAccelerationStructureBuildFlagsBits::PreferFastTrace,
            .operation = RHIAccelerationStructureBuildOp::Build,
            .geometries = Span<const RHIAccelerationStructureGeometryInfo>{&sphereGeoInfo, 1},
            .ranges = Span<const RHIAccelerationStructureBuildRangeInfo>{&sphereRange, 1}};

        StackArena<4096> sizeInfoArena;
        AllocatorStack sizeInfoScratch(sizeInfoArena);
        auto rectSize = mDevice->GetAccelerationStructureSizeInfo(rectDesc, sizeInfoScratch.Ptr());
        sizeInfoScratch.Reset(sizeInfoArena);
        auto diskSize = mDevice->GetAccelerationStructureSizeInfo(diskDesc, sizeInfoScratch.Ptr());
        sizeInfoScratch.Reset(sizeInfoArena);
        auto sphereSize = mDevice->GetAccelerationStructureSizeInfo(sphereDesc, sizeInfoScratch.Ptr());

        uint32_t rectOffset = 0;
        uint32_t diskOffset = AlignUp(rectSize.accelerationStructureSize, 256u);
        uint32_t sphereOffset = AlignUp(diskOffset + diskSize.accelerationStructureSize, 256u);
        uint32_t totalSize = sphereOffset + sphereSize.accelerationStructureSize;

        mLightBLASBuffer =
            mDevice->CreateBuffer({.resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
                                   .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress |
                                       RHIBufferUsageBits::AccelerationStructureStorage,
                                   .size = totalSize});

        uint32_t scratchSize =
            std::max(std::max(rectSize.buildScratchSize, diskSize.buildScratchSize), sphereSize.buildScratchSize);
        auto scratch =
            mDevice->CreateBuffer({.resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
                                   .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress,
                                   .size = scratchSize,
                                   .alignment = 256});

        mRectBLAS = mDevice->CreateAccelerationStructure({.type = RHIAccelerationStructureType::BottomLevel,
                                                          .buffer = mLightBLASBuffer.Get(),
                                                          .offset = rectOffset,
                                                          .size = rectSize.accelerationStructureSize});
        mDiskBLAS = mDevice->CreateAccelerationStructure({.type = RHIAccelerationStructureType::BottomLevel,
                                                          .buffer = mLightBLASBuffer.Get(),
                                                          .offset = diskOffset,
                                                          .size = diskSize.accelerationStructureSize});
        mSphereBLAS = mDevice->CreateAccelerationStructure({.type = RHIAccelerationStructureType::BottomLevel,
                                                            .buffer = mLightBLASBuffer.Get(),
                                                            .offset = sphereOffset,
                                                            .size = sphereSize.accelerationStructureSize});

        ImmediateContext ctx(mDevice);
        auto* cmd = ctx.Get();
        cmd->Begin();

        rectDesc.scratchBuffer = scratch.Get();
        rectDesc.scratchBufferOffset = 0;
        rectDesc.dstAS = mRectBLAS.Get();
        cmd->BuildAccelerationStructure({{{rectDesc}}});

        cmd->BeginTransition();
        cmd->SetBufferTransition(
            scratch.Get(),
            {.srcStage = RHIPipelineStageBits::AccelerationBuild, .dstStage = RHIPipelineStageBits::AccelerationBuild});
        cmd->EndTransition();

        diskDesc.scratchBuffer = scratch.Get();
        diskDesc.scratchBufferOffset = 0;
        diskDesc.dstAS = mDiskBLAS.Get();
        cmd->BuildAccelerationStructure({{{diskDesc}}});

        cmd->BeginTransition();
        cmd->SetBufferTransition(
            scratch.Get(),
            {.srcStage = RHIPipelineStageBits::AccelerationBuild, .dstStage = RHIPipelineStageBits::AccelerationBuild});
        cmd->EndTransition();

        sphereDesc.scratchBuffer = scratch.Get();
        sphereDesc.scratchBufferOffset = 0;
        sphereDesc.dstAS = mSphereBLAS.Get();
        cmd->BuildAccelerationStructure({{{sphereDesc}}});

        cmd->End();
        ctx.Submit();
        ctx.WaitIdle();
    }
    // Upload precomputed LUTs
    {
        auto MakeLUT = [this](const float* data, RHIResourceFormat format, uint32_t width, uint32_t height = 1,
                              uint32_t depth = 1, RHITextureDimension dimension = RHITextureDimension::E2D)
        {
            FTexture tex(GLOBAL_ALLOC);
            tex.Initialize(format, dimension, width, height, depth);
            const auto* bytes = reinterpret_cast<const unsigned char*>(data);
            tex.bytes.assign(bytes, bytes + tex.GetSize());
            return tex;
        };
        auto lutE = MakeLUT(kGGXlutE, RHIResourceFormat::R32G32SignedFloat, 32, 32);
        auto lutEavg = MakeLUT(kGGXlutEavg, RHIResourceFormat::R32SignedFloat, 32, 1, 1, RHITextureDimension::E2D);
        auto lutEIOR = MakeLUT(kGGXlutEIOR, RHIResourceFormat::R32SignedFloat, 16, 16, 16, RHITextureDimension::E3D);
        auto lutEIORavg = MakeLUT(kGGXlutEIORavg, RHIResourceFormat::R32SignedFloat, 32, 32);
        auto lutEIORInv =
            MakeLUT(kGGXlutEInvIOR, RHIResourceFormat::R32SignedFloat, 16, 16, 16, RHITextureDimension::E3D);
        auto lutEIORInvavg = MakeLUT(kGGXlutEInvIORavg, RHIResourceFormat::R32SignedFloat, 32, 32);
        auto sheenLtc = MakeLUT(kSheenLTCLut, RHIResourceFormat::R32G32B32A32SignedFloat, 32, 32);
        FTexture foundationDefaultTexture2D(mAllocator);
        foundationDefaultTexture2D.Initialize(RHIResourceFormat::R32G32B32A32SignedFloat, RHITextureDimension::E2D, 1,
                                              1);
        foundationDefaultTexture2D.bytes.assign(foundationDefaultTexture2D.GetSize(), 0u);
        FTexture foundationDefaultTexture2DFloat(mAllocator);
        foundationDefaultTexture2DFloat.Initialize(RHIResourceFormat::R32SignedFloat, RHITextureDimension::E2D, 1, 1);
        foundationDefaultTexture2DFloat.bytes.resize(sizeof(float));
        *reinterpret_cast<float*>(foundationDefaultTexture2DFloat.bytes.data()) = 1.0f;
        const size_t foundationDefaultBufferFloatSize = sizeof(float);
        owner.mFoundationDefaultBufferFloat =
            mDevice->CreateBuffer({.resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
                                   .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination,
                                   .size = foundationDefaultBufferFloatSize});
        Upload(lutE, owner.mLUTGGXEIndex, nullptr, true);
        Upload(lutEavg, owner.mLUTGGXEavgIndex, nullptr, true);
        Upload(lutEIOR, owner.mLUTGGXEIORIndex, nullptr, true);
        Upload(lutEIORavg, owner.mLUTGGXEIORavgIndex, nullptr, true);
        Upload(lutEIORInv, owner.mLUTGGXEIORInvIndex, nullptr, true);
        Upload(lutEIORInvavg, owner.mLUTGGXEIORInvavgIndex, nullptr, true);
        Upload(sheenLtc, owner.mLUTSheenLTCIndex, nullptr, true);
        Upload(foundationDefaultTexture2D, owner.mFoundationDefaultTexture2DIndex, "_FoundationDefaultTexture2D", true);
        Upload(foundationDefaultTexture2DFloat, owner.mFoundationDefaultTexture2DFloatIndex,
               "_FoundationDefaultTexture2DFloat", true);
        const float foundationDefaultBufferFloat = 1.0f;
        Upload(owner.mFoundationDefaultBufferFloat.Get(),
               Span<const unsigned char>(reinterpret_cast<const unsigned char*>(&foundationDefaultBufferFloat),
                                         foundationDefaultBufferFloatSize));
        Upload(owner.mSobolMatricesBuffer.Get(),
               Span<const unsigned char>(reinterpret_cast<const unsigned char*>(kSobolMatrices32),
                                         sizeof(kSobolMatrices32)));
        Join();
    }
}

GPUScene::~GPUScene() = default;

GPUSceneImpl::~GPUSceneImpl()
{
    Join();
    if (mImmediateUpload)
        mImmediateUpload->WaitIdle();
    if (mPrimitiveAlloc)
        mPrimitiveAlloc->Clear();
    for (auto& g : mGeometry)
    {
        g.dynamicBLASBuffer.Reset();
        g.dynamicBLASScratch.Reset();
    }
    if (mDynamicPrimitiveAlloc)
        mDynamicPrimitiveAlloc->Clear();
}

Span<GSInstance> GPUSceneImpl::AllocateInstance(uint32_t count, uint32& outOffset)
{
    auto [ptr, off] = mInstanceBuffer.Allocate(count);
    outOffset = off;
    return {ptr, count};
}

Span<GSMaterial> GPUSceneImpl::AllocateMaterial(uint32_t count, uint32& outOffset)
{
    auto [ptr, off] = mMaterialBuffer.Allocate(count);
    outOffset = off;
    return {ptr, count};
}

Span<GSLight> GPUSceneImpl::AllocateLight(uint32_t count, uint32& outOffset)
{
    auto [ptr, off] = mLightBuffer.Allocate(count);
    outOffset = off;
    return {ptr, count};
}

GPUScene::GPUSceneTables GPUSceneImpl::BeginScene(uint32_t instanceCount, uint32_t materialCount, uint32_t lightCount)
{
    GPUSceneTables tables{};
    tables.instances = AllocateInstance(instanceCount, tables.instanceRange.offset);
    tables.materials = AllocateMaterial(materialCount, tables.materialRange.offset);
    tables.lights = AllocateLight(lightCount, tables.lightRange.offset);
    tables.instanceRange.count = instanceCount;
    tables.materialRange.count = materialCount;
    tables.lightRange.count = lightCount;
    return tables;
}

GPUScene::UpdateResult GPUSceneImpl::EndScene(GPUSceneTables& tables)
{
    UpdateResult res{};
    res.instances = tables.instanceRange;
    res.instancesHash = FNV1a64(tables.instances);
    res.materials = tables.materialRange;
    res.materialsHash = FNV1a64(tables.materials);
    res.lights = tables.lightRange;
    CHECK_MSG(!tables.lights.empty(), "Scene must contain an environment light");
    std::sort(tables.lights.begin(), tables.lights.end(), [&](GSLight const& a, GSLight const& b)
              { return (a.flags & kGSLightTypeMask) < (b.flags & kGSLightTypeMask); });
    GSLight& environment = tables.lights.front();
    CHECK_MSG(GSLightTypeCPU(environment) == kGSLightTypeEnvironment,
              "First light must be an environment light, got {}", GSLightTypeCPU(environment));
    // When not using a HDRI, we set env's radiance to 0...so it in effect never particpates in NEE evaluation
    // and improves NEE for the rest of the lights.
    environment.params.y =
        (environment.flags & to_integer(GSLightFlagsBits::EnvironmentMap)) != 0u ? owner.mEnvMapAverageRadiance : 0.0f;
    res.lightsHash = FNV1a64(tables.lights);
    // Resolve instance resources
    for (auto& inst : tables.instances)
    {
        Geometry* g = ResolveGeometry({inst.resourceIndex, 0u});
        CHECK_MSG(g, "Instance isn't assigned with a valid resourceIndex");
        uint32_t const resourceOffset = g->offset;
        GSInstanceFlags const flags = g->dynamic ? GSInstanceFlagsBits::Dynamic : GSInstanceFlagsBits{};
        uint32_t const type = g->type | static_cast<uint32_t>(flags);
        inst.resourceOffset = resourceOffset;
        inst.type = type;
    }
    owner.mCommittedInstances.assign(tables.instances.begin(), tables.instances.end());
    owner.mCommittedMaterials.assign(tables.materials.begin(), tables.materials.end());
    owner.mCommittedLights.assign(tables.lights.begin(), tables.lights.end());
    // Light BVH update
    if (owner.mLastUpdateResult.lightsHash == res.lightsHash)
        res.lightBVH = owner.mLastUpdateResult.lightBVH, mLightBVHNeedsRefit = false;
    else
    {
        mLightBVHNeedsRefit = true;
        LightBVHOptions options{};
        LightBVHBuild bvh = BuildLightBVH(tables.lights, options, mStackAlloc ? mStackAlloc : mAllocator);
#ifndef NDEBUG
        String validationError;
        CHECK_MSG(ValidateLightBVH(bvh, tables.lights, &validationError), "Light BVH validation failed: {}",
                  validationError);
#endif
        UpdateResult::LightBVH& lightBVH = res.lightBVH;
        lightBVH.valid = bvh.valid;
        mLightBVHRefitLevels = bvh.refitLevels;

        if (!bvh.nodes.empty())
        {
            auto [nodePtr, nodeOff] = mLightBVHNodeBuffer.Allocate(static_cast<uint32_t>(bvh.nodes.size()));
            std::memcpy(nodePtr, bvh.nodes.data(), bvh.nodes.size() * sizeof(GSLightBVHNode));
            lightBVH.nodes.offset = nodeOff;
            lightBVH.nodes.count = static_cast<uint32_t>(bvh.nodes.size());
            if (bvh.distantRootNode != UINT32_MAX)
            {
                lightBVH.distantNodes.offset = nodeOff + bvh.distantRootNode;
                lightBVH.distantNodes.count = bvh.distantNodeCount;
            }
        }
        if (!bvh.nodeIndices.empty())
        {
            auto [nPtr, nOff] = mLightBVHNodeIndexBuffer.Allocate(static_cast<uint32_t>(bvh.nodeIndices.size()));
            std::memcpy(nPtr, bvh.nodeIndices.data(), bvh.nodeIndices.size() * sizeof(uint32_t));
            lightBVH.nodeIndices.offset = nOff;
            lightBVH.nodeIndices.count = static_cast<uint32_t>(bvh.nodeIndices.size());
        }
        {
            auto [maskPtr, maskOff] = mLightBVHBitmaskBuffer.Allocate(static_cast<uint32_t>(bvh.lightBitmasks.size()));
            for (size_t i = 0; i < bvh.lightBitmasks.size(); ++i)
            {
                uint64_t mask = bvh.lightBitmasks[i];
                maskPtr[i] = uint2(static_cast<uint32_t>(mask), static_cast<uint32_t>(mask >> 32));
            }
            lightBVH.bitmasks.offset = maskOff;
            lightBVH.bitmasks.count = static_cast<uint32_t>(bvh.lightBitmasks.size());
        }
        if (!bvh.lightIndices.empty())
        {
            auto [idxPtr, idxOff] = mLightBVHLightIndexBuffer.Allocate(static_cast<uint32_t>(bvh.lightIndices.size()));
            std::memcpy(idxPtr, bvh.lightIndices.data(), bvh.lightIndices.size() * sizeof(uint32_t));
            lightBVH.lightIndices.offset = idxOff;
            lightBVH.lightIndices.count = static_cast<uint32_t>(bvh.lightIndices.size());
        }
        if (!bvh.globalLightIndices.empty())
        {
            auto [gPtr, gOff] =
                mLightBVHGlobalIndexBuffer.Allocate(static_cast<uint32_t>(bvh.globalLightIndices.size()));
            std::memcpy(gPtr, bvh.globalLightIndices.data(), bvh.globalLightIndices.size() * sizeof(uint32_t));
            lightBVH.globalLightIndices.offset = gOff;
            lightBVH.globalLightIndices.count = static_cast<uint32_t>(bvh.globalLightIndices.size());
        }
    }

    owner.mLastUpdateResult = res;
    return res;
}

void GPUScene::UpdateUBO(RendererUBO& globals) const
{
    globals.instances = mLastUpdateResult.instances;
    globals.materials = mLastUpdateResult.materials;
    globals.lights = mLastUpdateResult.lights;
    globals.lightBVHNodes = mLastUpdateResult.lightBVH.nodes;
    globals.lightBVHLightIndices = mLastUpdateResult.lightBVH.lightIndices;
    globals.firstLightBVHBitmask = mLastUpdateResult.lightBVH.bitmasks.offset;
    globals.lightBVHGlobalIndices = mLastUpdateResult.lightBVH.globalLightIndices;
    globals.lightBVHValid = mLastUpdateResult.lightBVH.valid;
    globals.lightBVHDistantNodes = mLastUpdateResult.lightBVH.distantNodes;
    globals.ggxLutEIndex = mLUTGGXEIndex.index;
    globals.ggxLutEavgIndex = mLUTGGXEavgIndex.index;
    globals.ggxLutEIORIndex = mLUTGGXEIORIndex.index;
    globals.ggxLutEIORavgIndex = mLUTGGXEIORavgIndex.index;
    globals.ggxLutEIORInvIndex = mLUTGGXEIORInvIndex.index;
    globals.ggxLutEIORInvavgIndex = mLUTGGXEIORInvavgIndex.index;
    globals.sheenLtcIndex = mLUTSheenLTCIndex.index;
    globals.envMapTextureIndex = GetEnvMapIndexOrDefault();
    globals.envMapMarginalCDFIndex = GetEnvMapMarginalCDFIndexOrDefault();
    globals.envMapConditionalCDFIndex = GetEnvMapConditionalCDFIndexOrDefault();
    globals.envMapPrefilteredMips = HasEnvMap() ? mEnvMapPrefilteredMips : 0u;
    globals.hasEnvMap = HasEnvMap() ? 1u : 0u;
    Span<const float3> sh = GetEnvSHCoeffs();
    for (size_t i = 0; i < sh.size(); ++i)
        globals.envSHCoeffs[i] = sh[i];
}

void GPUSceneImpl::DbgGetMemoryStatistics(Vector<MemoryStat>& outStats) const
{
    auto AddBufferSize = [&](RHIDeviceScopedHandle<RHIBuffer> const& buffer) -> size_t
    {
        if (!buffer)
            return 0;
        return buffer->GetAllocationSize();
    };
    auto AddRingBufferSize = [&](auto const& buffer) -> size_t { return buffer.mBuffer->GetAllocationSize(); };
    auto SumBuffers = [&](auto const& buffers) -> size_t
    {
        size_t bytes = 0;
        for (auto const& buffer : buffers)
            bytes += AddBufferSize(buffer);
        return bytes;
    };

    size_t primitiveBytes = AddBufferSize(owner.mPrimitiveBuffer);
    auto texture2DStats = mTexture2DPool.GetStats();
    auto texture3DStats = mTexture3DPool.GetStats();
    size_t instanceBytes = AddRingBufferSize(mInstanceBuffer);
    size_t materialBytes = AddRingBufferSize(mMaterialBuffer);
    size_t lightBytes = AddRingBufferSize(mLightBuffer);
    size_t lightBVHNodeBytes = AddRingBufferSize(mLightBVHNodeBuffer);
    size_t lightBVHIndexBytes = AddRingBufferSize(mLightBVHLightIndexBuffer);
    size_t lightBVHBitmaskBytes = AddRingBufferSize(mLightBVHBitmaskBuffer);
    size_t lightBVHGlobalBytes = AddRingBufferSize(mLightBVHGlobalIndexBuffer);
    size_t lightBVHNodeIndexBytes = AddRingBufferSize(mLightBVHNodeIndexBuffer);
    size_t tlasInstanceBytes = AddRingBufferSize(mTLASInstances);
    size_t blasBytes = SumBuffers(mBLASBuffers);
    size_t curveBLASBytes = SumBuffers(mCurveBLASBuffers);
    size_t tlasBytes = AddBufferSize(mTLASBuffer);
    size_t tlasScratchBytes = AddBufferSize(mScratchBufferTLAS);
    size_t lightBLASBytes = AddBufferSize(mLightBLASBuffer);
    size_t lightGeometryBytes = AddBufferSize(mLightGeometryBuffer);
    size_t sobolBytes = AddBufferSize(owner.mSobolMatricesBuffer);
    size_t defaultBufferBytes = AddBufferSize(owner.mFoundationDefaultBufferFloat);

    size_t dynamicPrimitiveBytes = AddBufferSize(mDynamicPrimitiveBuffer);
    size_t dynamicStagingBytes = AddBufferSize(mDynamicStagingBuffer);

    outStats.push_back({"Primitive Buffer (Buffer)", primitiveBytes});
    outStats.push_back({"Dynamic Primitive Buffer (Buffer)", dynamicPrimitiveBytes});
    outStats.push_back({"Dynamic Primitive Staging (Buffer)", dynamicStagingBytes});
    outStats.push_back({"Texture2D Pool (Texture)", texture2DStats.ownedTextureBytes});
    outStats.push_back({"Texture3D Pool (Texture)", texture3DStats.ownedTextureBytes});
    outStats.push_back({"Instance Buffer (Buffer)", instanceBytes});
    outStats.push_back({"TLAS Instance Buffer (Buffer)", tlasInstanceBytes});
    outStats.push_back({"Dynamic Upload Buffers (Buffer)",
                        materialBytes + lightBytes + lightBVHNodeBytes + lightBVHIndexBytes + lightBVHBitmaskBytes +
                            lightBVHGlobalBytes + lightBVHNodeIndexBytes});
    outStats.push_back({"Mesh BLAS (Buffer)", blasBytes});
    outStats.push_back({"Curve BLAS (Buffer)", curveBLASBytes});
    outStats.push_back({"TLAS (Buffer)", tlasBytes});
    outStats.push_back({"TLAS Scratch (Buffer)", tlasScratchBytes});
    outStats.push_back({"Light AS (Buffer)", lightBLASBytes});
    outStats.push_back({"Other GPUScene Buffers (Buffer)", lightGeometryBytes + sobolBytes + defaultBufferBytes});
}

String GPUSceneImpl::DbgGetBufferStatistics() const
{
    String res;
    Vector<MemoryStat> stats(GLOBAL_ALLOC);
    DbgGetMemoryStatistics(stats);
    size_t totalBytes = 0;
    for (auto const& stat : stats)
        totalBytes += stat.bytes;

    auto texture2DStats = mTexture2DPool.GetStats();
    auto texture3DStats = mTexture3DPool.GetStats();
    fmt::format_to(std::back_inserter(res), "Primitive Buffer: {:.1f} MB allocated, used {:.1f} / {:.1f} MB\n",
                   owner.mPrimitiveBuffer->GetAllocationSize() / static_cast<float>(1 << 20u),
                   mPrimitiveAlloc->GetUsedBytes() / static_cast<float>(1 << 20u),
                   owner.mPrimitiveBuffer->mDesc.size / static_cast<float>(1 << 20u));
    fmt::format_to(std::back_inserter(res),
                   "Texture2D Pool: {:.1f} MB owned, {:.1f} MB referenced, used {} / {} bindings, owned {} textures\n",
                   texture2DStats.ownedTextureBytes / static_cast<float>(1 << 20u),
                   texture2DStats.referencedTextureBytes / static_cast<float>(1 << 20u), texture2DStats.activeBindings,
                   texture2DStats.capacity, texture2DStats.ownedTextureBindings);
    fmt::format_to(std::back_inserter(res),
                   "Texture3D Pool: {:.1f} MB owned, {:.1f} MB referenced, used {} / {} bindings, owned {} textures\n",
                   texture3DStats.ownedTextureBytes / static_cast<float>(1 << 20u),
                   texture3DStats.referencedTextureBytes / static_cast<float>(1 << 20u), texture3DStats.activeBindings,
                   texture3DStats.capacity, texture3DStats.ownedTextureBindings);
    fmt::format_to(std::back_inserter(res), "Instance Buffer: {:.1f} MB allocated, used {} / {} instances\n",
                   mInstanceBuffer.mBuffer->GetAllocationSize() / static_cast<float>(1 << 20u), mInstanceBuffer.Used(),
                   mInstanceBuffer.Capacity());
    for (auto const& stat : stats)
        fmt::format_to(std::back_inserter(res), "{}: {:.1f} MB\n", stat.name,
                       stat.bytes / static_cast<float>(1 << 20u));
    fmt::format_to(std::back_inserter(res), "GPUScene Tracked Total: {:.1f} MB",
                   totalBytes / static_cast<float>(1 << 20u));
    return res;
}

GPUScene::Result GPUSceneImpl::ReserveMesh(FSerializedMesh const& src, GSMesh& outData, uint32_t& outOffset)
{
    if (src.lods.empty())
        return Result::InvalidInput;
    auto const& lod0 = src.lods[0];
    const size_t size = GPUScene::CalculateMeshPrimitiveSize(src);
    constexpr size_t kAlign = 4;
    uint64_t base = mPrimitiveAlloc->Allocate(size, kAlign);
    if (base == RHIVirtualAllocator::kInvalidOffset)
    {
        LOG(GPUScene, LogError, "Primitive buffer overflow for serialized mesh. Need {} bytes, {} used of {}", size,
            mPrimitiveAlloc->GetUsedBytes(), mPrimitiveAlloc->GetCapacity());
        return Result::OutOfMemory;
    }
    outOffset = static_cast<uint32_t>(base);

    // Compute the shader header / absolute sub-offsets. The byte layout here MUST match
    // StageMesh, which derives local offsets from the header.
    outData = GSMesh{};
    uint32_t cursor = 0;
    auto Skip = [&](size_t bytes)
    {
        uint32_t off = cursor;
        cursor += static_cast<uint32_t>(bytes);
        return off;
    };
    Skip(sizeof(GSMesh));
    outData.vertices.count = src.vertexCount;
    outData.vertices.offset = outOffset + Skip(static_cast<size_t>(src.vertices.decodedSize));
    outData.indices.count = lod0.indexCount;
    outData.indices.offset = outOffset + Skip(static_cast<size_t>(lod0.indices.decodedSize));
    outData.groups.count = src.dagGroups.count;
    outData.groups.offset = outOffset + Skip(static_cast<size_t>(src.dagGroups.decodedSize));
    outData.meshlets.count = src.dagMeshlets.count;
    outData.meshlets.offset = outOffset + Skip(static_cast<size_t>(src.dagMeshlets.decodedSize));
    outData.meshletVtxOffset = outOffset + Skip(static_cast<size_t>(src.dagMeshletVtx.decodedSize));
    outData.meshletTriOffset = outOffset + Skip(static_cast<size_t>(src.dagMeshletTri.decodedSize));
    outData.meshletGlobalIndex = mMeshletGlobalCounter;
    mMeshletGlobalCounter += outData.meshlets.count;
    CHECK_MSG(cursor == size, "Mesh layout mismatch: expected {} got {}", size, cursor);
    return Result::InProgress;
}

size_t GPUSceneImpl::StageMesh(ImmediateUpload::UploadBatch* batch, FSerializedMesh const& src, GSMesh const& header,
                               uint32_t offset, Vector<BlobCopyTask>& outWrites)
{
    const size_t size = GPUScene::CalculateMeshPrimitiveSize(src);
    char* ptr = nullptr;
    if (mDirectGeometryUpload)
    {
        CHECK(mPrimitiveMapped != nullptr);
        ptr = mPrimitiveMapped + offset;
    }
    else
    {
        if (batch->ptr + size > batch->end)
            return 0;
        ptr = batch->Upload(owner.mPrimitiveBuffer.Get(), size, offset);
        CHECK(ptr != nullptr);
    }
    // The header is a trivial copy, written inline; only payloads need threaded decode.
    std::memcpy(ptr, &header, sizeof(GSMesh));
    auto AppendBlobWrite = [&](FBlobRef const& blob, uint32_t absOffset)
    { outWrites.push_back({blob, ptr + (absOffset - offset), static_cast<size_t>(blob.decodedSize)}); };
    auto const& lod0 = src.lods[0];
    AppendBlobWrite(src.vertices, header.vertices.offset);
    AppendBlobWrite(lod0.indices, header.indices.offset);
    AppendBlobWrite(src.dagGroups, header.groups.offset);
    AppendBlobWrite(src.dagMeshlets, header.meshlets.offset);
    AppendBlobWrite(src.dagMeshletVtx, header.meshletVtxOffset);
    AppendBlobWrite(src.dagMeshletTri, header.meshletTriOffset);
    return size;
}

GPUScene::Result GPUSceneImpl::ReserveCurve(FSerializedCurve const& src, GSCurveSet& outData, uint32_t& outOffset)
{
    static_assert(sizeof(FCurveDOTSVertex) == 8);
    static_assert(sizeof(FCurveLeaf) == 40);

    if (src.vertices.decodedSize == 0 || src.indices.count == 0 || src.leaves.count == 0)
        return Result::InvalidInput;
    CHECK_MSG(src.vertices.stride == sizeof(FCurveDOTSVertex), "Serialized curve vertex stride mismatch");
    CHECK_MSG(src.indices.stride == sizeof(uint32_t), "Serialized curve index stride mismatch");
    CHECK_MSG(src.leaves.stride == sizeof(FCurveLeaf), "Serialized curve leaf stride mismatch");
    CHECK_MSG(src.vertices.decodedSize == sizeof(FCurveDOTSVertex) * src.vertices.count,
              "Serialized curve vertex blob size mismatch");
    CHECK_MSG(src.indices.decodedSize == sizeof(uint32_t) * src.indices.count,
              "Serialized curve index blob size mismatch");
    CHECK_MSG(src.leaves.decodedSize == sizeof(FCurveLeaf) * src.leaves.count,
              "Serialized curve leaf blob size mismatch");
    CHECK_MSG(src.indices.count % 3 == 0, "Serialized curve index count must be a multiple of 3");
    CHECK_MSG(src.indices.count == src.leaves.count * 12,
              "Serialized curve DOTS expects 12 indices per leaf; got {} indices for {} leaves", src.indices.count,
              src.leaves.count);

    const size_t size = GPUScene::CalculateCurvePrimitiveSize(src);
    constexpr size_t kAlign = 4;
    uint64_t base = mPrimitiveAlloc->Allocate(size, kAlign);
    if (base == RHIVirtualAllocator::kInvalidOffset)
    {
        LOG(GPUScene, LogError, "Primitive buffer overflow for serialized curve. Need {} bytes, {} used of {}", size,
            mPrimitiveAlloc->GetUsedBytes(), mPrimitiveAlloc->GetCapacity());
        return Result::OutOfMemory;
    }
    outOffset = static_cast<uint32_t>(base);

    outData = GSCurveSet{};
    uint32_t cursor = 0;
    auto Skip = [&](size_t bytes)
    {
        uint32_t off = cursor;
        cursor += static_cast<uint32_t>(bytes);
        return off;
    };
    Skip(sizeof(GSCurveSet));
    outData.vertices.count = static_cast<uint32_t>(src.vertices.count);
    outData.vertices.offset = outOffset + Skip(static_cast<size_t>(src.vertices.decodedSize));
    outData.indices.count = static_cast<uint32_t>(src.indices.count);
    outData.indices.offset = outOffset + Skip(static_cast<size_t>(src.indices.decodedSize));
    outData.leaves.count = static_cast<uint32_t>(src.leaves.count);
    outData.leaves.offset = outOffset + Skip(static_cast<size_t>(src.leaves.decodedSize));
    CHECK_MSG(cursor == size, "Curve layout mismatch: expected {} got {}", size, cursor);
    return Result::InProgress;
}

size_t GPUSceneImpl::StageCurve(ImmediateUpload::UploadBatch* batch, FSerializedCurve const& src,
                                GSCurveSet const& header, uint32_t offset, Vector<BlobCopyTask>& outWrites)
{
    const size_t size = GPUScene::CalculateCurvePrimitiveSize(src);
    char* ptr = nullptr;
    if (mDirectGeometryUpload)
    {
        CHECK(mPrimitiveMapped != nullptr);
        ptr = mPrimitiveMapped + offset;
    }
    else
    {
        if (batch->ptr + size > batch->end)
            return 0;
        ptr = batch->Upload(owner.mPrimitiveBuffer.Get(), size, offset);
        CHECK(ptr != nullptr);
    }
    std::memcpy(ptr, &header, sizeof(GSCurveSet));
    auto AppendBlobWrite = [&](FBlobRef const& blob, char* dstPtr)
    { outWrites.push_back({blob, dstPtr, static_cast<size_t>(blob.decodedSize)}); };
    AppendBlobWrite(src.vertices, ptr + (header.vertices.offset - offset));
    AppendBlobWrite(src.indices, ptr + (header.indices.offset - offset));
    AppendBlobWrite(src.leaves, ptr + (header.leaves.offset - offset));
    return size;
}

static bool IsTexture3DView(RHITextureDimension dimension) { return dimension == RHITextureDimension::E3D; }

BindlessPool& GPUSceneImpl::SelectTexturePool(RHITextureDimension viewDimension)
{
    if (IsTexture3DView(viewDimension))
        return mTexture3DPool;
    CHECK_MSG(viewDimension != RHITextureDimension::E1D && viewDimension != RHITextureDimension::E1DArray,
              "Unsupported bindless texture view dimension {}", static_cast<uint32_t>(viewDimension));
    return mTexture2DPool;
}

BindlessPool const& GPUSceneImpl::SelectTexturePool(RHITextureDimension viewDimension) const
{
    return const_cast<GPUSceneImpl*>(this)->SelectTexturePool(viewDimension);
}

static uint32_t GetTextureUploadAlignment(FTextureHeader const& metadata)
{
    uint32_t const alignment = std::max(metadata.GetBpp() / 8, metadata.GetBlockSize());
    CHECK_MSG(alignment != 0, "Unsupported texture format {}", metadata.GetFormat());
    return alignment;
}

static FSerializedBounds BuildMeshBounds(FImportedMesh const& mesh)
{
    if (mesh.vertices.empty())
        return {};

    FSerializedBounds bounds = FSerializedBounds::Empty();
    for (FVertex const& vertex : mesh.vertices)
        bounds += vertex.position;
    return bounds;
}

static std::pair<FSerializedMesh, Vector<unsigned char>> SerializedFromMesh(FImportedMesh const& mesh, Allocator* alloc,
                                                                            FUUID skeleton)
{
    const_cast<FImportedMesh&>(mesh).EnsureQuantized();

    FSerializedMesh desc(alloc);
    desc.bounds = BuildMeshBounds(mesh);
    desc.vertexCount = static_cast<uint32_t>(mesh.verticesQuantized.size());
    desc.skeleton = skeleton;

    Vector<unsigned char> payload(alloc);
    MemoryBlobSerializer serializer(payload);

    desc.vertices = serializer.AppendArray(mesh.verticesQuantized);

    desc.lods.reserve(mesh.lods.size());
    for (auto const& lod : mesh.lods)
    {
        FSerializedMeshLOD& lodDesc = desc.lods.emplace_back();
        lodDesc.indexCount = static_cast<uint32_t>(lod.indices.size());
        lodDesc.indices = serializer.AppendArray(lod.indices);
    }

    desc.dagGroups = serializer.AppendArray(mesh.dag.groups);
    desc.dagMeshlets = serializer.AppendArray(mesh.dag.meshlets);
    desc.dagMeshletTri = serializer.AppendArray(mesh.dag.meshletTri);
    desc.dagMeshletVtx = serializer.AppendArray(mesh.dag.meshletVtx);
    desc.skinBinding = serializer.AppendArray(mesh.skin);

    return {desc, std::move(payload)};
}

static FSerializedTexture SerializedFromTexture(FTexture const& source, Allocator* alloc)
{
    FSerializedTexture adaptor(alloc);
    adaptor.magic = source.magic;
    adaptor.header = source.header;
    adaptor.header10 = source.header10;
    uint32_t const layers = adaptor.GetNumLayers();
    uint32_t const mips = adaptor.GetNumMips();
    adaptor.subresources.resize(static_cast<size_t>(layers) * mips);
    const unsigned char* base = source.bytes.data();
    for (uint32_t layer = 0; layer < layers; ++layer)
        for (uint32_t mip = 0; mip < mips; ++mip)
        {
            Span<unsigned char> sub = source.GetSubresource(mip, layer);
            FBlobRef ref{};
            ref.offset = static_cast<uint64_t>(sub.data() - base);
            ref.storedSize = sub.size_bytes();
            ref.decodedSize = sub.size_bytes();
            ref.codec = FBlobCodec::None;
            adaptor.subresources[adaptor.GetSubresourceIndex(layer, mip)] = ref;
        }
    return adaptor;
}

static void TransitionTextureLayout(RHICommandList* cmd, RHITexture* texture, FTextureHeader const& metadata,
                                    RHITextureLayout from, RHITextureLayout to)
{
    auto range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color, 0, metadata.GetNumMips(), 0,
                                                    texture->mDesc.arrayLayers);
    cmd->BeginTransition();
    cmd->SetImageTransition(texture, {.srcImgLayout = from, .dstImgLayout = to, .srcImgRange = range});
    cmd->EndTransition();
}

size_t GPUSceneImpl::StageTextureSubresource(ImmediateUpload::UploadBatch* batch, FSerializedTexture const& source,
                                             RHITexture* texture, uint32_t layer, uint32_t mip,
                                             Vector<BlobCopyTask>& outWrites)
{
    CHECK(texture != nullptr);
    CHECK_MSG(source.IsValid(), "Serialized texture is invalid");

    FTextureHeader const& metadata = static_cast<FTextureHeader const&>(source);
    FBlobRef const& subresourceBlob = source.GetSubresourceBlob(layer, mip);
    size_t const subresourceSize = metadata.GetSubresourceSize(layer, mip);
    CHECK_MSG(subresourceBlob.decodedSize == subresourceSize,
              "Serialized texture subresource size mismatch: layer {}, mip {}, blob {}, expected {}", layer, mip,
              subresourceBlob.decodedSize, subresourceSize);

    uint32_t const alignment = GetTextureUploadAlignment(metadata);
    char* preflight = reinterpret_cast<char*>(AlignUp(reinterpret_cast<uintptr_t>(batch->ptr), alignment));
    if (preflight >= batch->end || static_cast<size_t>(batch->end - preflight) < subresourceSize)
        return 0;

    CHECK(batch->Align(alignment));
    RHIExtent3D const mipExtent = metadata.GetMipExtent(mip);
    char* ptr = batch->Upload(
        texture, subresourceSize,
        {.aspect = RHITextureAspectFlagBits::Color, .mipLevel = mip, .baseArrayLayer = layer, .layerCount = 1},
        {0, 0, 0}, mipExtent);
    CHECK(ptr != nullptr);
    outWrites.push_back({subresourceBlob, ptr, subresourceSize});
    return subresourceSize;
}

GPUScene::Result GPUSceneImpl::Upload(FBlobDeserializer* blobs, FSerializedMesh const& source,
                                      GeometryHandle& outHandle)
{
    CHECK(blobs != nullptr);
    PendingGeometryUpload pending;
    {
        std::lock_guard<Mutex> lock(mUploadStateMutex);
        GSMesh header{};
        uint32_t offset = 0;
        Result r = ReserveMesh(source, header, offset);
        if (r != Result::InProgress)
            return r;
        uint32_t slot = FreelistPop(mGeometry, mGeometryFreelist);
        if (slot == UINT32_MAX)
            return Result::OutOfMemory;
        Geometry& g = mGeometry[slot];
        uint32_t version = g.version;
        g = Geometry{};
        g.version = version;
        g.type = kGSInstanceTypeMesh;
        g.blas = UINT32_MAX;
        g.offset = offset;
        g.mesh = header;
        g.state = ResourceState::Queued;
        g.live = true;
        outHandle = {slot, version};
        pending = {.handle = outHandle,
                   .blobs = *blobs,
                   .mesh = &source,
                   .curve = nullptr,
                   .footprint = mDirectGeometryUpload ? 1 : GPUScene::CalculateMeshPrimitiveSize(source)};
    }
    EnqueueUpload(mUploadGeometryQueue, std::move(pending));
    return Result::InProgress;
}

GPUScene::Result GPUSceneImpl::Upload(FBlobDeserializer* blobs, FSerializedCurve const& source,
                                      GeometryHandle& outHandle)
{
    CHECK(blobs != nullptr);
    PendingGeometryUpload pending;
    {
        std::lock_guard<Mutex> lock(mUploadStateMutex);
        GSCurveSet header{};
        uint32_t offset = 0;
        Result r = ReserveCurve(source, header, offset);
        if (r != Result::InProgress)
            return r;
        uint32_t slot = FreelistPop(mGeometry, mGeometryFreelist);
        if (slot == UINT32_MAX)
            return Result::OutOfMemory;
        Geometry& g = mGeometry[slot];
        uint32_t version = g.version;
        g = Geometry{};
        g.version = version;
        g.type = kGSInstanceTypeCurve;
        g.blas = UINT32_MAX;
        g.offset = offset;
        g.curve = header;
        g.state = ResourceState::Queued;
        g.live = true;
        outHandle = {slot, version};
        const size_t footprint = GPUScene::CalculateCurvePrimitiveSize(source);
        pending = {.handle = outHandle,
                   .blobs = *blobs,
                   .mesh = nullptr,
                   .curve = &source,
                   .footprint = mDirectGeometryUpload ? 1 : footprint};
    }
    EnqueueUpload(mUploadGeometryQueue, std::move(pending));
    return Result::InProgress;
}

GPUScene::Result GPUSceneImpl::Upload(FBlobDeserializer* blobs, FSerializedTexture const& source,
                                      TextureHandle& outTextureIndex, const char* debugName, bool pinned)
{
    CHECK(blobs != nullptr);
    if (!source.IsValid())
        return Result::InvalidInput;
    FTextureHeader const& metadata = static_cast<FTextureHeader const&>(source);
    bool const is3D = IsTexture3DView(metadata.GetViewDimension());
    auto texture = mDevice->CreateTexture(metadata.GetDesc());
    if (debugName)
        texture->DebugSetObjectName(debugName);
    auto view = texture->CreateTextureView(
        {.format = metadata.GetFormat(),
         .dimension = metadata.GetViewDimension(),
         .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color, 0, metadata.GetNumMips(), 0,
                                                     texture->mDesc.arrayLayers)});
    PendingTextureUpload pending;
    {
        std::lock_guard<Mutex> lock(mUploadStateMutex);
        BindlessPool& pool = TexturePool(is3D);
        Vector<Texture>& slots = TextureSlots(is3D);
        if (!outTextureIndex.IsValid())
        {
            uint32_t slot = pool.Allocate(std::move(texture), std::move(view));
            if (slot >= slots.size())
                slots.resize(slot + 1);
            slots[slot].live = true;
            slots[slot].pinned = pinned;
            slots[slot].resident = false;
            outTextureIndex = {slot, slots[slot].version, is3D};
        }
        else
        {
            CHECK_MSG(outTextureIndex.is3D == is3D && outTextureIndex.index < slots.size() &&
                          slots[outTextureIndex.index].live &&
                          slots[outTextureIndex.index].version == outTextureIndex.version,
                      "Upload in-place update on a stale texture handle (slot {}, version {})", outTextureIndex.index,
                      outTextureIndex.version);
            slots[outTextureIndex.index].pinned = pinned;
            slots[outTextureIndex.index].resident = false; // re-upload: not Ready until the drain completes
            pool.Update(outTextureIndex.index, std::move(texture), std::move(view));
        }
        pending = {.index = outTextureIndex.index, .blobs = *blobs, .source = &source};
    }
    EnqueueUpload(mUploadTextureQueue, std::move(pending));
    return Result::InProgress;
}

GPUScene::Result GPUSceneImpl::Upload(FTexture const& source, TextureHandle& outTextureIndex, const char* debugName,
                                      bool pinned)
{
    if (!source.IsValid())
        return Result::InvalidInput;
    FSerializedTexture adaptor = SerializedFromTexture(source, mAllocator);
    FBlobDeserializer blobs(Span<const unsigned char>(source.bytes.data(), source.bytes.size()));
    Result r = Upload(&blobs, adaptor, outTextureIndex, debugName, pinned);
    // Lifetime for the serialized blob must outlive the upload itself, so we can't really do this asynchronously within
    // this helper func. See also @ref FImportedMesh upload next, same thing here.
    // TODO: Just offer SerializedFromTexture, etc as public APIs...
    if (r != Result::InProgress)
        return r;
    Join();
    return Result::Ready;
}

GPUScene::Result GPUSceneImpl::Upload(FImportedMesh const& source, GeometryHandle& outHandle, FUUID skeleton)
{
    if (source.vertices.empty())
        return Result::InvalidInput;

    auto [adaptor, payload] = SerializedFromMesh(source, mAllocator, skeleton);
    FBlobDeserializer blobs(Span<const unsigned char>(payload.data(), payload.size()));
    Result r = Upload(&blobs, adaptor, outHandle);
    if (r != Result::InProgress)
        return r;
    Join();
    return Result::Ready;
}

GPUScene::Result GPUSceneImpl::Upload(RHIBuffer* dst, Span<const unsigned char> data, uint32_t dstOffset)
{
    if (!dst || data.empty())
        return Result::InvalidInput;
    PendingBufferUpload pending{dst, dstOffset, Vector<unsigned char>(mAllocator)};
    pending.data.assign(data.begin(), data.end());
    EnqueueUpload(mUploadBufferQueue, std::move(pending));
    return Result::InProgress;
}

GPUScene::Result GPUSceneImpl::Query(GeometryHandle handle) const
{
    Geometry const* g = ResolveGeometry(handle);
    if (!g)
        return Result::InvalidHandle;
    switch (g->state)
    {
    case ResourceState::Ready:
        return Result::Ready;
    case ResourceState::Failed:
        return Result::DecodeFailed;
    default:
        return Result::InProgress;
    }
}

GPUScene::Result GPUSceneImpl::Query(TextureHandle texture) const
{
    Vector<Texture> const& slots = TextureSlots(texture.is3D);
    if (!texture.IsValid() || texture.index >= slots.size() || !slots[texture.index].live ||
        slots[texture.index].version != texture.version)
        return Result::InvalidHandle;
    std::lock_guard<Mutex> lock(mUploadStateMutex);
    return slots[texture.index].resident ? Result::Ready : Result::InProgress;
}

template <typename T>
void GPUSceneImpl::EnqueueUpload(MPMCQueue<T>& queue, T&& item)
{
    mUploadPending.fetch_add(1, std::memory_order_release);
    while (!queue.Push(std::move(item)))
        std::this_thread::yield();
}

bool GPUSceneImpl::StartUploadBatch()
{
    if (mActiveUpload || mUploadFailed.load(std::memory_order_acquire))
        return false;

    auto state = ConstructUnique<UploadBatchState>(mAllocator, mAllocator);
    PendingGeometryUpload g;
    while (mUploadGeometryQueue.Pop(g))
        state->geometry.push_back(std::move(g));
    PendingTextureUpload t;
    while (mUploadTextureQueue.Pop(t))
        state->textures.push_back(std::move(t));
    PendingBufferUpload b;
    while (mUploadBufferQueue.Pop(b))
        state->buffers.push_back(std::move(b));

    state->itemCount = state->geometry.size() + state->textures.size() + state->buffers.size();
    if (state->itemCount == 0)
        return false;

    size_t stagingBudget = 0;
    if (!mDirectGeometryUpload)
        for (PendingGeometryUpload const& pending : state->geometry)
            stagingBudget += pending.footprint;
    for (PendingTextureUpload const& pending : state->textures)
    {
        FTextureHeader const& metadata = static_cast<FTextureHeader const&>(*pending.source);
        for (uint32_t layer = 0; layer < metadata.GetNumLayers(); ++layer)
            for (uint32_t mip = 0; mip < metadata.GetNumMips(); ++mip)
                stagingBudget += GPUSceneTextureSubresourceFootprint(metadata, layer, mip) + kUploadBudgetSlack;
    }
    for (PendingBufferUpload const& pending : state->buffers)
        stagingBudget += pending.data.size();
    stagingBudget = std::max<size_t>(stagingBudget, 1u);

    if (!mImmediateUpload || mImmediateUploadCapacity < stagingBudget)
    {
        if (mImmediateUpload)
            mImmediateUpload->WaitIdle();
        mImmediateUploadCapacity = std::bit_ceil(stagingBudget);
        mImmediateUpload = ConstructUnique<ImmediateUpload>(
            mAllocator, mDevice, mImmediateUploadCapacity, RHIDeviceQueueType::Transfer, kUploadStagingBuffers);
    }
    size_t taskCount = 0;
    for (PendingGeometryUpload const& pending : state->geometry)
    {
        if (pending.mesh)
            taskCount += 5u + (!pending.mesh->lods.empty() ? 1u : 0u);
        else
            taskCount += 3u;
    }
    for (PendingTextureUpload const& pending : state->textures)
    {
        FTextureHeader const& metadata = static_cast<FTextureHeader const&>(*pending.source);
        taskCount += static_cast<size_t>(metadata.GetNumLayers()) * metadata.GetNumMips();
    }

    UploadBatchState* batchState = state.get();
    state->prepareJob = mJobs->CreateJob(
        "GPUScenePrepare", [this, batchState] { PrepareUploads(*batchState); }, 1);
    state->prepareGate = state->prepareJob.AdoptDependencyGuard();

    size_t const chunkCount =
        std::min(taskCount, std::max<size_t>(mJobs->GetMaxConcurrency() * 4u, 1u));
    Vector<JobHandle> decodeJobs(mAllocator);
    decodeJobs.reserve(chunkCount);
    for (size_t chunk = 0; chunk < chunkCount; ++chunk)
    {
        decodeJobs.push_back(mJobs->CreateJobAfter(
            "GPUSceneDecode", Span<const JobHandle>(&state->prepareJob, 1),
            [this, batchState, chunk, chunkCount](JobContext& context)
            {
                size_t const count = batchState->writes.size();
                size_t const begin = count * chunk / chunkCount;
                size_t const end = count * (chunk + 1u) / chunkCount;
                DecodeUploads(*batchState, begin, end, context);
            }));
    }
    state->decodeJob = decodeJobs.empty()
        ? mJobs->CreateJobAfter("GPUSceneDecode", Span<const JobHandle>(&state->prepareJob, 1), [] {})
        : mJobs->CreateJobAfter("GPUSceneDecode", decodeJobs, [] {});
    state->submitJob = mJobs->CreateJobAfter(
        "GPUSceneSubmit", Span<const JobHandle>(&state->decodeJob, 1),
        [this, batchState] { SubmitUploads(*batchState); });
    state->commitJob =
        mJobs->CreateJob("GPUSceneCommit", [this, batchState] { CommitUploads(*batchState); }, 1);
    state->publishGate = state->commitJob.AdoptDependencyGuard();
    mActiveUpload = std::move(state);
    mActiveUpload->prepareGate.Release();
    return true;
}

bool GPUSceneImpl::DriveUploadBatch(size_t timeout)
{
    if (!mActiveUpload)
        return false;
    UploadBatchState& state = *mActiveUpload;

    if (state.submitJob.IsDone() && !state.submitted.load(std::memory_order_acquire))
    {
        if (state.batch.IsValid())
            state.batch.Abort();
        state.failed.store(true, std::memory_order_release);
    }

    if (state.submitted.load(std::memory_order_acquire) && !state.publicationReleased)
    {
        state.gpuComplete = !state.finalTimeline || state.finalTimelineValue == 0;
        if (!state.gpuComplete)
        {
            RHIDeviceQueue::TimelinePair wait{state.finalTimeline, state.finalTimelineValue};
            state.gpuComplete = mDevice->WaitForTimelineSemaphores(
                Span<const RHIDeviceQueue::TimelinePair>(&wait, 1), timeout);
        }
        if (state.gpuComplete && state.needsCompaction && !state.failed.load(std::memory_order_acquire))
        {
            SubmitBLASCompaction(state);
            state.gpuComplete = false;
            return false;
        }
        if (state.gpuComplete && !state.failed.load(std::memory_order_acquire))
        {
            state.publicationReleased = true;
            state.publishGate.Release();
        }
    }

    if (state.failed.load(std::memory_order_acquire))
    {
        if (state.submitted.load(std::memory_order_acquire) && !state.gpuComplete)
            return false;
        if (!state.publicationReleased)
        {
            mJobs->Cancel(state.commitJob);
            state.publishGate.Release();
            state.publicationReleased = true;
        }
        {
            std::lock_guard<Mutex> lock(mUploadStateMutex);
            for (GeometryHandle handle : state.geometryHandles)
                if (Geometry* geometry = ResolveGeometry(handle))
                    geometry->state = ResourceState::Failed;
        }
        mUploadPending.fetch_sub(state.itemCount, std::memory_order_release);
        mUploadFailed.store(true, std::memory_order_release);
        mActiveUpload.reset();
        return true;
    }

    if (state.publicationReleased && state.commitJob.IsDone())
    {
        if (state.commitJob.Status() != JobStatus::Completed)
            mUploadFailed.store(true, std::memory_order_release);
        mActiveUpload.reset();
        return true;
    }
    return false;
}

void GPUSceneImpl::Join()
{
    while (true)
    {
        JobBarrier barrier;
        {
            std::lock_guard<Mutex> lock(mUploadDriveMutex);
            if (mUploadFailed.load(std::memory_order_acquire))
                return;
            if (!mActiveUpload)
                StartUploadBatch();
            if (!mActiveUpload)
            {
                if (mUploadPending.load(std::memory_order_acquire) == 0)
                    return;
                std::this_thread::yield();
                continue;
            }

            DriveUploadBatch(static_cast<size_t>(-1));
            if (!mActiveUpload)
                continue;

            barrier = mJobs->CreateBarrier();
            UploadBatchState& state = *mActiveUpload;
            if (!state.submitted.load(std::memory_order_acquire))
                barrier.Add(state.submitJob);
            else if (state.publicationReleased && !state.commitJob.IsDone())
                barrier.Add(state.commitJob);
        }
        if (!barrier.IsEmpty())
            mJobs->Wait(barrier);
        else
            std::this_thread::yield();
    }
}

GPUScene::Result GPUSceneImpl::Poll()
{
    std::lock_guard<Mutex> lock(mUploadDriveMutex);
    if (mUploadFailed.load(std::memory_order_acquire))
        return Result::SubmitFailed;
    if (mActiveUpload)
        DriveUploadBatch(0);
    if (!mActiveUpload)
        StartUploadBatch();
    if (mUploadFailed.load(std::memory_order_acquire))
        return Result::SubmitFailed;
    return mUploadPending.load(std::memory_order_acquire) == 0 ? Result::Ready : Result::InProgress;
}

void GPUSceneImpl::PrepareUploads(UploadBatchState& state)
{
    CHECK_MSG(mImmediateUpload->TryBeginBatch(state.batch), "No reusable GPUScene upload lane");
    size_t maxBlob = 0;
    auto IncludeBlob = [&](FBlobRef const& blob)
    {
        if (blob.codec != FBlobCodec::None)
            maxBlob = std::max(maxBlob, static_cast<size_t>(blob.decodedSize));
    };
    for (PendingGeometryUpload const& pending : state.geometry)
    {
        if (pending.mesh)
        {
            IncludeBlob(pending.mesh->vertices);
            if (!pending.mesh->lods.empty())
                IncludeBlob(pending.mesh->lods[0].indices);
            IncludeBlob(pending.mesh->dagGroups);
            IncludeBlob(pending.mesh->dagMeshlets);
            IncludeBlob(pending.mesh->dagMeshletVtx);
            IncludeBlob(pending.mesh->dagMeshletTri);
        }
        else
        {
            IncludeBlob(pending.curve->vertices);
            IncludeBlob(pending.curve->indices);
            IncludeBlob(pending.curve->leaves);
        }
    }
    for (PendingTextureUpload const& pending : state.textures)
    {
        FTextureHeader const& metadata = static_cast<FTextureHeader const&>(*pending.source);
        for (uint32_t layer = 0; layer < metadata.GetNumLayers(); ++layer)
            for (uint32_t mip = 0; mip < metadata.GetNumMips(); ++mip)
                IncludeBlob(pending.source->GetSubresourceBlob(layer, mip));
    }

    size_t const workerCount = mJobs->GetMaxConcurrency();
    size_t const laneBudget = std::max<size_t>(maxBlob + kUploadBudgetSlack, alignof(std::max_align_t));
    state.scratchArena = mAllocator->AllocateArena(laneBudget * workerCount);
    CHECK(state.scratchArena.memory != nullptr);
    state.scratchArenas.resize(workerCount);
    state.scratchAllocators.reserve(workerCount);
    char* scratch = static_cast<char*>(state.scratchArena.memory);
    for (size_t i = 0; i < workerCount; ++i)
    {
        state.scratchArenas[i] = {scratch + i * laneBudget, laneBudget};
        state.scratchAllocators.push_back(
            ConstructUnique<AllocatorStack>(mAllocator, state.scratchArenas[i]));
    }

    Vector<size_t> order(state.geometry.size(), mAllocator);
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return state.geometry[a].footprint > state.geometry[b].footprint; });
    for (size_t index : order)
    {
        PendingGeometryUpload& pending = state.geometry[index];
        Geometry* geometry = ResolveGeometry(pending.handle);
        CHECK_MSG(geometry, "Pending geometry references a freed slot");
        size_t const firstWrite = state.writes.size();
        size_t const staged = pending.mesh
            ? StageMesh(&state.batch, *pending.mesh, geometry->mesh, geometry->offset, state.writes)
            : StageCurve(&state.batch, *pending.curve, geometry->curve, geometry->offset, state.writes);
        CHECK_MSG(staged != 0, "Staging buffer too small for geometry upload");
        for (size_t i = firstWrite; i < state.writes.size(); ++i)
            state.writes[i].blobs = &pending.blobs;
        state.geometryHandles.push_back(pending.handle);
    }
    for (PendingBufferUpload const& pending : state.buffers)
    {
        char* dst = state.batch.Upload(pending.dst, pending.data.size(), pending.dstOffset);
        CHECK_MSG(dst != nullptr, "Staging buffer too small for buffer upload");
        std::memcpy(dst, pending.data.data(), pending.data.size());
    }
    if (mDirectGeometryUpload)
        FlushDirectGeometryUpload();

    struct Subresource
    {
        size_t slot;
        uint32_t layer;
        uint32_t mip;
        size_t footprint;
    };
    Vector<Subresource> subresources(mAllocator);
    for (size_t slot = 0; slot < state.textures.size(); ++slot)
    {
        FTextureHeader const& metadata = static_cast<FTextureHeader const&>(*state.textures[slot].source);
        for (uint32_t layer = 0; layer < metadata.GetNumLayers(); ++layer)
            for (uint32_t mip = 0; mip < metadata.GetNumMips(); ++mip)
                subresources.push_back(
                    {slot, layer, mip, GPUSceneTextureSubresourceFootprint(metadata, layer, mip)});
    }
    std::sort(subresources.begin(), subresources.end(),
              [](Subresource const& a, Subresource const& b) { return a.footprint > b.footprint; });
    Vector<uint8_t> transitioned(state.textures.size(), 0u, mAllocator);
    for (Subresource const& subresource : subresources)
    {
        PendingTextureUpload& pending = state.textures[subresource.slot];
        FTextureHeader const& metadata = static_cast<FTextureHeader const&>(*pending.source);
        RHITexture* texture = SelectTexturePool(metadata.GetViewDimension()).GetResource(pending.index);
        CHECK(texture != nullptr);
        if (!transitioned[subresource.slot])
        {
            TransitionTextureLayout(state.batch.Get(), texture, metadata, RHITextureLayout::Undefined,
                                    RHITextureLayout::TransferDst);
            transitioned[subresource.slot] = 1u;
        }
        size_t const firstWrite = state.writes.size();
        size_t const staged = StageTextureSubresource(
            &state.batch, *pending.source, texture, subresource.layer, subresource.mip, state.writes);
        CHECK_MSG(staged != 0, "Staging buffer too small for texture subresource");
        for (size_t i = firstWrite; i < state.writes.size(); ++i)
            state.writes[i].blobs = &pending.blobs;
    }
    for (size_t slot = 0; slot < state.textures.size(); ++slot)
    {
        if (!transitioned[slot])
            continue;
        PendingTextureUpload& pending = state.textures[slot];
        FTextureHeader const& metadata = static_cast<FTextureHeader const&>(*pending.source);
        RHITexture* texture = SelectTexturePool(metadata.GetViewDimension()).GetResource(pending.index);
        TransitionTextureLayout(state.batch.Get(), texture, metadata, RHITextureLayout::TransferDst,
                                RHITextureLayout::ShaderReadOnly);
        state.uploadedTextures.emplace_back(pending.index, IsTexture3DView(metadata.GetViewDimension()));
    }
}

void GPUSceneImpl::DecodeUploads(UploadBatchState& state, size_t begin, size_t end, JobContext& context)
{
    AllocatorStack& scratch = *state.scratchAllocators[context.GetWorkerId()];
    for (size_t i = begin; i < end; ++i)
    {
        BlobCopyTask const& write = state.writes[i];
        CHECK(write.blobs != nullptr);
        if (write.blob.codec != FBlobCodec::None)
        {
            scratch.Reset(state.scratchArenas[context.GetWorkerId()]);
            write.blobs->ReadBytes(write.blob, write.dst, write.size, &scratch);
        }
        else
            write.blobs->ReadBytes(write.blob, write.dst, write.size, nullptr);
    }
}

void GPUSceneImpl::SubmitUploads(UploadBatchState& state)
{
    state.batch.End();
    state.finalTimeline = mImmediateUpload->CompletionTimeline();
    state.finalTimelineValue = state.batch.CompletionValue();
    if (!state.geometryHandles.empty())
    {
        RHIDeviceQueue::TimelinePair wait{state.finalTimeline, state.finalTimelineValue};
        RHIPipelineStage waitStage = RHIPipelineStageBits::AccelerationBuild;
        SubmitBLAS(state, {.timelineWaits = {&wait, 1}, .waitStages = {&waitStage, 1}});
    }
    state.submitted.store(true, std::memory_order_release);
}

void GPUSceneImpl::CommitUploads(UploadBatchState& state)
{
    std::lock_guard<Mutex> lock(mUploadStateMutex);
    CHECK(state.geometryHandles.size() == state.geometryBLAS.size());
    for (size_t i = 0; i < state.geometryHandles.size(); ++i)
    {
        if (Geometry* geometry = ResolveGeometry(state.geometryHandles[i]))
        {
            geometry->blas = state.geometryBLAS[i];
            geometry->state = ResourceState::Ready;
        }
    }
    for (auto const& [index, is3D] : state.uploadedTextures)
    {
        Vector<Texture>& slots = TextureSlots(is3D);
        if (index < slots.size())
            slots[index].resident = true;
    }
    mUploadPending.fetch_sub(state.itemCount, std::memory_order_release);
}

void GPUSceneImpl::SubmitBLAS(UploadBatchState& state, ImmediateSubmitDesc const& submitDesc)
{
    size_t const count = state.geometryHandles.size();
    Vector<RHIAccelerationStructureGeometryInfo> geometries(count, mAllocator);
    Vector<RHIAccelerationStructureBuildRangeInfo> ranges(count, mAllocator);
    Vector<RHIAccelerationStructureBuildDesc> builds(count, mAllocator);
    Vector<RHIAccelerationStructureSizeInfo> sizes(count, mAllocator);
    Vector<uint32_t> resultOffsets(count, mAllocator);
    Vector<uint32_t> scratchOffsets(count, mAllocator);
    Vector<uint8_t> curves(count, 0u, mAllocator);
    StackArena<4096> sizeArena;
    AllocatorStack sizeScratch(sizeArena);
    uint32_t meshBytes = 0;
    uint32_t curveBytes = 0;
    uint32_t scratchBytes = 0;

    for (size_t i = 0; i < count; ++i)
    {
        Geometry* geometry = ResolveGeometry(state.geometryHandles[i]);
        CHECK(geometry != nullptr);
        bool const curve = geometry->type == kGSInstanceTypeCurve;
        curves[i] = curve;
        RHIAccelerationStructureGeometryInfo& info = geometries[i];
        info.type = RHIAccelerationGeometryType::Triangles;
        if (curve)
        {
            GSCurveSet const& source = geometry->curve;
            info.triangleData = {
                .vertexFormat = RHIResourceFormat::R16G16B16SignedFloat,
                .vertexBuffer = owner.mPrimitiveBuffer.Get(),
                .vertexOffset = source.vertices.offset,
                .vertexCount = source.vertices.count,
                .vertexStride = sizeof(FCurveDOTSVertex),
                .indexFormat = RHIResourceFormat::R32Uint,
                .indexBuffer = owner.mPrimitiveBuffer.Get(),
                .indexOffset = source.indices.offset,
                .indexCount = source.indices.count,
            };
            ranges[i] = {.primitiveCount = source.indices.count / 3};
        }
        else
        {
            GSMesh const& source = geometry->mesh;
            info.triangleData = {
                .vertexFormat = RHIResourceFormat::R16G16B16SignedFloat,
                .vertexBuffer = owner.mPrimitiveBuffer.Get(),
                .vertexOffset = source.vertices.offset,
                .vertexCount = source.vertices.count,
                .vertexStride = sizeof(FQVertex),
                .indexFormat = RHIResourceFormat::R32Uint,
                .indexBuffer = owner.mPrimitiveBuffer.Get(),
                .indexOffset = source.indices.offset,
                .indexCount = source.indices.count,
            };
            ranges[i] = {.primitiveCount = source.indices.count / 3};
        }
        RHIAccelerationStructureBuildFlags const flags = curve
            ? RHIAccelerationStructureBuildFlagsBits::PreferFastTrace
            : RHIAccelerationStructureBuildFlagsBits::PreferFastTrace |
                RHIAccelerationStructureBuildFlagsBits::AllowUpdate |
                RHIAccelerationStructureBuildFlagsBits::AllowCompaction;
        builds[i] = {
            .type = RHIAccelerationStructureType::BottomLevel,
            .flags = flags,
            .operation = RHIAccelerationStructureBuildOp::Build,
            .geometries = {&geometries[i], 1},
            .ranges = {&ranges[i], 1},
        };
        sizeScratch.Reset(sizeArena);
        sizes[i] = mDevice->GetAccelerationStructureSizeInfo(builds[i], sizeScratch.Ptr());
        uint32_t& bytes = curve ? curveBytes : meshBytes;
        resultOffsets[i] = bytes = AlignUp(bytes, 256u);
        bytes += sizes[i].accelerationStructureSize;
        scratchOffsets[i] = scratchBytes = AlignUp(scratchBytes, 256u);
        scratchBytes += sizes[i].buildScratchSize;
    }

    RHIDeviceScopedHandle<RHIBuffer> meshBuffer;
    RHIDeviceScopedHandle<RHIBuffer> curveBuffer;
    auto CreateBLASBuffer = [&](uint32_t bytes)
    {
        return mDevice->CreateBuffer({
            .resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
            .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress |
                RHIBufferUsageBits::AccelerationStructureStorage,
            .size = bytes,
        });
    };
    if (meshBytes)
        meshBuffer = CreateBLASBuffer(meshBytes);
    if (curveBytes)
        curveBuffer = CreateBLASBuffer(curveBytes);
    state.blasScratch = mDevice->CreateBuffer({
        .resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
        .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress,
        .size = scratchBytes,
        .alignment = 256,
    });
    state.blasContext = ConstructUnique<ImmediateContext>(
        mAllocator, RHIDeviceQueueType::Compute, mDevice);
    RHICommandList* cmd = state.blasContext->Get();
    cmd->Begin();
    state.geometryBLAS.resize(count, UINT32_MAX);

    {
        std::lock_guard<Mutex> lock(mUploadStateMutex);
        if (meshBuffer)
            state.uncompactedBLASBuffer = std::move(meshBuffer);
        if (curveBuffer)
            mCurveBLASBuffers.push_back(std::move(curveBuffer));
        RHIBuffer* meshStorage = state.uncompactedBLASBuffer.Get();
        RHIBuffer* curveStorage = curveBytes ? mCurveBLASBuffers.back().Get() : nullptr;
        for (size_t i = 0; i < count; ++i)
        {
            bool const curve = curves[i] != 0;
            RHIAccelerationStructureDesc desc{
                .type = RHIAccelerationStructureType::BottomLevel,
                .flags = builds[i].flags,
                .buffer = curve ? curveStorage : meshStorage,
                .offset = resultOffsets[i],
                .size = sizes[i].accelerationStructureSize,
            };
            RHIAccelerationStructure* accelerationStructure = nullptr;
            if (curve)
            {
                uint32_t const slot = FreelistPop(mCurveBLASes, mCurveBLASFreelist);
                state.geometryBLAS[i] = slot;
                mCurveBLASes[slot] = mDevice->CreateAccelerationStructure(desc);
                accelerationStructure = mCurveBLASes[slot].Get();
            }
            else
            {
                state.meshGeometryIndices.push_back(i);
                state.uncompactedBLAS.push_back(mDevice->CreateAccelerationStructure(desc));
                accelerationStructure = state.uncompactedBLAS.back().Get();
            }
            builds[i].scratchBuffer = state.blasScratch.Get();
            builds[i].scratchBufferOffset = scratchOffsets[i];
            builds[i].dstAS = accelerationStructure;
            cmd->BuildAccelerationStructure({{{builds[i]}}});
        }
    }
    if (!state.uncompactedBLAS.empty())
    {
        state.compactionQueries = mDevice->CreateQueryPool(
            {.type = RHIDeviceQueryPool::QueryPoolDesc::AccelerationStructureCompactedSize,
             .count = static_cast<uint32_t>(state.uncompactedBLAS.size())});
        state.compactionQueries->Reset();
        cmd->BeginTransition();
        cmd->SetBufferTransition(
            state.uncompactedBLASBuffer.Get(),
            {.srcAccess = RHIResourceAccessBits::AccelerationStructureWrite,
             .dstAccess = RHIResourceAccessBits::AccelerationStructureRead,
             .srcStage = RHIPipelineStageBits::AccelerationBuild,
             .dstStage = RHIPipelineStageBits::AccelerationBuild});
        cmd->EndTransition();
        Vector<RHIAccelerationStructure*> pointers(mAllocator);
        pointers.reserve(state.uncompactedBLAS.size());
        for (auto const& accelerationStructure : state.uncompactedBLAS)
            pointers.push_back(accelerationStructure.Get());
        cmd->WriteAccelerationStructureCompactedSize(pointers, state.compactionQueries.Get(), 0);
        state.needsCompaction = true;
    }
    cmd->End();

    state.completionTimeline = mDevice->CreateSemaphore(true);
    RHIDeviceQueue::TimelinePair signal{state.completionTimeline.Get(), 1u};
    ImmediateSubmitDesc finalSubmit = submitDesc;
    finalSubmit.timelineSignals = {&signal, 1};
    state.blasContext->Submit(finalSubmit);
    state.finalTimeline = state.completionTimeline.Get();
    state.finalTimelineValue = 1u;
}

void GPUSceneImpl::SubmitBLASCompaction(UploadBatchState& state)
{
    CHECK(state.needsCompaction);
    auto compactSizes = state.compactionQueries->GetResults();
    CHECK(compactSizes.size() == state.meshGeometryIndices.size());
    Vector<uint32_t> offsets(compactSizes.size(), mAllocator);
    uint32_t compactBytes = 0;
    for (size_t i = 0; i < compactSizes.size(); ++i)
    {
        offsets[i] = compactBytes = AlignUp(compactBytes, 256u);
        compactBytes += static_cast<uint32_t>(compactSizes[i]);
    }
    auto compactBuffer = mDevice->CreateBuffer({
        .resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
        .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress |
            RHIBufferUsageBits::AccelerationStructureStorage,
        .size = compactBytes,
    });
    RHICommandList* cmd = state.blasContext->Get();
    cmd->Begin();
    {
        std::lock_guard<Mutex> lock(mUploadStateMutex);
        mBLASBuffers.push_back(std::move(compactBuffer));
        RHIBuffer* storage = mBLASBuffers.back().Get();
        for (size_t i = 0; i < state.meshGeometryIndices.size(); ++i)
        {
            size_t const geometryIndex = state.meshGeometryIndices[i];
            RHIAccelerationStructureDesc desc{
                .type = RHIAccelerationStructureType::BottomLevel,
                .flags = RHIAccelerationStructureBuildFlagsBits::PreferFastTrace |
                    RHIAccelerationStructureBuildFlagsBits::AllowCompaction,
                .buffer = storage,
                .offset = offsets[i],
                .size = static_cast<uint32_t>(compactSizes[i]),
            };
            uint32_t const slot = FreelistPop(mBLASes, mBLASFreelist);
            state.geometryBLAS[geometryIndex] = slot;
            mBLASes[slot] = mDevice->CreateAccelerationStructure(desc);
            cmd->CopyAccelerationStructure(state.uncompactedBLAS[i].Get(), mBLASes[slot].Get(), true);
        }
    }
    cmd->End();
    RHIDeviceQueue::TimelinePair signal{state.completionTimeline.Get(), 2u};
    state.blasContext->Submit({.timelineSignals = {&signal, 1}});
    state.finalTimelineValue = 2u;
    state.needsCompaction = false;
}

void GPUSceneImpl::AllocateDynamicBLAS(Geometry& g)
{
    CHECK(mDynamicPrimitiveBuffer);
    uint32_t const base = g.offset;
    RHIAccelerationStructureGeometryInfo geo{
        .type = RHIAccelerationGeometryType::Triangles,
        .triangleData = {
            .vertexFormat = RHIResourceFormat::R16G16B16SignedFloat,
            .vertexBuffer = mDynamicPrimitiveBuffer.Get(),
            .vertexOffset = base + static_cast<uint32_t>(sizeof(GSMesh)),
            .vertexCount = g.mesh.vertices.count,
            .vertexStride = sizeof(FQVertex),
            .indexFormat = RHIResourceFormat::R32Uint,
            .indexBuffer = mDynamicPrimitiveBuffer.Get(),
            .indexOffset = base + static_cast<uint32_t>(sizeof(GSMesh) + g.dynamicVtxBytes),
            .indexCount = g.mesh.indices.count,
        }};
    RHIAccelerationStructureBuildRangeInfo range{.primitiveCount = g.mesh.indices.count / 3};
    RHIAccelerationStructureBuildFlags const flags =
        RHIAccelerationStructureBuildFlagsBits::PreferFastBuild | RHIAccelerationStructureBuildFlagsBits::AllowUpdate;
    RHIAccelerationStructureBuildDesc desc{.type = RHIAccelerationStructureType::BottomLevel,
                                           .flags = flags,
                                           .operation = RHIAccelerationStructureBuildOp::Build,
                                           .geometries = Span<const RHIAccelerationStructureGeometryInfo>{&geo, 1},
                                           .ranges = Span<const RHIAccelerationStructureBuildRangeInfo>{&range, 1}};
    StackArena<4096> sizeInfoArena;
    AllocatorStack sizeInfoScratch(sizeInfoArena);
    auto size = mDevice->GetAccelerationStructureSizeInfo(desc, sizeInfoScratch.Ptr());
    uint32_t const scratchSize = AlignUp(std::max(size.buildScratchSize, size.updateScratchSize), 256u);
    g.dynamicBLASBuffer =
        mDevice->CreateBuffer({.resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
                               .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress |
                                   RHIBufferUsageBits::AccelerationStructureStorage,
                               .size = AlignUp(size.accelerationStructureSize, 256u)});
    g.dynamicBLASScratch =
        mDevice->CreateBuffer({.resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
                               .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress,
                               .size = scratchSize,
                               .alignment = 256});
    if (g.blas == UINT32_MAX)
        g.blas = FreelistPop(mBLASes, mBLASFreelist);
    mBLASes[g.blas] = mDevice->CreateAccelerationStructure({.type = RHIAccelerationStructureType::BottomLevel,
                                                            .flags = flags,
                                                            .buffer = g.dynamicBLASBuffer.Get(),
                                                            .offset = 0,
                                                            .size = size.accelerationStructureSize});
    g.dynamicIsBuilt = false;
}

GPUScene::Result GPUSceneImpl::Allocate(uint32_t vertexCount, uint32_t indexCount, GeometryHandle& outHandle,
                                        bool isGpu)
{
    if (!mDynamicPrimitiveBuffer)
    {
        LOG(GPUScene, LogError, "Allocate called but GPUSceneDesc::dynamicGeometryBudget is 0 (feature disabled)");
        return Result::InvalidInput;
    }
    if (vertexCount == 0 || indexCount == 0 || (indexCount % 3u) != 0u)
        return Result::InvalidInput;

    uint64_t const vtxBytes64 = static_cast<uint64_t>(vertexCount) * sizeof(FQVertex);
    uint64_t const idxBytes64 = static_cast<uint64_t>(indexCount) * sizeof(uint32_t);
    uint64_t const footprint64 = sizeof(GSMesh) + vtxBytes64 + idxBytes64;
    uint64_t const stride64 = AlignUp(footprint64, 16ull);
    if (vtxBytes64 > UINT32_MAX || idxBytes64 > UINT32_MAX || stride64 > UINT32_MAX)
        return Result::InvalidInput;

    uint32_t const vtxBytes = static_cast<uint32_t>(vtxBytes64);
    uint32_t const stride = static_cast<uint32_t>(stride64);

    GeometryHandle handle{};
    Geometry* gp = nullptr;
    {
        std::lock_guard<Mutex> lock(mUploadStateMutex);
        uint64_t base = mDynamicPrimitiveAlloc->Allocate(stride, 16);
        if (base == RHIVirtualAllocator::kInvalidOffset)
        {
            LOG(GPUScene, LogError, "Dynamic geometry overflow. Need {} bytes, {} used of {}", stride,
                mDynamicPrimitiveAlloc->GetUsedBytes(), mDynamicPrimitiveAlloc->GetCapacity());
            return Result::OutOfMemory;
        }
        uint32_t slot = FreelistPop(mGeometry, mGeometryFreelist);
        if (slot == UINT32_MAX)
        {
            mDynamicPrimitiveAlloc->Free(base);
            return Result::OutOfMemory;
        }
        Geometry& g = mGeometry[slot];
        uint32_t const version = g.version;
        g = Geometry{}; // releases any retained handles from a recycled slot
        g.version = version;
        g.type = kGSInstanceTypeMesh;
        g.dynamic = true;
        g.isGpu = isGpu;
        g.blas = UINT32_MAX;
        g.offset = static_cast<uint32_t>(base);
        g.dynamicVtxBytes = vtxBytes;
        g.mesh = GSMesh{};
        g.mesh.vertices.count = vertexCount;
        g.mesh.indices.count = indexCount;
        g.live = true;
        g.uploadHeader = true;
        g.state = ResourceState::Ready;
        handle = {slot, version};
        gp = &g;
        mDynamicGeometries.push_back(slot);
    }
    Geometry& g = *gp;

    g.mesh.vertices.offset = g.offset + sizeof(GSMesh);
    g.mesh.indices.offset = g.mesh.vertices.offset + g.dynamicVtxBytes;
    AllocateDynamicBLAS(g);
    outHandle = handle;
    return Result::Ready;
}

uint32_t GPUSceneImpl::AllocateDynamicStaging(uint32_t size, uint32_t alignment)
{
    CHECK_MSG(mDynamicStagingMapped, "CPU dynamic updates require GPUSceneDesc::dynamicStagingBudget");
    uint32_t const offset = AlignUp(mDynamicStagingCursor, alignment);
    CHECK_MSG(size <= mDynamicStagingFrameSize - std::min(offset, mDynamicStagingFrameSize),
              "Dynamic staging overflow. Need {} bytes, {} used of {}", size, offset, mDynamicStagingFrameSize);
    mDynamicStagingCursor = offset + size;
    return mDynamicStagingFrameIndex * mDynamicStagingFrameSize + offset;
}

void GPUSceneImpl::BeginDynamicGeometryUpdate()
{
    CHECK_MSG(!mDynamicIsUpdate, "BeginDynamicGeometryUpdate called while a window is already open");
    CHECK_MSG(mDynamicUploadRegions.empty(),
              "BeginDynamicGeometryUpdate called before pending CPU updates were uploaded");
    mDynamicIsUpdate = true;
    if (mDynamicStagingFrames != 0)
        mDynamicStagingFrameIndex = (mDynamicStagingFrameIndex + 1u) % mDynamicStagingFrames;
    mDynamicStagingCursor = 0;
}

void GPUSceneImpl::UpdateDynamicGeometryGPU(GeometryHandle handle, bool updateVertices, bool updateIndices)
{
    CHECK_MSG(mDynamicIsUpdate,
              "UpdateDynamicGeometryGPU must be called inside a BeginDynamicGeometryUpdate / EndDynamicGeometryUpdate "
              "window");
    Geometry* g = ResolveGeometry(handle);
    CHECK_MSG(g && g->dynamic, "UpdateDynamicGeometryGPU on a non-dynamic or invalid geometry handle");
    CHECK_MSG(g->isGpu, "UpdateDynamicGeometryGPU called on CPU-authored geometry");
    CHECK_MSG(updateVertices || updateIndices, "UpdateDynamicGeometryGPU requires at least one updated range");
    g->dirty = true;
    g->dynamicIndicesDirty |= updateIndices;
}

void GPUSceneImpl::UpdateDynamicGeometryCPU(GeometryHandle handle, Span<const FQVertex> vertices,
                                            Span<const uint32_t> indices)
{
    CHECK_MSG(mDynamicIsUpdate,
              "UpdateDynamicGeometryCPU must be called inside a BeginDynamicGeometryUpdate / EndDynamicGeometryUpdate "
              "window");
    Geometry* g = ResolveGeometry(handle);
    CHECK_MSG(g && g->dynamic, "UpdateDynamicGeometryCPU on a non-dynamic or invalid geometry handle");
    CHECK_MSG(!g->isGpu, "CPU-authored UpdateDynamicGeometryCPU called on GPU-authored geometry");
    CHECK_MSG(!vertices.empty() || !indices.empty(), "UpdateDynamicGeometryCPU requires at least one updated range");
    CHECK_MSG(vertices.empty() || vertices.size() == g->mesh.vertices.count,
              "Dynamic vertex update has {} vertices; expected {}", vertices.size(), g->mesh.vertices.count);
    CHECK_MSG(indices.empty() || indices.size() == g->mesh.indices.count,
              "Dynamic index update has {} indices; expected {}", indices.size(), g->mesh.indices.count);

    if (!vertices.empty())
    {
        uint32_t const size = static_cast<uint32_t>(vertices.size_bytes());
        uint32_t const srcOffset = AllocateDynamicStaging(size, alignof(FQVertex));
        std::memcpy(mDynamicStagingMapped + srcOffset, vertices.data(), size);
        mDynamicUploadRegions.push_back({.srcOffset = srcOffset, .dstOffset = g->mesh.vertices.offset, .size = size});
    }
    if (!indices.empty())
    {
        uint32_t const size = static_cast<uint32_t>(indices.size_bytes());
        uint32_t const srcOffset = AllocateDynamicStaging(size, alignof(uint32_t));
        std::memcpy(mDynamicStagingMapped + srcOffset, indices.data(), size);
        mDynamicUploadRegions.push_back({.srcOffset = srcOffset, .dstOffset = g->mesh.indices.offset, .size = size});
        g->dynamicIndicesDirty = true;
    }
    g->dirty = true;
}

void GPUSceneImpl::EndDynamicGeometryUpdate()
{
    CHECK_MSG(mDynamicIsUpdate, "EndDynamicGeometryUpdate called without a matching BeginDynamicGeometryUpdate");
    mDynamicIsUpdate = false;
}

void GPUSceneImpl::UploadDynamicGeometryCPU(RHICommandList* cmd)
{
    CHECK_MSG(!mDynamicIsUpdate,
              "UploadDynamicGeometryCPU recorded while a dynamic update is still in progress "
              "(call EndDynamicGeometryUpdate before the upload pass)");
    CHECK(cmd);
    if (!mDynamicPrimitiveBuffer || mDynamicGeometries.empty())
        return;

    for (uint32_t slot : mDynamicGeometries)
    {
        Geometry& g = mGeometry[slot];
        if (!g.live || !g.dynamic)
            continue;
        if (g.uploadHeader)
        {
            cmd->UpdateBuffer(mDynamicPrimitiveBuffer.Get(), g.offset, AsBytes(AsSpan(g.mesh)));
            g.uploadHeader = false;
        }
    }
    if (mDynamicUploadRegions.empty())
        return;
    CHECK(mDynamicStagingBuffer);
    cmd->CopyBuffer(mDynamicStagingBuffer.Get(), mDynamicPrimitiveBuffer.Get(), mDynamicUploadRegions);
    mDynamicUploadRegions.clear();
}

void GPUSceneImpl::BuildBLAS(RHICommandList* cmd)
{
    CHECK_MSG(!mDynamicIsUpdate,
              "BuildBLAS recorded while a dynamic update is still in progress "
              "(call EndDynamicGeometryUpdate before the refit pass)");
    mLastRefitCount = 0;
    mLastRebuildCount = 0;
    if (mDynamicGeometries.empty())
        return;
    bool any = false;
    for (uint32_t slot : mDynamicGeometries)
    {
        Geometry& g = mGeometry[slot];
        if (!g.live || !g.dynamic || g.blas == UINT32_MAX || !g.dirty)
            continue;
        bool const periodic = g.dynamicLastRebuildFrame >= kGPUSceneDynamicRebuildRate;
        bool const rebuild = !g.dynamicIsBuilt || g.dynamicIndicesDirty || periodic;
        if (rebuild)
            g.dynamicLastRebuildFrame = 0;
        else
            ++g.dynamicLastRebuildFrame;
        ++mLastRefitCount;
        mLastRebuildCount += rebuild ? 1u : 0u;
        uint32_t const base = g.offset;
        RHIAccelerationStructureGeometryInfo geo{
            .type = RHIAccelerationGeometryType::Triangles,
            .triangleData = {
                .vertexFormat = RHIResourceFormat::R16G16B16SignedFloat,
                .vertexBuffer = mDynamicPrimitiveBuffer.Get(),
                .vertexOffset = base + static_cast<uint32_t>(sizeof(GSMesh)),
                .vertexCount = g.mesh.vertices.count,
                .vertexStride = sizeof(FQVertex),
                .indexFormat = RHIResourceFormat::R32Uint,
                .indexBuffer = mDynamicPrimitiveBuffer.Get(),
                .indexOffset = base + static_cast<uint32_t>(sizeof(GSMesh) + g.dynamicVtxBytes),
                .indexCount = g.mesh.indices.count,
            }};
        RHIAccelerationStructureBuildRangeInfo range{.primitiveCount = g.mesh.indices.count / 3};
        RHIAccelerationStructureBuildDesc desc{.type = RHIAccelerationStructureType::BottomLevel,
                                               .flags = RHIAccelerationStructureBuildFlagsBits::PreferFastBuild |
                                                   RHIAccelerationStructureBuildFlagsBits::AllowUpdate,
                                               .operation = rebuild ? RHIAccelerationStructureBuildOp::Build
                                                                    : RHIAccelerationStructureBuildOp::Update,
                                               .geometries = Span<const RHIAccelerationStructureGeometryInfo>{&geo, 1},
                                               .ranges = Span<const RHIAccelerationStructureBuildRangeInfo>{&range, 1}};
        desc.scratchBuffer = g.dynamicBLASScratch.Get();
        desc.scratchBufferOffset = 0;
        desc.dstAS = mBLASes[g.blas].Get();
        if (!rebuild)
            desc.srcAS = mBLASes[g.blas].Get();
        cmd->BuildAccelerationStructure(Span<const RHIAccelerationStructureBuildDesc>{&desc, 1});
        g.dirty = false;
        g.dynamicIndicesDirty = false;
        g.dynamicIsBuilt = true;
        any = true;
    }
    if (!any)
        return;
    cmd->BeginTransition();
    for (uint32_t slot : mDynamicGeometries)
    {
        Geometry const& g = mGeometry[slot];
        if (g.live && g.dynamic && g.blas != UINT32_MAX)
            cmd->SetAccelerationStructureTransition(mBLASes[g.blas].Get(),
                                                    {.srcAccess = RHIResourceAccessBits::AccelerationStructureWrite,
                                                     .dstAccess = RHIResourceAccessBits::AccelerationStructureRead,
                                                     .srcStage = RHIPipelineStageBits::AccelerationBuild,
                                                     .dstStage = RHIPipelineStageBits::AccelerationBuild});
    }
    cmd->EndTransition();
}

GPUSceneImpl::Geometry* GPUSceneImpl::ResolveGeometry(GeometryHandle handle)
{
    if (!handle.IsValid() || handle.index >= mGeometry.size())
        return nullptr;
    Geometry& g = mGeometry[handle.index];
    if (!g.live || (handle.version != 0 && g.version != handle.version))
        return nullptr;
    return &g;
}

GPUSceneImpl::Geometry const* GPUSceneImpl::ResolveGeometry(GeometryHandle handle) const
{
    return const_cast<GPUSceneImpl*>(this)->ResolveGeometry(handle);
}

void GPUSceneImpl::FreeGeometry(uint32_t slot)
{
    CHECK(slot < mGeometry.size());
    Geometry& g = mGeometry[slot];
    if (!g.live)
        return;
    if (g.dynamic)
    {
        if (mDynamicPrimitiveAlloc)
            mDynamicPrimitiveAlloc->Free(g.offset);
        if (g.blas < mBLASes.size())
        {
            mBLASes[g.blas].Reset();
            FreelistPush(mBLASFreelist, g.blas);
        }
        g.dynamicBLASBuffer.Reset();
        g.dynamicBLASScratch.Reset();
        for (size_t i = 0; i < mDynamicGeometries.size(); ++i)
            if (mDynamicGeometries[i] == slot)
            {
                mDynamicGeometries[i] = mDynamicGeometries.back();
                mDynamicGeometries.pop_back();
                break;
            }
    }
    else
    {
        mPrimitiveAlloc->Free(g.offset);
        if (g.type == kGSInstanceTypeCurve)
        {
            if (g.blas < mCurveBLASes.size())
            {
                mCurveBLASes[g.blas].Reset();
                FreelistPush(mCurveBLASFreelist, g.blas);
            }
        }
        else if (g.blas < mBLASes.size())
        {
            mBLASes[g.blas].Reset();
            FreelistPush(mBLASFreelist, g.blas);
        }
    }
    g.live = false;
    g.dynamic = false;
    g.state = ResourceState::Queued;
    ++g.version;
    FreelistPush(mGeometryFreelist, slot);
}

void GPUSceneImpl::FreeTextureSlot(bool is3D, uint32_t slot)
{
    Vector<Texture>& slots = TextureSlots(is3D);
    CHECK(slot < slots.size());
    Texture& s = slots[slot];
    if (!s.live)
        return;
    TexturePool(is3D).Free(slot); // releases the bindless binding + owned resource
    s.live = false;
    ++s.version; // invalidate outstanding handles to this slot
}

void GPUSceneImpl::Collect()
{
    Join();
    {
        Vector<uint8_t> referenced(mGeometry.size(), 0u, mAllocator);
        for (GSInstance const& inst : owner.mCommittedInstances)
            if (inst.resourceIndex < referenced.size())
                referenced[inst.resourceIndex] = 1u;

        uint32_t freed = 0;
        for (uint32_t slot = 0; slot < mGeometry.size(); ++slot)
        {
            if (mGeometry[slot].live && !referenced[slot])
            {
                FreeGeometry(slot);
                ++freed;
            }
        }
        if (freed)
            LOG(GPUScene, LogDebug, "Collect freed {} unreferenced geometry resources", freed);
    }
    {
        Vector<uint8_t> referencedTex(mTexture2DSlots.size(), 0u, mAllocator);
        auto MarkTexture = [&](uint32_t index)
        {
            if (index != UINT32_MAX && index < referencedTex.size())
                referencedTex[index] = 1u;
        };
        for (GSMaterial const& m : owner.mCommittedMaterials)
        {
            MarkTexture(m.baseColorTexture);
            MarkTexture(m.emissiveTexture);
            MarkTexture(m.metallicRoughnessTexture);
            MarkTexture(m.normalTexture);
            MarkTexture(m.transmissionTexture);
            MarkTexture(m.specularTexture);
            MarkTexture(m.specularColorTexture);
            MarkTexture(m.anisotropyTexture);
            MarkTexture(m.sheenColorTexture);
            MarkTexture(m.sheenRoughnessTexture);
            MarkTexture(m.clearcoatTexture);
            MarkTexture(m.clearcoatRoughnessTexture);
        }
        uint32_t freedTex = 0;
        for (uint32_t slot = 0; slot < mTexture2DSlots.size(); ++slot)
        {
            Texture const& s = mTexture2DSlots[slot];
            if (s.live && !s.pinned && !referencedTex[slot])
            {
                FreeTextureSlot(false, slot);
                ++freedTex;
            }
        }
        if (freedTex)
            LOG(GPUScene, LogDebug, "Collect freed {} unreferenced textures", freedTex);
    }
}

uint32_t GPUSceneImpl::CountLiveInstances() const { return static_cast<uint32_t>(owner.mCommittedInstances.size()); }

uint32_t GPUSceneImpl::CountTLASInstances() const
{
    uint32_t numLightInstances = 0;
    for (const auto& light : owner.mCommittedLights)
    {
        if (IsIntersectableLight(light))
            numLightInstances++;
    }
    uint32_t numInstances = CountLiveInstances();
    CHECK_MSG(numInstances <= UINT32_MAX - numLightInstances,
              "TLAS instance count overflow: {} scene instances and {} light instances", numInstances,
              numLightInstances);
    return numInstances + numLightInstances;
}


void GPUSceneImpl::EnsureTLASCapacity(uint32_t totalInstances)
{
    if (totalInstances == 0)
        return;
    CHECK_MSG(totalInstances <= UINT32_MAX / mTLASInstanceStride,
              "TLAS instance byte count overflow: {} instances with stride {}", totalInstances, mTLASInstanceStride);
    RHIAccelerationStructureGeometryInstanceData instance{
        .instanceBuffer = mTLASInstances.mBuffer.Get(), .instanceOffset = 0, .totalPrimitives = totalInstances};
    RHIAccelerationStructureGeometryInfo geometry{.type = RHIAccelerationGeometryType::Instances,
                                                  .instanceData = instance};
    RHIAccelerationStructureBuildRangeInfo range{.primitiveCount = totalInstances};
    RHIAccelerationStructureBuildDesc desc{.type = RHIAccelerationStructureType::TopLevel,
                                           .flags = RHIAccelerationStructureBuildFlagsBits::PreferFastTrace |
                                               RHIAccelerationStructureBuildFlagsBits::AllowUpdate |
                                               RHIAccelerationStructureBuildFlagsBits::AllowCompaction,
                                           .operation = RHIAccelerationStructureBuildOp::Build,
                                           .geometries = Span<const RHIAccelerationStructureGeometryInfo>{&geometry, 1},
                                           .ranges = Span<const RHIAccelerationStructureBuildRangeInfo>{&range, 1}};
    StackArena<4096> sizeInfoArena;
    AllocatorStack sizeInfoScratch(sizeInfoArena);
    auto* device = mDevice;
    auto size = device->GetAccelerationStructureSizeInfo(desc, sizeInfoScratch.Ptr());
    size_t requiredTLASSize = static_cast<size_t>(AlignUp(size.accelerationStructureSize, 256u));
    size_t requiredScratchSize =
        static_cast<size_t>(AlignUp(std::max(size.buildScratchSize, size.updateScratchSize), 256u));
    CHECK_MSG(requiredTLASSize <= mTLASBuffer->mDesc.size && requiredScratchSize <= mScratchBufferTLAS->mDesc.size,
              "TLAS for {} instances needs {:.1f} MB (scratch {:.1f} MB) but the pre-allocated budget is "
              "{:.1f} MB (scratch {:.1f} MB). Increase GPUSceneDesc::tlasBudget / tlasScratchBudget.",
              totalInstances, requiredTLASSize / 1e6, requiredScratchSize / 1e6, mTLASBuffer->mDesc.size / 1e6,
              mScratchBufferTLAS->mDesc.size / 1e6);
}

GPUScene::TLASBuildResult GPUSceneImpl::BuildTLAS(RHICommandList* cmd, bool update)
{
    std::lock_guard<Mutex> residencyLock(mUploadStateMutex);
    auto GeometryReady = [&](uint32_t resourceIndex) -> bool
    {
        if (resourceIndex >= mGeometry.size())
            return false;
        Geometry const& g = mGeometry[resourceIndex];
        if (!g.live || g.state != ResourceState::Ready)
            return false;
        if (g.dynamic && !g.dynamicIsBuilt)
            return false;
        return g.blas != UINT32_MAX;
    };

    uint32_t capacityInstances = CountTLASInstances();
    EnsureTLASCapacity(capacityInstances);

    uint32_t lightInstances = 0;
    for (GSLight const& light : owner.mCommittedLights)
    {
        if (IsIntersectableLight(light))
            ++lightInstances;
    }
    uint32_t readyInstances = 0;
    for (GSInstance const& inst : owner.mCommittedInstances)
        if (GeometryReady(inst.resourceIndex))
            ++readyInstances;
    uint32_t totalInstances = readyInstances + lightInstances;
    if (totalInstances != owner.mLastTLASInstancesCount)
    {
        update = false;
        owner.mLastTLASInstancesCount = totalInstances;
    }

    auto ConvertInstance = [&](GSInstance const& src, uint32_t instanceID) -> RHIAccelerationStructureGeometryInstance
    {
        RHIAccelerationStructureGeometryInstance res{
            .instanceID = instanceID,
        };
        if (src.type == kGSInstanceTypeCurve)
        {
            res.mask = 0x04; // CURVE_MASK
            res.shaderBindingTableRecordOffset = kCurveSBTOffset;
            res.flags = RHIAccelerationGeometryInstanceFlagsBits::TriangleCullDisable;
        }
        else
        {
            res.mask = 0x01; // MESH_MASK
        }
        mat3 basis = transpose(mat3(scale(src.scale)) * mat3_cast(src.rotation));
        std::memcpy(res.transformBasisRowMajor[0], &basis[0], sizeof(float) * 3);
        std::memcpy(res.transformBasisRowMajor[1], &basis[1], sizeof(float) * 3);
        std::memcpy(res.transformBasisRowMajor[2], &basis[2], sizeof(float) * 3);
        res.transformTranslation[0] = src.transform.x;
        res.transformTranslation[1] = src.transform.y;
        res.transformTranslation[2] = src.transform.z;
        return res;
    };

    auto ConvertLight = [&](const GSLight* src, uint32_t lightIndex) -> RHIAccelerationStructureGeometryInstance
    {
        RHIAccelerationStructureGeometryInstance res{
            .instanceID = lightIndex | (1u << 23),
            .mask = 0x02, // LIGHT_MASK
        };
        uint32_t type = src->flags & kGSLightTypeMask;
        if (type == kGSLightTypePoint || type == kGSLightTypeSpot)
        {
            mat3 basis(src->params.x);
            std::memcpy(res.transformBasisRowMajor[0], &basis[0], sizeof(float) * 3);
            std::memcpy(res.transformBasisRowMajor[1], &basis[1], sizeof(float) * 3);
            std::memcpy(res.transformBasisRowMajor[2], &basis[2], sizeof(float) * 3);
            res.transformTranslation[0] = src->position.x;
            res.transformTranslation[1] = src->position.y;
            res.transformTranslation[2] = src->position.z;
            res.blas = mSphereBLAS.Get();
            res.shaderBindingTableRecordOffset = kSphereLightSBTOffset;
            return res;
        }

        float3 u = src->dpdu;
        float3 v = src->dpdv;
        if (type == kGSLightTypeDisk)
        {
            u *= src->params.x;
            v *= src->params.y;
        }
        float3 crossUV = cross(u, v);
        CHECK_MSG(length(crossUV) > 0.0f, "Area light has zero surface area");
        float3 n = normalize(crossUV);
        mat3 basis = transpose(mat3(u, v, n));
        std::memcpy(res.transformBasisRowMajor[0], &basis[0], sizeof(float) * 3);
        std::memcpy(res.transformBasisRowMajor[1], &basis[1], sizeof(float) * 3);
        std::memcpy(res.transformBasisRowMajor[2], &basis[2], sizeof(float) * 3);
        res.transformTranslation[0] = src->position.x;
        res.transformTranslation[1] = src->position.y;
        res.transformTranslation[2] = src->position.z;
        res.blas = (type == kGSLightTypeDisk) ? mDiskBLAS.Get() : mRectBLAS.Get();
        res.shaderBindingTableRecordOffset = (type == kGSLightTypeDisk) ? kDiskLightSBTOffset : kRectLightSBTOffset;
        return res;
    };

    owner.mTLASInstanceMap.clear();
    uint32_t instancesOffset = 0;
    if (totalInstances > 0)
    {
        auto [pInstances, offset] = mTLASInstances.Allocate(mTLASInstanceStride * totalInstances);
        instancesOffset = offset;
        for (uint32_t i = 0; i < owner.mCommittedInstances.size(); ++i)
        {
            GSInstance const& inst = owner.mCommittedInstances[i];
            if (!GeometryReady(inst.resourceIndex))
                continue;
            Geometry const* g = &mGeometry[inst.resourceIndex];
            auto data = ConvertInstance(inst, static_cast<uint32_t>(owner.mTLASInstanceMap.size()));
            data.blas = (g->type == kGSInstanceTypeCurve) ? mCurveBLASes[g->blas].Get() : mBLASes[g->blas].Get();
            pInstances += mDevice->WriteAccelerationStructureInstanceData(data, pInstances);
            owner.mTLASInstanceMap.push_back(i);
        }
        for (uint32_t i = 0; i < owner.mCommittedLights.size(); ++i)
        {
            GSLight const& light = owner.mCommittedLights[i];
            if (IsIntersectableLight(light))
            {
                auto data = ConvertLight(&light, i);
                pInstances += mDevice->WriteAccelerationStructureInstanceData(data, pInstances);
            }
        }
    }
    RHIAccelerationStructureGeometryInstanceData instance{.instanceBuffer = mTLASInstances.mBuffer.Get(),
                                                          .instanceOffset = instancesOffset,
                                                          .totalPrimitives = totalInstances};
    RHIAccelerationStructureGeometryInfo geometry{.type = RHIAccelerationGeometryType::Instances,
                                                  .instanceData = instance};
    RHIAccelerationStructureBuildRangeInfo range{.primitiveCount = totalInstances};
    RHIAccelerationStructureBuildFlags buildFlags = RHIAccelerationStructureBuildFlagsBits::PreferFastTrace |
        RHIAccelerationStructureBuildFlagsBits::AllowUpdate | RHIAccelerationStructureBuildFlagsBits::AllowCompaction;
    RHIAccelerationStructureBuildDesc desc{.type = RHIAccelerationStructureType::TopLevel,
                                           .flags = buildFlags,
                                           .operation = update ? RHIAccelerationStructureBuildOp::Update
                                                               : RHIAccelerationStructureBuildOp::Build,
                                           .geometries = Span<const RHIAccelerationStructureGeometryInfo>{&geometry, 1},
                                           .ranges = Span<const RHIAccelerationStructureBuildRangeInfo>{&range, 1}};
    desc.scratchBuffer = mScratchBufferTLAS.Get();
    desc.scratchBufferOffset = 0;
    desc.dstAS = owner.mTLAS.Get();
    if (update)
        desc.srcAS = owner.mTLAS.Get();
    cmd->BuildAccelerationStructure({{{desc}}});
    return TLASBuildResult::Built;
}

GPUScene::Result GPUSceneImpl::UploadEnvironmentMap(FTexture const& source)
{
    uint32_t width = source.GetWidth();
    uint32_t height = source.GetHeight();
    Span<const unsigned char> data = source.GetSubresource(0, 0);
    const float4* pixels = reinterpret_cast<const float4*>(data.data());

    Vector<float> f(width * height, mAllocator);
    size_t const grain = std::max<size_t>(
        1u, height / std::max<size_t>(mJobs->GetMaxConcurrency() * 4u, 1u));
    mJobs->Wait(mJobs->ParallelFor(
        ExecutionPolicy::Par, "EnvLuminance", height, grain,
        [&](size_t begin, size_t end, JobContext&)
        {
            for (size_t y = begin; y < end; ++y)
            {
                float v = (static_cast<float>(y) + 0.5f) / height;
                float sinTheta = std::sin(pi<float>() * v);
                for (uint32_t x = 0; x < width; ++x)
                {
                    float4 pixel = pixels[y * width + x];
                    float luminance = max(pixel.x, max(pixel.y, pixel.z));
                    f[y * width + x] = luminance * sinTheta;
                }
            }
        }));

    PiecewiseConstant2D cdf(f, width, height, mAllocator);
    owner.mEnvMapAverageRadiance = cdf.Int() * pi<float>() / 2.0f;

    auto UploadR32Texture = [this](Span<const float> values, uint32_t texWidth, uint32_t texHeight,
                                   TextureHandle& outTexture, const char* debugName) -> Result
    {
        FTexture texture(mAllocator);
        texture.Initialize(RHIResourceFormat::R32SignedFloat, RHITextureDimension::E2D, texWidth, texHeight);
        texture.bytes.resize(values.size_bytes());
        std::memcpy(texture.bytes.data(), values.data(), values.size_bytes());
        return Upload(texture, outTexture, debugName, true);
    };

    uint32_t marginalWidth = static_cast<uint32_t>(cdf.mMarginal->mCDF.size());
    if (Result r = UploadR32Texture(cdf.mMarginal->mCDF, marginalWidth, 1u, owner.mEnvMapMarginalCDFIndex,
                                    "Environment Map Marginal CDF");
        r != Result::Ready)
        return r;

    uint32_t conditionalWidth = static_cast<uint32_t>(cdf.mConditional[0]->mCDF.size());
    Vector<float> conditionalCDF(static_cast<size_t>(height) * conditionalWidth, mAllocator);

    for (uint32_t y = 0; y < height; ++y)
    {
        std::memcpy(conditionalCDF.data() + static_cast<size_t>(y) * conditionalWidth, cdf.mConditional[y]->mCDF.data(),
                    cdf.mConditional[y]->mCDF.size() * sizeof(float));
    }

    if (Result r = UploadR32Texture(conditionalCDF, conditionalWidth, height, owner.mEnvMapConditionalCDFIndex,
                                    "Environment Map Conditional CDF");
        r != Result::Ready)
        return r;

    float3 shCoeffs[9];
    PrefilterEnvmapSH9(source, shCoeffs, mJobs, mAllocator);
    for (int i = 0; i < 9; ++i)
        owner.mEnvSHCoeffs[static_cast<size_t>(i)] = shCoeffs[i];

    FTexture prefiltered = PrefilterEnvmapSpecular(source, mJobs, mAllocator);
    owner.mEnvMapPrefilteredMips = prefiltered.GetNumMips();
    if (Result r = Upload(prefiltered, owner.mEnvMapIndex, "Environment Map", true); r != Result::Ready)
        return r;

    LOG(GPUScene, LogInfo, "Environment map uploaded: {}x{} ({} prefilter mips)", source.GetWidth(), source.GetHeight(),
        owner.mEnvMapPrefilteredMips);
    return Result::Ready;
}

static RHITexture* ResolvePoolTexture(BindlessPool& pool, uint32_t index)
{
    if (index == UINT32_MAX)
        return nullptr;
    return pool.GetResource(index);
}

RHITexture* GPUScene::GetFoundationDefaultTexture2D() const
{
    return ResolvePoolTexture(const_cast<BindlessPool&>(mImpl->mTexture2DPool), mFoundationDefaultTexture2DIndex.index);
}

RHITexture* GPUScene::GetFoundationDefaultTexture2DFloat() const
{
    return ResolvePoolTexture(const_cast<BindlessPool&>(mImpl->mTexture2DPool),
                              mFoundationDefaultTexture2DFloatIndex.index);
}

void GPUSceneImpl::Reset()
{
    Join();
    if (mImmediateUpload)
        mImmediateUpload->WaitIdle();
    mActiveUpload.reset();
    PendingGeometryUpload g;
    while (mUploadGeometryQueue.Pop(g))
    {
    }
    PendingTextureUpload t;
    while (mUploadTextureQueue.Pop(t))
    {
    }
    PendingBufferUpload b;
    while (mUploadBufferQueue.Pop(b))
    {
    }
    mUploadPending.store(0, std::memory_order_release);
    mUploadFailed.store(false, std::memory_order_release);
    mDynamicStagingFrameIndex = 0;
    mDynamicStagingCursor = 0;
    mDynamicUploadRegions.clear();
    mDynamicIsUpdate = false;
    mBLASes.clear();
    mBLASBuffers.clear();
    mBLASFreelist.clear();
    mCurveBLASes.clear();
    mCurveBLASBuffers.clear();
    mCurveBLASFreelist.clear();
    mGeometry.clear();
    owner.mCommittedInstances.clear();
    owner.mCommittedLights.clear();
    owner.mCommittedMaterials.clear();
    owner.mTLASInstanceMap.clear();
    mMaterialBuffer.Reset();
    mInstanceBuffer.Reset();
    mLightBuffer.Reset();
    mLightBVHNodeBuffer.Reset();
    mLightBVHLightIndexBuffer.Reset();
    mLightBVHBitmaskBuffer.Reset();
    mLightBVHGlobalIndexBuffer.Reset();
    mLightBVHNodeIndexBuffer.Reset();
    mLightBVHRefitLevels.clear();
    mLightBVHNeedsRefit = false;
    owner.mLastUpdateResult = {};
    mMeshletGlobalCounter = 0;
    owner.mLastTLASInstancesCount = 0;
    mPrimitiveAlloc->Clear();
    if (mDynamicPrimitiveAlloc)
        mDynamicPrimitiveAlloc->Clear();
    mDynamicGeometries.clear();
    owner.mEnvMapPrefilteredMips = 0u;
    for (auto& c : owner.mEnvSHCoeffs)
        c = float3(0.0f);
}

GPUScene::Result GPUScene::Upload(FBlobDeserializer* blobs, FSerializedMesh const& source, GeometryHandle& outHandle)
{
    return mImpl->Upload(blobs, source, outHandle);
}

GPUScene::Result GPUScene::Upload(FImportedMesh const& source, GeometryHandle& outHandle, FUUID skeleton)
{
    return mImpl->Upload(source, outHandle, skeleton);
}

GPUScene::Result GPUScene::Upload(FBlobDeserializer* blobs, FSerializedCurve const& source, GeometryHandle& outHandle)
{
    return mImpl->Upload(blobs, source, outHandle);
}

GPUScene::Result GPUScene::Allocate(uint32_t vertexCount, uint32_t indexCount, GeometryHandle& outHandle, bool isGpu)
{
    return mImpl->Allocate(vertexCount, indexCount, outHandle, isGpu);
}

GPUScene::Result GPUScene::Upload(FBlobDeserializer* blobs, FSerializedTexture const& source, TextureHandle& outTexture,
                                  const char* debugName, bool pinned)
{
    return mImpl->Upload(blobs, source, outTexture, debugName, pinned);
}

GPUScene::Result GPUScene::Upload(FTexture const& source, TextureHandle& outTexture, const char* debugName, bool pinned)
{
    return mImpl->Upload(source, outTexture, debugName, pinned);
}

GPUScene::Result GPUScene::Upload(RHIBuffer* dst, Span<const unsigned char> data, uint32_t dstOffset)
{
    return mImpl->Upload(dst, data, dstOffset);
}

GPUScene::Result GPUScene::Query(GeometryHandle handle) const { return mImpl->Query(handle); }
GPUScene::Result GPUScene::Query(TextureHandle texture) const { return mImpl->Query(texture); }
void GPUScene::Join() { mImpl->Join(); }
GPUScene::Result GPUScene::Poll() { return mImpl->Poll(); }
GPUScene::Result GPUScene::UploadEnvironmentMap(FTexture const& source) { return mImpl->UploadEnvironmentMap(source); }

GPUScene::GPUSceneTables GPUScene::BeginScene(uint32_t instanceCount, uint32_t materialCount, uint32_t lightCount)
{
    return mImpl->BeginScene(instanceCount, materialCount, lightCount);
}
void GPUScene::ResolveGeometry(GeometryHandle handle, uint32_t& outPrimitiveOffset, uint32_t& outPrimitiveType,
                               GSInstanceFlags& outPrimitiveFlags) const
{
    GPUSceneImpl::Geometry* g = mImpl->ResolveGeometry(handle);
    CHECK_MSG(g, "ResolveGeometry on an invalid geometry handle");
    outPrimitiveOffset = g->offset;
    outPrimitiveType = g->type;
    outPrimitiveFlags = g->dynamic ? GSInstanceFlagsBits::Dynamic : GSInstanceFlagsBits{};
}
GPUScene::UpdateResult GPUScene::EndScene(GPUSceneTables& tables) { return mImpl->EndScene(tables); }
void GPUScene::DbgGetMemoryStatistics(Vector<MemoryStat>& outStats) const { mImpl->DbgGetMemoryStatistics(outStats); }
String GPUScene::DbgGetBufferStatistics() const { return mImpl->DbgGetBufferStatistics(); }
GPUScene::TLASBuildResult GPUScene::BuildTLAS(RHICommandList* cmd, bool update)
{
    return mImpl->BuildTLAS(cmd, update);
}
void GPUScene::Collect() { mImpl->Collect(); }
void GPUScene::Reset() { mImpl->Reset(); }

bool GPUScene::HasDynamicGeometry() const { return mImpl->HasDynamicGeometry(); }
bool GPUScene::HasCurveGeometry() const { return mImpl->HasCurveGeometry(); }
void GPUScene::BeginDynamicGeometryUpdate() { mImpl->BeginDynamicGeometryUpdate(); }
void GPUScene::UpdateDynamicGeometryGPU(GeometryHandle handle, bool updateVertices, bool updateIndices)
{
    mImpl->UpdateDynamicGeometryGPU(handle, updateVertices, updateIndices);
}
void GPUScene::UpdateDynamicGeometryCPU(GeometryHandle handle, Span<const FQVertex> vertices,
                                        Span<const uint32_t> indices)
{
    mImpl->UpdateDynamicGeometryCPU(handle, vertices, indices);
}
void GPUScene::EndDynamicGeometryUpdate() { mImpl->EndDynamicGeometryUpdate(); }
void GPUScene::UploadDynamicGeometryCPU(RHICommandList* cmd) { mImpl->UploadDynamicGeometryCPU(cmd); }
void GPUScene::BuildBLAS(RHICommandList* cmd) { mImpl->BuildBLAS(cmd); }
uint32_t GPUScene::GetDynamicRefitCount() const { return mImpl->mLastRefitCount; }
uint32_t GPUScene::GetDynamicRebuildCount() const { return mImpl->mLastRebuildCount; }

RHIBuffer* GPUScene::GetDynamicPrimitiveBuffer() const
{
    return mImpl->mDynamicPrimitiveBuffer ? mImpl->mDynamicPrimitiveBuffer.Get() : mPrimitiveBuffer.Get();
}
RHIBuffer* GPUScene::GetDynamicStagingBuffer() const
{
    return mImpl->mDynamicStagingBuffer ? mImpl->mDynamicStagingBuffer.Get() : nullptr;
}
RHIBuffer* GPUScene::GetInstanceBuffer() const { return mImpl->mInstanceBuffer.mBuffer.Get(); }
RHIBuffer* GPUScene::GetMaterialBuffer() const { return mImpl->mMaterialBuffer.mBuffer.Get(); }
RHIBuffer* GPUScene::GetLightBuffer() const { return mImpl->mLightBuffer.mBuffer.Get(); }
RHIBuffer* GPUScene::GetLightBVHNodeBuffer() const { return mImpl->mLightBVHNodeBuffer.mBuffer.Get(); }
RHIBuffer* GPUScene::GetLightBVHLightIndexBuffer() const { return mImpl->mLightBVHLightIndexBuffer.mBuffer.Get(); }
RHIBuffer* GPUScene::GetLightBVHBitmaskBuffer() const { return mImpl->mLightBVHBitmaskBuffer.mBuffer.Get(); }
RHIBuffer* GPUScene::GetLightBVHGlobalIndexBuffer() const { return mImpl->mLightBVHGlobalIndexBuffer.mBuffer.Get(); }
RHIBuffer* GPUScene::GetLightBVHNodeIndexBuffer() const { return mImpl->mLightBVHNodeIndexBuffer.mBuffer.Get(); }
bool GPUScene::NeedsLightBVHRefit() const { return mImpl->mLightBVHNeedsRefit; }
uint32_t GPUScene::GetLightBVHRefitLevelCount() const
{
    return static_cast<uint32_t>(mImpl->mLightBVHRefitLevels.size());
}
uint32_t GPUScene::GetLightBVHRefitLevelOffset(uint32_t level) const
{
    CHECK(level < mImpl->mLightBVHRefitLevels.size());
    return mImpl->mLightBVHRefitLevels[level].offset;
}
uint32_t GPUScene::GetLightBVHRefitLevelNodeCount(uint32_t level) const
{
    CHECK(level < mImpl->mLightBVHRefitLevels.size());
    return mImpl->mLightBVHRefitLevels[level].count;
}
uint32_t GPUScene::GetLightBVHFirstNodeIndex() const { return mLastUpdateResult.lightBVH.nodeIndices.offset; }
GSOffsetCount GPUScene::GetLightBVHNodes() const { return mLastUpdateResult.lightBVH.nodes; }
GSOffsetCount GPUScene::GetLightBVHDistantNodes() const { return mLastUpdateResult.lightBVH.distantNodes; }
BindlessPool* GPUScene::GetTexture2DPool() { return &mImpl->mTexture2DPool; }
BindlessPool* GPUScene::GetTexture3DPool() { return &mImpl->mTexture3DPool; }
BindlessPool const* GPUScene::GetTexture2DPool() const { return &mImpl->mTexture2DPool; }
BindlessPool const* GPUScene::GetTexture3DPool() const { return &mImpl->mTexture3DPool; }
uint32_t GPUScene::GetLightCapacity() const { return mImpl->mLightBuffer.Capacity(); }
