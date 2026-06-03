#include "GPUScene.hpp"
#include "Renderer.hpp"
#include "Precompute.hpp"
#include "Tables/GGX.hpp"
#include "Tables/GGX_IOR.hpp"
#include "Tables/LTCSheen.hpp"
#include "Tables/Sobol.hpp"
#include "Tables/ViewLUTs.hpp"
#include <Core/Allocator.hpp>
#include <Core/AllocatorStack.hpp>
#include <Core/Atomic.hpp>
#include <Core/AtomicQueue.hpp>
#include <Core/Paths.hpp>
#include <Core/Thread.hpp>
#include <Core/ThreadPool.hpp>
#include <bit>
#include <condition_variable>

// Per-resource staging footprint slack, mirrored from the previous Editor scheduler.
static constexpr size_t kUploadBudgetSlack = 1ull * (1ull << 20);
static constexpr size_t kUploadStagingBudgetSlack = 32ull * (1ull << 20);
static constexpr size_t kUploadStagingBuffers = 3u;
// Plain device-local buffer copies are rare (Sobol, default buffers, occasional refreshes);
// a small fixed-capacity submission queue is plenty.
static constexpr size_t kGPUSceneBufferQueueCapacity = 256u;

static size_t GPUSceneTextureSubresourceFootprint(FTextureHeader const& metadata, uint32_t layer, uint32_t mip)
{
    uint32_t const alignment = std::max(metadata.GetBpp() / 8, metadata.GetBlockSize());
    CHECK_MSG(alignment != 0, "Unsupported texture format {}", metadata.GetFormat());
    size_t const size = metadata.GetSubresourceSize(layer, mip);
    CHECK_MSG(size <= std::numeric_limits<size_t>::max() - (alignment - 1u),
              "Texture subresource staging footprint exceeds addressable range");
    return size + alignment - 1u;
}

// Pending-job counter helpers (a job batch is "done" when the counter reaches zero).
static void GPUSceneCompleteJob(Atomic<size_t>* counter)
{
    if (!counter)
        return;
    size_t const previous = counter->fetch_sub(1, std::memory_order_release);
    CHECK_MSG(previous > 0, "GPUScene upload job counter underflow");
    if (previous == 1)
        counter->notify_all();
}

static void GPUSceneWaitJobs(Atomic<size_t>* counter)
{
    size_t pending = counter->load(std::memory_order_acquire);
    while (pending != 0)
    {
        counter->wait(pending, std::memory_order_relaxed);
        pending = counter->load(std::memory_order_acquire);
    }
}

static FTexture LoadLUT(Allocator* allocator, StringView path)
{
    FTexture tex(allocator);
    String resolvedPath = PathsResolve(path);
    LoadDDS(tex, resolvedPath);
    return tex;
}

static FTexture MakeLUT(const float* data, RHIResourceFormat format, uint32_t width, uint32_t height = 1,
                          uint32_t depth = 1, RHITextureDimension dimension = RHITextureDimension::E2D)
{
    FTexture tex(GLOBAL_ALLOC);
    tex.Initialize(format, dimension, width, height, depth);
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    tex.bytes.assign(bytes, bytes + tex.GetSize());
    return tex;
}

static constexpr size_t kMinDirectGeometryUploadHeapSize = 512ull * (1ull << 20);
static constexpr uint32_t kGPUScenePersistentTexture3DBindings = 2u; // default SDR/HDR view LUTs.
static constexpr uint32_t kGPUSceneTextureBindingSlack = 8u;
static constexpr size_t kGPUSceneByteBudgetSlack = 64u << 10u;

/** @brief One deferred blob payload write: decode @ref blob into the mapped destination @ref dst. */
struct GPUSceneBlobWrite
{
    FBlobRef blob{};
    char* dst{nullptr};
    size_t size{0};
};

BITMASK_ENUM_BEGIN(GSData, uint8_t)
    Mesh = 1 << 0
BITMASK_ENUM_END()

#pragma pack(push, 1)
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
#pragma pack(pop)
static_assert(sizeof(GSCurveSet) == 24);
static_assert(sizeof(GSCurvePoint) == 16);
static_assert(sizeof(GSCurveSegment) == 16);

struct GPUSceneGeometry
{
    uint32_t type{kGSInstanceTypeMesh};
    uint32_t resourceOffset{0}; // Header byte offset in the primitive buffer.
    GSMesh mesh{};
    GSCurveSet curve{};
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
 * @brief All implementation state and machinery owned by @ref GPUScene.
 * @note Lives entirely in GPUScene.cpp via @ref GPUScene::mImpl. The committed-snapshot
 *       tables, the LUT/env bindless indices and the plain primitive/TLAS/sobol/default
 *       buffer handles remain direct members of the facade (so the hot read getters stay
 *       inline); this struct reaches them through @ref owner.
 */
struct GPUSceneImpl
{
    // Re-export the facade's public nested types so member signatures read naturally.
    using Result = GPUScene::Result;
    using UpdateResult = GPUScene::UpdateResult;
    using GPUSceneTables = GPUScene::GPUSceneTables;
    using MemoryStat = GPUScene::MemoryStat;
    using TLASBuildResult = GPUScene::TLASBuildResult;

    GPUScene& owner; // Back-reference to the facade-resident state (committed tables, handles).

    RHIDevice* mDevice{nullptr};
    Allocator* mAllocator{GLOBAL_ALLOC};
    AllocatorStack* mFrameScratch{nullptr};
    /* Geometry */
    char* mPrimitiveMapped{nullptr};
    bool mDirectGeometryUpload{false};
    // VMA-backed byte suballocator over the facade's primitive buffer (upload & free at will).
    RHIDeviceScopedHandle<RHIVirtualAllocator> mPrimitiveAlloc;
    // --- Dynamic (CPU-updateable) geometry ring. Host-coherent, persistently mapped, and
    //     AS-build-readable, so the BLAS refit + (eventually) shading read positions straight
    //     out of the current frame slot with no staging copy. Sized dynamicGeometryBudget per
    //     slot, replicated across mDynamicFrameCount slots. The virtual allocator hands out
    //     per-geo intra-slot regions (same offset in every slot). Null when the feature is off. ---
    RHIDeviceScopedHandle<RHIBuffer> mDynamicPrimitiveBuffer;
    char* mDynamicPrimitiveMapped{nullptr};
    RHIDeviceScopedHandle<RHIVirtualAllocator> mDynamicPrimitiveAlloc;
    uint32_t mDynamicFrameCount{0}; // ring slots (frames in flight)
    uint32_t mDynamicFrameSlot{0};  // slot the CPU writes / the refit reads this frame
    bool mDynamicUpdateOpen{false}; // inside a BeginDynamicGeometryUpdate / End window
    uint32_t mDynamicRebuildCadence{64}; // frames between forced full rebuilds (0 = refit only)
    uint32_t mLastRefitCount{0};    // dirty geos refitted in the last RefitDynamicGeometry call
    uint32_t mLastRebuildCount{0};  // of those, how many were full rebuilds
    Vector<uint32_t> mDynamicGeometrySlots; // live dynamic geometry residency slots (refit set)
    // For @ref meshletGlobalIndex
    uint32_t mMeshletGlobalCounter{0};
    UploadGPURingBuffer<GSInstance> mInstanceBuffer;
    UploadGPURingBuffer<GSMaterial> mMaterialBuffer;
    UploadGPURingBuffer<GSLight> mLightBuffer;
    UploadGPURingBuffer<GSAlias> mLightAliasTableBuffer;
    /* Textures */
    BindlessPool mTexture2DPool;
    BindlessPool mTexture3DPool;
    /**
     * @brief Per-bindless-slot residency backing @ref TextureHandle (one table per pool,
     *        since the 2D/3D pools have independent slot spaces).
     * @note `generation` is bumped on free so stale handles fail @ref Query; `pinned`
     *       marks GPUScene-owned singletons (LUTs / defaults / env map) that @ref Collect
     *       must never reclaim.
     */
    struct TextureSlot
    {
        uint32_t generation{0};
        bool live{false};
        bool pinned{false};
        bool resident{false}; // image contents uploaded + transitioned to ShaderReadOnly (guarded by mResidencyMutex)
    };
    Vector<TextureSlot> mTexture2DSlots;
    Vector<TextureSlot> mTexture3DSlots;
    [[nodiscard]] Vector<TextureSlot>& TextureSlots(bool is3D) { return is3D ? mTexture3DSlots : mTexture2DSlots; }
    [[nodiscard]] Vector<TextureSlot> const& TextureSlots(bool is3D) const { return is3D ? mTexture3DSlots : mTexture2DSlots; }
    [[nodiscard]] BindlessPool& TexturePool(bool is3D) { return is3D ? mTexture3DPool : mTexture2DPool; }
    // Frees a live texture slot (pool binding + owned resource) and bumps its generation.
    void FreeTextureSlot(bool is3D, uint32_t slot);
    [[nodiscard]] BindlessPool& SelectTexturePool(RHITextureDimension viewDimension);
    [[nodiscard]] BindlessPool const& SelectTexturePool(RHITextureDimension viewDimension) const;
    /* AS */
    // BLAS
    Vector<RHIDeviceScopedHandle<RHIAccelerationStructure>> mBLASes;
    Vector<RHIDeviceScopedHandle<RHIBuffer>> mBLASBuffers;
    Vector<uint32_t> mFreeBLASSlots;
    Vector<RHIDeviceScopedHandle<RHIAccelerationStructure>> mCurveBLASes;
    Vector<RHIDeviceScopedHandle<RHIBuffer>> mCurveBLASBuffers;
    Vector<uint32_t> mFreeCurveBLASSlots;
    RHIDeviceScopedHandle<RHIBuffer> mCurveAABBBuffer;
    char* mCurveAABBMapped{nullptr};
    // VMA-backed byte suballocator over mCurveAABBBuffer.
    RHIDeviceScopedHandle<RHIVirtualAllocator> mCurveAABBAlloc;
    uint32_t AcquireMeshBLASSlot();
    uint32_t AcquireCurveBLASSlot();

    enum class ResourceState : uint8_t { Queued, Uploading, Ready, Failed };

    /* Geometry residency (handle-owned). Slots are recycled with a bumped generation. */
    struct GeometryResidency
    {
        uint32_t generation{0};
        uint32_t type{kGSInstanceTypeMesh};
        uint32_t blasSlot{UINT32_MAX};
        uint32_t resourceOffset{0}; // Header byte offset in primitive buffer (static); slot-0 base in the dynamic ring (dynamic).
        GSMesh mesh{};
        GSCurveSet curve{};
        ResourceState state{ResourceState::Queued};
        bool live{false};
        // --- CPU-updateable dynamic geometry (see Upload). Dynamic geo bypasses the async
        //     worker, the immutable primitive pool, compaction and DAG/meshlets entirely. ---
        bool dynamic{false};
        uint32_t dynamicFootprint{0}; // bytes per frame slot (GSMesh header + verts + indices)
        uint32_t dynamicStride{0};    // per-slot stride (footprint aligned up); slot s base = resourceOffset + s*stride
        uint32_t dynamicVtxBytes{0};  // FQVertex bytes (the part rewritten each frame)
        uint32_t dynamicIdxBytes{0};  // LOD0 UINT32 index bytes (topology, written once per slot)
        bool dirty{false};            // verts rewritten this frame -> needs BLAS refit
        bool dynBuilt{false};         // the AllowUpdate BLAS has been GPU-built at least once
        uint32_t framesSinceRebuild{0}; // periodic full-rebuild cadence (refit quality decay)
        // The single AllowUpdate BLAS (in mBLASes[blasSlot]) plus its retained backing + scratch.
        RHIDeviceScopedHandle<RHIBuffer> dynBLASBuffer;
        RHIDeviceScopedHandle<RHIBuffer> dynScratchBuffer;
    };
    Vector<GeometryResidency> mGeometry;
    Vector<uint32_t> mFreeGeometrySlots;

    [[nodiscard]] GeometryResidency* ResolveGeometry(GeometryHandle handle);
    [[nodiscard]] GeometryResidency const* ResolveGeometry(GeometryHandle handle) const;
    uint32_t AcquireGeometrySlot();
    void FreeGeometry(uint32_t slot);
    void BuildBLAS(ImmediateContext* ctx, Span<const GSMesh> meshes, Span<uint32_t> outBLASIndices,
                   ImmediateSubmitDesc const& firstSubmitDesc = {});
    void BuildCurveBLAS(ImmediateContext* ctx, Span<const GSCurveSet> curves, Span<uint32_t> outBLASIndices,
                        ImmediateSubmitDesc const& firstSubmitDesc = {});

    /* --- Private upload work queue (owned staging / decode / transfer / BLAS) --- */
    /**
     * @brief A geometry resource reserved by @ref Upload and finalized by @ref Join / @ref Poll.
     * @note Final primitive/AABB memory is allocated up front; staging, blob decode,
     *       transfer and BLAS build run later when the queue is drained.
     */
    struct PendingGeometryUpload
    {
        GeometryHandle handle{};
        FBlobDeserializer blobs{Span<const unsigned char>{}};
        const FSerializedMesh* mesh{nullptr};
        const FSerializedCurve* curve{nullptr};
        size_t footprint{0}; // staging bytes (primitive [+ curve AABB]); 0/1 when direct-mapped.
    };
    struct PendingTextureUpload
    {
        uint32_t index{UINT32_MAX};
        FBlobDeserializer blobs{Span<const unsigned char>{}};
        const FSerializedTexture* source{nullptr};
    };
    /** @brief A plain device-local buffer copy; the payload is owned so the caller's source can go. */
    struct PendingBufferUpload
    {
        RHIBuffer* dst{nullptr};
        uint32_t dstOffset{0};
        // Default member init keeps the struct default-constructible (the MPMCQueue default-
        // constructs its slots); real uploads move in a payload built on the scene allocator.
        Vector<unsigned char> data{GLOBAL_ALLOC};
    };
    /* --- Async upload worker (multi-producer, single-consumer) ---
     * Producers (any thread) enqueue work via @ref Upload onto the lock-free queues; a single
     * persistent worker drains them, uploads on the transfer/compute queues, and publishes
     * residency while the renderer consumes the scene concurrently. @ref mResidencyMutex
     * guards the residency views the render thread reads each frame (geometry
     * @ref ResourceState / BLAS slots, texture residency); the BLAS/residency vectors are
     * reserved to budget up front so they never reallocate under those concurrent reads. */
    MPMCQueue<PendingGeometryUpload> mGeometryQueue;
    MPMCQueue<PendingTextureUpload> mTextureQueue;
    MPMCQueue<PendingBufferUpload> mBufferQueue;
    Thread mUploadThread;
    Mutex mWorkMutex;               // backs the worker wake / drained CVs and the flags below
    CondVar mWorkCV;                // wakes the worker on new work or stop
    CondVar mDoneCV;                // wakes Join() once outstanding work hits zero
    bool mHasWork{false};
    bool mStop{false};
    bool mWorkerStarted{false};
    Atomic<size_t> mOutstanding{0}; // enqueued-but-not-yet-resident items (drives Poll/Join)
    Atomic<bool> mUploadFailed{false};
    mutable Mutex mResidencyMutex;

    /** @brief Drains all currently-queued uploads into one batch and processes it (returns false if nothing was queued). */
    bool DrainUploadBatch();
    /** @brief Persistent worker entry point: drains batches, sleeping while the queues are empty. */
    void UploadWorker();
    /** @brief Enqueues one item, bumping the outstanding count and waking the worker. */
    template <typename T> void EnqueueUpload(MPMCQueue<T>& queue, T&& item);

    /** @brief Reserves final resident memory and computes the shader header (no staging yet). */
    Result ReserveMesh(FSerializedMesh const& src, GSMesh& outHeader, uint32_t& outOffset);
    Result ReserveCurve(FSerializedCurve const& src, GSCurveSet& outHeader, uint32_t& outOffset);
    /**
     * @brief Stages a reserved resource into the upload context: writes the shader header
     *        inline and emits blob-decode jobs for its payloads.
     * @return Bytes staged, or 0 when the staging lane is full (caller flushes and retries).
     */
    size_t StageMesh(ImmediateUpload* ctx, FSerializedMesh const& src, GSMesh const& header,
                     uint32_t offset, Vector<GPUSceneBlobWrite>& outWrites);
    size_t StageCurve(ImmediateUpload* ctx, FSerializedCurve const& src, GSCurveSet const& header,
                      uint32_t offset, Vector<GPUSceneBlobWrite>& outWrites);
    /** @brief Stages one texture subresource (emits a blob-decode job); 0 when the lane is full. */
    size_t StageTextureSubresource(ImmediateUpload* ctx, FSerializedTexture const& source, RHITexture* texture,
                                   uint32_t layer, uint32_t mip, Vector<GPUSceneBlobWrite>& outWrites);
    /**
     * @brief Drains the pending upload queues: best-fit staging packing, threaded blob
     *        decode, transfer submission, BLAS build, residency patching, Ready marking.
     */
    /**
     * @brief Drains one batch of queued uploads: best-fit staging packing, threaded blob
     *        decode, transfer submission, BLAS build, residency patching, Ready marking.
     */
    void ProcessUploads(Vector<PendingGeometryUpload>& geometry, Vector<PendingTextureUpload>& textures,
                        Vector<PendingBufferUpload>& buffers);
    void FlushDirectGeometryUpload();

    /* --- Table ring allocation --- */
    Pair<GSInstance*, uint32_t> AllocateInstance(uint32_t count);
    Pair<GSMaterial*, uint32_t> AllocateMaterial(uint32_t count);
    Pair<GSLight*, uint32_t> AllocateLight(uint32_t count);
    Pair<GSAlias*, uint32_t> AllocateLightAliasTable(uint32_t count);
    struct OpenTables
    {
        bool open{false};
        uint32_t firstAliasTable{0};
        GSAlias* aliasPtr{nullptr};
        GSInstance* instancePtr{nullptr}; // Ring destination for the translated instances.
        uint32_t instanceCount{0};
    } mOpenTables;
    // Caller-facing InstanceDesc scratch handed out by BeginScene; persists until EndScene.
    Vector<InstanceDesc> mInstanceScratch;
    // TLAS
    uint32_t mTLASInstanceStride{0}; // In bytes, read only once
    RHIDeviceScopedHandle<RHIBuffer> mTLASBuffer, mScratchBufferTLAS;
    UploadGPURingBuffer<char> mTLASInstances;
    uint32_t CountLiveInstances() const;
    uint32_t CountTLASInstances() const;
    // Validates the TLAS fits the pre-allocated buffers (never grows them); aborts if exceeded.
    void EnsureTLASCapacity(uint32_t totalInstances);

    // Light BLAS
    RHIDeviceScopedHandle<RHIBuffer> mLightGeometryBuffer;
    RHIDeviceScopedHandle<RHIAccelerationStructure> mRectBLAS;
    RHIDeviceScopedHandle<RHIAccelerationStructure> mDiskBLAS;
    RHIDeviceScopedHandle<RHIBuffer> mLightBLASBuffer;

    GPUSceneImpl(GPUScene& owner, RHIDevice* device, Allocator* allocator, GPUSceneDesc const& desc,
                 AllocatorStack* frameScratch);
    ~GPUSceneImpl();

    /* --- Methods backing the facade forwarders (heavy bodies live in GPUScene.cpp) --- */
    Result Upload(FBlobDeserializer* blobs, FSerializedMesh const& source, GeometryHandle& outHandle);
    Result Upload(FBlobDeserializer* blobs, FSerializedCurve const& source, GeometryHandle& outHandle);
    Result UploadDynamic(FBlobDeserializer* blobs, FSerializedMesh const& source, GeometryHandle& outHandle);
    Result Upload(FBlobDeserializer* blobs, FSerializedTexture const& source, TextureHandle& outTexture,
                  const char* debugName = nullptr, bool pinned = false);
    Result Upload(FTexture const& source, TextureHandle& outTexture, const char* debugName = nullptr,
                  bool pinned = false);
    Result Upload(RHIBuffer* dst, Span<const unsigned char> data, uint32_t dstOffset = 0);
    [[nodiscard]] Result Query(GeometryHandle handle) const;
    [[nodiscard]] Result Query(TextureHandle texture) const;
    void Join();
    [[nodiscard]] Result Poll();
    Result UploadViewLUTs(FTexture const& sdr, FTexture const& hdr);
    Result UploadEnvMap(FTexture const& source);
    GPUSceneTables BeginScene(uint32_t instanceCount, uint32_t materialCount, uint32_t lightCount);
    UpdateResult EndScene(GPUSceneTables& tables);
    void DbgGetMemoryStatistics(Vector<MemoryStat>& outStats) const;
    [[nodiscard]] String DbgGetBufferStatistics() const;
    [[nodiscard]] TLASBuildResult BuildTLAS(RHICommandList* cmd, bool update);
    void Collect();
    void Reset();

    /* --- Dynamic (CPU-updateable) geometry --- */
    // Slot-correct absolute byte base of geo @p g's region in ring slot @p slot. Each geo owns a
    // contiguous (frames * stride) block from the ring allocator, so its slots are stride apart.
    [[nodiscard]] uint32_t DynamicRegionBase(GeometryResidency const& g, uint32_t slot) const
    {
        return g.resourceOffset + slot * g.dynamicStride;
    }
    // Writes geo @p g's GSMesh header (slot-correct absolute sub-offsets) into ring slot @p slot.
    void WriteDynamicHeader(GeometryResidency& g, uint32_t slot);
    // Host-only: creates @p g's AllowUpdate BLAS + its backing/scratch buffers and acquires its
    // BLAS slot. No GPU work - the actual build is recorded by the first RefitDynamicGeometry
    // (so it folds into the render graph instead of stalling on a synchronous submit).
    void AllocateDynamicBLAS(GeometryResidency& g);
    [[nodiscard]] bool HasDynamicGeometry() const;
    void BeginDynamicGeometryUpdate();
    Span<std::byte> UpdateDynamicGeometry(GeometryHandle handle);
    void EndDynamicGeometryUpdate();
    void BuildBLAS(RHICommandList* cmd);
};

size_t GPUScene::CalculateMeshPrimitiveSize(FSerializedMesh const& src)
{
    uint64_t lod0Size = src.lods.empty() ? 0 : src.lods[0].indices.decodedSize;
    return sizeof(GSMesh) +
        src.vertices.decodedSize +
        lod0Size +
        src.dagGroups.decodedSize +
        src.dagMeshlets.decodedSize +
        src.dagMeshletVtx.decodedSize +
        src.dagMeshletTri.decodedSize;
}

size_t GPUScene::CalculateCurvePrimitiveSize(FSerializedCurve const& src)
{
    return sizeof(GSCurveSet) + src.points.decodedSize + src.segments.decodedSize;
}

size_t GPUScene::CalculateCurveAABBSize(FSerializedCurve const& src)
{
    return src.aabbs.decodedSize;
}

// Threaded decode of one blob payload into its mapped staging/direct destination.
struct GPUSceneBlobDecodeJob final : Foundation::Core::Job
{
    GPUSceneBlobWrite write{};
    FBlobDeserializer blobs{Span<const unsigned char>{}};
    Span<Arena> scratchArenas{};
    Span<AllocatorStack> scratchAllocators{};
    Atomic<size_t>* counter{nullptr};

    GPUSceneBlobDecodeJob(GPUSceneBlobWrite const& write, FBlobDeserializer const& blobs,
                          Span<Arena> scratchArenas, Span<AllocatorStack> scratchAllocators,
                          Atomic<size_t>* counter) :
        write(write), blobs(blobs), scratchArenas(scratchArenas), scratchAllocators(scratchAllocators),
        counter(counter) {}

    void Execute(size_t workerID) noexcept override
    {
        if (write.blob.codec != FBlobCodec::None)
        {
            AllocatorStack& scratch = scratchAllocators[workerID];
            scratch.Reset(scratchArenas[workerID]);
            blobs.ReadBytes(write.blob, write.dst, write.size, &scratch);
        }
        else
        {
            blobs.ReadBytes(write.blob, write.dst, write.size, nullptr);
        }
        GPUSceneCompleteJob(counter);
    }
};

void GPUSceneImpl::FlushDirectGeometryUpload()
{
    if (!mDirectGeometryUpload)
        return;
    if (mPrimitiveAlloc->GetPeakUsage())
        owner.mPrimitiveBuffer->Flush(0, mPrimitiveAlloc->GetPeakUsage());
    if (mCurveAABBAlloc->GetPeakUsage())
        mCurveAABBBuffer->Flush(0, mCurveAABBAlloc->GetPeakUsage());
}

GPUScene::GPUScene(RHIDevice* device, Allocator* allocator, GPUSceneDesc const& desc,
                   AllocatorStack* frameScratch) :
    mCommittedInstances(allocator),
    mCommittedLights(allocator),
    mCommittedMaterials(allocator),
    mPickMap(allocator)
{
    mImpl = ConstructUnique<GPUSceneImpl>(allocator, *this, device, allocator, desc, frameScratch);
}

GPUSceneImpl::GPUSceneImpl(GPUScene& owner, RHIDevice* device, Allocator* allocator, GPUSceneDesc const& desc,
                           AllocatorStack* frameScratch) :
    owner(owner),
    mDevice(device), mAllocator(allocator), mFrameScratch(frameScratch),
    mInstanceBuffer(device, desc.instanceBudget),
    mMaterialBuffer(device, desc.materialBudget),
    mLightBuffer(device, desc.lightBudget),
    mLightAliasTableBuffer(device, desc.lightBudget),
    mTexture2DPool(device, allocator, {.maxBindings = desc.texturesBudget}),
    mTexture3DPool(device, allocator,
                   {.maxBindings = kGPUScenePersistentTexture3DBindings + kGPUSceneTextureBindingSlack}),
    mTexture2DSlots(allocator),
    mTexture3DSlots(allocator),
    mBLASes(allocator),
    mBLASBuffers(allocator),
    mFreeBLASSlots(allocator),
    mCurveBLASes(allocator), mCurveBLASBuffers(allocator),
    mFreeCurveBLASSlots(allocator),
    mGeometry(allocator),
    mFreeGeometrySlots(allocator),
    mDynamicGeometrySlots(allocator),
    mGeometryQueue(std::bit_ceil(static_cast<size_t>(desc.geometryBudget) + 1), allocator),
    mTextureQueue(std::bit_ceil(static_cast<size_t>(desc.texturesBudget) + 1), allocator),
    mBufferQueue(std::bit_ceil(static_cast<size_t>(kGPUSceneBufferQueueCapacity)), allocator),
    mInstanceScratch(allocator),
    mTLASInstanceStride(mDevice->WriteAccelerationStructureInstanceData({}, nullptr)),
    mTLASInstances(device, desc.tlasInstanceBudget * mTLASInstanceStride)
{
    CHECK(mDevice != nullptr);
    CHECK(mAllocator != nullptr);
    // Reserve the containers the render thread reads concurrently with the upload worker so
    // they never reallocate during the scene's lifetime (stable storage for lock-free element
    // reads). Capacities are sized to the construction budget; uploads beyond it are rejected.
    mGeometry.reserve(desc.geometryBudget);
    mBLASes.reserve(desc.geometryBudget);
    mBLASBuffers.reserve(desc.geometryBudget);
    mCurveBLASes.reserve(desc.geometryBudget);
    mCurveBLASBuffers.reserve(desc.geometryBudget);
    mTexture2DSlots.reserve(desc.texturesBudget);
    mTexture3DSlots.reserve(kGPUScenePersistentTexture3DBindings + kGPUSceneTextureBindingSlack);
    mPrimitiveAlloc = mDevice->CreateVirtualAllocator(desc.primitiveBudget);
    mCurveAABBAlloc = mDevice->CreateVirtualAllocator(desc.curveAABBBudget);
    auto caps = mDevice->GetCapabilities();
    size_t directGeometryBudget = static_cast<size_t>(desc.primitiveBudget) + desc.curveAABBBudget;
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
             RHIBufferUsageBits::DeviceAddress | RHIBufferUsageBits::AccelerationStructureBuildReadOnly,
             .size = desc.primitiveBudget});
    geoDesc.shared = false;
    mCurveAABBBuffer = mDevice->CreateBuffer(
    {.resource = geoDesc,
     .usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::StorageBuffer |
     RHIBufferUsageBits::DeviceAddress | RHIBufferUsageBits::AccelerationStructureBuildReadOnly,
     .size = desc.curveAABBBudget});
    if (mDirectGeometryUpload)
    {
        mPrimitiveMapped = owner.mPrimitiveBuffer->Map<char>();
        mCurveAABBMapped = mCurveAABBBuffer->Map<char>();
        LOG(GPUScene, LogInfo, "Direct GPU Memory Access available ({} MiB budget used). Uploading via direct copy.",
            directGeometryBudget / (1u << 20));
    }
    // Dynamic (CPU-updateable) geometry ring: a single stable, host-coherent, persistently
    // mapped, AS-build-readable buffer of (dynamicGeometryBudget * frames) bytes. Allocated once
    // here so the device object never moves. The whole buffer is suballocated by the virtual
    // allocator: each dynamic geo gets one (frames * stride) block whose per-frame slots are
    // contiguous (stride == that geo's footprint; see UploadDynamic). Off (null) when budget == 0.
    if (desc.dynamicGeometryBudget != 0)
    {
        CHECK_MSG(desc.framesInFlight >= 1, "framesInFlight must be >= 1");
        // N+1 slots: up to framesInFlight slots are read by in-flight GPU frames while the CPU
        // writes one more for the frame being prepared (it writes ahead of that frame's fence).
        mDynamicFrameCount = desc.framesInFlight + 1u;
        const size_t totalBytes = static_cast<size_t>(desc.dynamicGeometryBudget) * mDynamicFrameCount;
        mDynamicPrimitiveBuffer = mDevice->CreateBuffer(
            {.resource = {.heap = RHIDeviceHeapType::Upload,
                          .hostAccess = RHIResourceHostAccess::WriteOnly,
                          .coherent = true},
             .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress |
                      RHIBufferUsageBits::AccelerationStructureBuildReadOnly | RHIBufferUsageBits::IndexBuffer,
             .size = totalBytes});
        mDynamicPrimitiveBuffer->DebugSetObjectName("Dynamic Primitive Ring");
        mDynamicPrimitiveMapped = mDynamicPrimitiveBuffer->Map<char>();
        mDynamicPrimitiveAlloc = mDevice->CreateVirtualAllocator(totalBytes);
        LOG(GPUScene, LogInfo, "Dynamic geometry ring: {} MiB ({} MiB/frame x {} slots, {} frames in flight).",
            totalBytes / (1u << 20), desc.dynamicGeometryBudget / (1u << 20), mDynamicFrameCount, desc.framesInFlight);
    }
    mTLASBuffer = mDevice->CreateBuffer(
    {
        .resource = {
            .heap = RHIDeviceHeapType::Local,
            .shared = false
        },
        .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress |
        RHIBufferUsageBits::AccelerationStructureStorage,
        .size = desc.tlasBudget
    });
    mScratchBufferTLAS = mDevice->CreateBuffer(
    {
        .resource = {
            .heap = RHIDeviceHeapType::Local,
            .shared = false
        },
        .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress,
        .size = desc.tlasScratchBudget,
        .alignment = 256 // Aligned to Vulkan spec. Should be large enough for other APIs as well?
    });
    CHECK_MSG(mTLASBuffer->mDesc.size <= UINT32_MAX, "TLAS budget {} exceeds uint32_t range", mTLASBuffer->mDesc.size);
    RHIAccelerationStructureDesc tlasDesc{
        .type = RHIAccelerationStructureType::TopLevel,
        .buffer = mTLASBuffer.Get(),
        .size = static_cast<uint32_t>(mTLASBuffer->mDesc.size)
    };
    owner.mTLAS = mDevice->CreateAccelerationStructure(tlasDesc);
    
    owner.mSobolMatricesBuffer = mDevice->CreateBuffer(
    {
        .resource = {
            .heap = RHIDeviceHeapType::Local,
            .shared = false
        },
        .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination,
        .size = sizeof(kSobolMatrices32)
    });
    owner.mSobolMatricesBuffer->DebugSetObjectName("Sobol Matrices");

    // Initialize procedural light BLASes. Intersections are done in shader against the
    // unit disk/rect in object space; TLAS instances provide the light transform.
    {
        struct LightAABBs {
            RHIAccelerationStructureAABB rect;
            RHIAccelerationStructureAABB disk;
        } geo;
        constexpr float kLightAABBThickness = 1e-3f;
        geo.rect = RHIAccelerationStructureAABB{-1.0f, -1.0f, -kLightAABBThickness,
                                                 1.0f,  1.0f,  kLightAABBThickness};
        geo.disk = geo.rect;

        mLightGeometryBuffer = mDevice->CreateBuffer({
            .resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
            .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::DeviceAddress | RHIBufferUsageBits::AccelerationStructureBuildReadOnly,
            .size = sizeof(LightAABBs)
        });

        ImmediateUpload upload(mDevice, sizeof(LightAABBs));
        upload.Begin();
        char* ptr = upload.Upload(mLightGeometryBuffer.Get(), sizeof(LightAABBs), 0);
        std::memcpy(ptr, &geo, sizeof(LightAABBs));
        upload.End();
        upload.WaitIdle();

        // Build BLAS
        RHIAccelerationStructureGeometryInfo rectGeoInfo{
            .type = RHIAccelerationGeometryType::AABBs,
            .aabbData = {
                .aabbBuffer = mLightGeometryBuffer.Get(),
                .offset = offsetof(LightAABBs, rect),
                .count = 1,
                .stride = sizeof(RHIAccelerationStructureAABB)
            }
        };
        RHIAccelerationStructureBuildRangeInfo rectRange{.primitiveCount = 1};

        RHIAccelerationStructureGeometryInfo diskGeoInfo{
            .type = RHIAccelerationGeometryType::AABBs,
            .aabbData = {
                .aabbBuffer = mLightGeometryBuffer.Get(),
                .offset = offsetof(LightAABBs, disk),
                .count = 1,
                .stride = sizeof(RHIAccelerationStructureAABB)
            }
        };
        RHIAccelerationStructureBuildRangeInfo diskRange{.primitiveCount = 1};

        RHIAccelerationStructureBuildDesc rectDesc{
            .type = RHIAccelerationStructureType::BottomLevel,
            .flags = RHIAccelerationStructureBuildFlagsBits::PreferFastTrace,
            .operation = RHIAccelerationStructureBuildOp::Build,
            .geometries = Span<const RHIAccelerationStructureGeometryInfo>{&rectGeoInfo, 1},
            .ranges = Span<const RHIAccelerationStructureBuildRangeInfo>{&rectRange, 1}
        };
        RHIAccelerationStructureBuildDesc diskDesc{
            .type = RHIAccelerationStructureType::BottomLevel,
            .flags = RHIAccelerationStructureBuildFlagsBits::PreferFastTrace,
            .operation = RHIAccelerationStructureBuildOp::Build,
            .geometries = Span<const RHIAccelerationStructureGeometryInfo>{&diskGeoInfo, 1},
            .ranges = Span<const RHIAccelerationStructureBuildRangeInfo>{&diskRange, 1}
        };

        StackArena<4096> sizeInfoArena;
        AllocatorStack sizeInfoScratch(sizeInfoArena);
        auto rectSize = mDevice->GetAccelerationStructureSizeInfo(rectDesc, sizeInfoScratch.Ptr());
        sizeInfoScratch.Reset(sizeInfoArena);
        auto diskSize = mDevice->GetAccelerationStructureSizeInfo(diskDesc, sizeInfoScratch.Ptr());

        uint32_t rectOffset = 0;
        uint32_t diskOffset = AlignUp(rectSize.accelerationStructureSize, 256u);
        uint32_t totalSize = diskOffset + diskSize.accelerationStructureSize;

        mLightBLASBuffer = mDevice->CreateBuffer({
            .resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
            .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress | RHIBufferUsageBits::AccelerationStructureStorage,
            .size = totalSize
        });

        uint32_t scratchSize = std::max(rectSize.buildScratchSize, diskSize.buildScratchSize);
        auto scratch = mDevice->CreateBuffer({
            .resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
            .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress,
            .size = scratchSize,
            .alignment = 256
        });

        mRectBLAS = mDevice->CreateAccelerationStructure({
            .type = RHIAccelerationStructureType::BottomLevel,
            .buffer = mLightBLASBuffer.Get(),
            .offset = rectOffset,
            .size = rectSize.accelerationStructureSize
        });
        mDiskBLAS = mDevice->CreateAccelerationStructure({
            .type = RHIAccelerationStructureType::BottomLevel,
            .buffer = mLightBLASBuffer.Get(),
            .offset = diskOffset,
            .size = diskSize.accelerationStructureSize
        });

        ImmediateContext ctx(mDevice);
        auto* cmd = ctx.Get();
        cmd->Begin();
        
        rectDesc.scratchBuffer = scratch.Get();
        rectDesc.scratchBufferOffset = 0;
        rectDesc.dstAS = mRectBLAS.Get();
        cmd->BuildAccelerationStructure({{{rectDesc}}});

        cmd->BeginTransition();
        cmd->SetBufferTransition(scratch.Get(), {
            .srcStage = RHIPipelineStageBits::AccelerationBuild,
            .dstStage = RHIPipelineStageBits::AccelerationBuild
        });
        cmd->EndTransition();

        diskDesc.scratchBuffer = scratch.Get();
        diskDesc.scratchBufferOffset = 0;
        diskDesc.dstAS = mDiskBLAS.Get();
        cmd->BuildAccelerationStructure({{{diskDesc}}});

        cmd->End();
        ctx.Submit();
        ctx.WaitIdle();
    }
    // Upload precomputed LUTs
    {
        auto lutE = MakeLUT(kGGXlutE, RHIResourceFormat::R32G32SignedFloat, 32, 32);
        auto lutEavg = MakeLUT(kGGXlutEavg, RHIResourceFormat::R32SignedFloat, 32, 1, 1, RHITextureDimension::E2D);
        auto lutEIOR = MakeLUT(kGGXlutEIOR, RHIResourceFormat::R32SignedFloat, 16, 16, 16, RHITextureDimension::E3D);
        auto lutEIORavg = MakeLUT(kGGXlutEIORavg, RHIResourceFormat::R32SignedFloat, 32, 32);
        auto lutEIORInv = MakeLUT(kGGXlutEInvIOR, RHIResourceFormat::R32SignedFloat, 16, 16, 16, RHITextureDimension::E3D);
        auto lutEIORInvavg = MakeLUT(kGGXlutEInvIORavg, RHIResourceFormat::R32SignedFloat, 32, 32);
        auto sheenLtc = MakeLUT(kSheenLTCLut, RHIResourceFormat::R32G32B32A32SignedFloat, 32, 32);
        FTexture foundationDefaultTexture2D(mAllocator);
        foundationDefaultTexture2D.Initialize(RHIResourceFormat::R32G32B32A32SignedFloat, RHITextureDimension::E2D, 1, 1);
        foundationDefaultTexture2D.bytes.assign(foundationDefaultTexture2D.GetSize(), 0u);
        FTexture foundationDefaultTexture2DFloat(mAllocator);
        foundationDefaultTexture2DFloat.Initialize(RHIResourceFormat::R32SignedFloat, RHITextureDimension::E2D, 1, 1);
        foundationDefaultTexture2DFloat.bytes.resize(sizeof(float));
        *reinterpret_cast<float*>(foundationDefaultTexture2DFloat.bytes.data()) = 1.0f;
        auto defaultViewLutSdr =
            LoadLUT(mAllocator, kViewLUTsSdr[kDefaultViewLUTSdr].path);
        auto defaultViewLutHdr =
            LoadLUT(mAllocator, kViewLUTsHdr[kDefaultViewLUTHdr].path);
        const size_t foundationDefaultBufferFloatSize = sizeof(float);
        owner.mFoundationDefaultBufferFloat = mDevice->CreateBuffer({
            .resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
            .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination,
            .size = foundationDefaultBufferFloatSize
        });
        // Textures (incl. view LUTs) go through the unified upload queue.
        // Pinned: GPUScene-owned singletons that Collect must never reclaim.
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
        UploadViewLUTs(defaultViewLutSdr, defaultViewLutHdr);

        // Plain device-local buffers ride the same upload queue.
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
    // Stop the persistent upload worker (it loops until mStop) before tearing anything down.
    {
        std::lock_guard<Mutex> lock(mWorkMutex);
        mStop = true;
    }
    mWorkCV.notify_all();
    if (mUploadThread.joinable())
        mUploadThread.join();
    // Release every outstanding suballocation before the VMA virtual blocks are destroyed;
    // a non-empty block trips a VMA assert on teardown (geometry need not be GC'd first).
    if (mPrimitiveAlloc)
        mPrimitiveAlloc->Clear();
    if (mCurveAABBAlloc)
        mCurveAABBAlloc->Clear();
    // Dynamic-geo BLAS backing buffers / scratch live in GeometryResidency; clearing the
    // residency vector (or its element handles) releases them. Clear the ring allocator so the
    // VMA virtual block is empty before it is destroyed.
    for (auto& g : mGeometry)
    {
        g.dynBLASBuffer.Reset();
        g.dynScratchBuffer.Reset();
    }
    if (mDynamicPrimitiveAlloc)
        mDynamicPrimitiveAlloc->Clear();
}

Pair<GSInstance*, uint32_t> GPUSceneImpl::AllocateInstance(uint32_t count)
{
    return mInstanceBuffer.Allocate(count);
}

Pair<GSMaterial*, uint32_t> GPUSceneImpl::AllocateMaterial(uint32_t count)
{
    return mMaterialBuffer.Allocate(count);
}

Pair<GSLight*, uint32_t> GPUSceneImpl::AllocateLight(uint32_t count)
{
    return mLightBuffer.Allocate(count);
}

Pair<GSAlias*, uint32_t> GPUSceneImpl::AllocateLightAliasTable(uint32_t count)
{
    return mLightAliasTableBuffer.Allocate(count);
}

GPUScene::GPUSceneTables GPUSceneImpl::BeginScene(uint32_t instanceCount, uint32_t materialCount, uint32_t lightCount)
{
    CHECK_MSG(!mOpenTables.open, "BeginScene called while a scene table is already open");
    GPUSceneTables tables{};
    if (instanceCount != 0)
    {
        auto [ptr, off] = AllocateInstance(instanceCount);
        mInstanceScratch.assign(instanceCount, InstanceDesc{});
        tables.instances = Span<InstanceDesc>(mInstanceScratch.data(), instanceCount);
        tables.firstInstance = off;
        mOpenTables.instancePtr = ptr;
        mOpenTables.instanceCount = instanceCount;
    }
    if (materialCount != 0)
    {
        auto [ptr, off] = AllocateMaterial(materialCount);
        tables.materials = Span<GSMaterial>(ptr, materialCount);
        tables.firstMaterial = off;
    }
    if (lightCount != 0)
    {
        auto [ptr, off] = AllocateLight(lightCount);
        auto [aliasPtr, aliasOff] = AllocateLightAliasTable(lightCount);
        tables.lights = Span<GSLight>(ptr, lightCount);
        tables.firstLight = off;
        tables.firstLightAliasTable = aliasOff;
        mOpenTables.firstAliasTable = aliasOff;
        mOpenTables.aliasPtr = aliasPtr;
    }
    mOpenTables.open = true;
    return tables;
}

GPUScene::UpdateResult GPUSceneImpl::EndScene(GPUSceneTables& tables)
{
    CHECK_MSG(mOpenTables.open, "EndScene called without a matching BeginScene");
    UpdateResult res{};
    res.firstInstance = tables.firstInstance;
    res.numInstances = static_cast<uint32_t>(tables.instances.size());
    res.firstMaterial = tables.firstMaterial;
    res.numMaterials = static_cast<uint32_t>(tables.materials.size());

    // Translate each caller-facing InstanceDesc into a GPU GSInstance, resolving the
    // primitive-buffer offset and geometry type from the bound GeometryHandle. resourceOffset/
    // type are known at Upload() time, so this is valid even while geometry still streams in.
    // The committed snapshot is the authoritative CPU-side scene used by BuildTLAS, picking,
    // and Collect(); it must not alias ring memory overwritten by a later commit.
    CHECK(tables.instances.size() == mOpenTables.instanceCount);
    owner.mCommittedInstances.resize(tables.instances.size());
    for (size_t i = 0; i < tables.instances.size(); ++i)
    {
        InstanceDesc const& desc = tables.instances[i];
        GeometryResidency const* g = ResolveGeometry(desc.geometry);
        CHECK_MSG(g, "EndScene instance references invalid geometry (index {}, generation {})",
                  desc.geometry.index, desc.geometry.generation);
        // Dynamic geo's header lives in the ring; resolve to the *current frame slot's* absolute
        // base so the BLAS refit and (Phase 4) shading read this frame's deformed data.
        uint32_t const resourceOffset = g->dynamic ? DynamicRegionBase(*g, mDynamicFrameSlot) : g->resourceOffset;
        GSInstance inst{
            .transform = desc.transform,
            .rotation = desc.rotation,
            .scale = desc.scale,
            .resourceOffset = resourceOffset,
            .materialIndex = desc.materialIndex,
            .resourceIndex = desc.geometry.index,
            .type = g->type | (g->dynamic ? kGSInstanceFlagDynamic : 0u),
        };
        mOpenTables.instancePtr[i] = inst; // ring (GPU)
        owner.mCommittedInstances[i] = inst;     // committed snapshot
    }

    owner.mCommittedMaterials.assign(tables.materials.begin(), tables.materials.end());
    owner.mCommittedLights.assign(tables.lights.begin(), tables.lights.end());

    if (!tables.lights.empty())
    {
        Allocator* scratch = mFrameScratch ? mFrameScratch : mAllocator;
        Vector<float> powers(tables.lights.size(), scratch);
        float weightSum = 0.0f;
        for (size_t i = 0; i < tables.lights.size(); ++i)
        {
            powers[i] = tables.lights[i].selectionWeight;
            weightSum += powers[i];
        }
        AliasTable table(powers, scratch);
        CHECK(mOpenTables.aliasPtr != nullptr);
        std::memcpy(mOpenTables.aliasPtr, table.mBins.data(), table.mBins.size() * sizeof(GSAlias));
        res.firstLight = tables.firstLight;
        res.firstLightAliasTable = mOpenTables.firstAliasTable;
        res.numLights = static_cast<uint32_t>(tables.lights.size());
        res.sceneLightWeightSum = weightSum;
    }
    mOpenTables = OpenTables{};
    return res;
}

void GPUScene::BuildUBO(UBO& globals, bool hdr) const
{
    globals.ggxLutEIndex = mLUTGGXEIndex.index;
    globals.ggxLutEavgIndex = mLUTGGXEavgIndex.index;
    globals.ggxLutEIORIndex = mLUTGGXEIORIndex.index;
    globals.ggxLutEIORavgIndex = mLUTGGXEIORavgIndex.index;
    globals.ggxLutEIORInvIndex = mLUTGGXEIORInvIndex.index;
    globals.ggxLutEIORInvavgIndex = mLUTGGXEIORInvavgIndex.index;
    globals.sheenLtcIndex = mLUTSheenLTCIndex.index;
    globals.viewLutIndex = (hdr ? mLUTViewHdrIndex : mLUTViewSdrIndex).index;
    globals.envMapTextureIndex = GetEnvMapIndexOrDefault();
    globals.envMapMarginalCDFIndex = GetEnvMapMarginalCDFIndexOrDefault();
    globals.envMapConditionalCDFIndex = GetEnvMapConditionalCDFIndexOrDefault();
}

void GPUSceneImpl::DbgGetMemoryStatistics(Vector<MemoryStat>& outStats) const
{
    auto AddBufferSize = [&](RHIDeviceScopedHandle<RHIBuffer> const& buffer) -> size_t
    {
        if (!buffer)
            return 0;
        return buffer->GetAllocationSize();
    };
    auto AddRingBufferSize = [&](auto const& buffer) -> size_t
    {
        return buffer.mBuffer->GetAllocationSize();
    };
    auto SumBuffers = [&](auto const& buffers) -> size_t
    {
        size_t bytes = 0;
        for (auto const& buffer : buffers)
            bytes += AddBufferSize(buffer);
        return bytes;
    };

    size_t primitiveBytes = AddBufferSize(owner.mPrimitiveBuffer);
    size_t curveAABBBytes = AddBufferSize(mCurveAABBBuffer);
    auto texture2DStats = mTexture2DPool.GetStats();
    auto texture3DStats = mTexture3DPool.GetStats();
    size_t instanceBytes = AddRingBufferSize(mInstanceBuffer);
    size_t materialBytes = AddRingBufferSize(mMaterialBuffer);
    size_t lightBytes = AddRingBufferSize(mLightBuffer);
    size_t lightAliasBytes = AddRingBufferSize(mLightAliasTableBuffer);
    size_t tlasInstanceBytes = AddRingBufferSize(mTLASInstances);
    size_t blasBytes = SumBuffers(mBLASBuffers);
    size_t curveBLASBytes = SumBuffers(mCurveBLASBuffers);
    size_t tlasBytes = AddBufferSize(mTLASBuffer);
    size_t tlasScratchBytes = AddBufferSize(mScratchBufferTLAS);
    size_t lightBLASBytes = AddBufferSize(mLightBLASBuffer);
    size_t lightGeometryBytes = AddBufferSize(mLightGeometryBuffer);
    size_t sobolBytes = AddBufferSize(owner.mSobolMatricesBuffer);
    size_t defaultBufferBytes = AddBufferSize(owner.mFoundationDefaultBufferFloat);

    outStats.push_back({"Primitive Buffer (Buffer)", primitiveBytes});
    outStats.push_back({"Curve AABB Buffer (Buffer)", curveAABBBytes});
    outStats.push_back({"Texture2D Pool (Texture)", texture2DStats.ownedTextureBytes});
    outStats.push_back({"Texture3D Pool (Texture)", texture3DStats.ownedTextureBytes});
    outStats.push_back({"Instance Buffer (Buffer)", instanceBytes});
    outStats.push_back({"TLAS Instance Buffer (Buffer)", tlasInstanceBytes});
    outStats.push_back({"Dynamic Upload Buffers (Buffer)",
                        materialBytes + lightBytes + lightAliasBytes});
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
    fmt::format_to(std::back_inserter(res), "Curve AABB Buffer: {:.1f} MB allocated, used {:.1f} / {:.1f} MB\n",
                   mCurveAABBBuffer->GetAllocationSize() / static_cast<float>(1 << 20u),
                   mCurveAABBAlloc->GetUsedBytes() / static_cast<float>(1 << 20u),
                   mCurveAABBBuffer->mDesc.size / static_cast<float>(1 << 20u));
    fmt::format_to(std::back_inserter(res), "Texture2D Pool: {:.1f} MB owned, {:.1f} MB referenced, used {} / {} bindings, owned {} textures\n",
                   texture2DStats.ownedTextureBytes / static_cast<float>(1 << 20u),
                   texture2DStats.referencedTextureBytes / static_cast<float>(1 << 20u),
                   texture2DStats.activeBindings,
                   texture2DStats.capacity,
                   texture2DStats.ownedTextureBindings);
    fmt::format_to(std::back_inserter(res), "Texture3D Pool: {:.1f} MB owned, {:.1f} MB referenced, used {} / {} bindings, owned {} textures\n",
                   texture3DStats.ownedTextureBytes / static_cast<float>(1 << 20u),
                   texture3DStats.referencedTextureBytes / static_cast<float>(1 << 20u),
                   texture3DStats.activeBindings,
                   texture3DStats.capacity,
                   texture3DStats.ownedTextureBindings);
    fmt::format_to(std::back_inserter(res), "Instance Buffer: {:.1f} MB allocated, used {} / {} instances\n",
                   mInstanceBuffer.mBuffer->GetAllocationSize() / static_cast<float>(1 << 20u),
                   mInstanceBuffer.Used(), mInstanceBuffer.Capacity());
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
        LOG(GPUScene, LogError, "Primitive buffer overflow for serialized mesh. Need {} bytes, {} used of {}",
            size, mPrimitiveAlloc->GetUsedBytes(), mPrimitiveAlloc->GetCapacity());
        return Result::OutOfMemory;
    }
    outOffset = static_cast<uint32_t>(base);

    // Compute the shader header / absolute sub-offsets. The byte layout here MUST match
    // StageMesh, which derives local offsets from the header.
    outData = GSMesh{};
    uint32_t cursor = 0;
    auto Skip = [&](size_t bytes) { uint32_t off = cursor; cursor += static_cast<uint32_t>(bytes); return off; };
    Skip(sizeof(GSMesh));
    outData.vtxCount = src.vertexCount;
    outData.vtxOffset = outOffset + Skip(static_cast<size_t>(src.vertices.decodedSize));
    outData.idxCount = lod0.indexCount;
    outData.idxOffset = outOffset + Skip(static_cast<size_t>(lod0.indices.decodedSize));
    outData.groupCount = src.dagGroups.count;
    outData.groupOffset = outOffset + Skip(static_cast<size_t>(src.dagGroups.decodedSize));
    outData.meshletCount = src.dagMeshlets.count;
    outData.meshletOffset = outOffset + Skip(static_cast<size_t>(src.dagMeshlets.decodedSize));
    outData.meshletVtxOffset = outOffset + Skip(static_cast<size_t>(src.dagMeshletVtx.decodedSize));
    outData.meshletTriOffset = outOffset + Skip(static_cast<size_t>(src.dagMeshletTri.decodedSize));
    outData.meshletGlobalIndex = mMeshletGlobalCounter;
    mMeshletGlobalCounter += outData.meshletCount;
    CHECK_MSG(cursor == size, "Mesh layout mismatch: expected {} got {}", size, cursor);
    return Result::InProgress;
}

size_t GPUSceneImpl::StageMesh(ImmediateUpload* ctx, FSerializedMesh const& src, GSMesh const& header,
                           uint32_t offset, Vector<GPUSceneBlobWrite>& outWrites)
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
        if (ctx->ptr + size > ctx->end)
            return 0;
        ptr = ctx->Upload(owner.mPrimitiveBuffer.Get(), size, offset);
        CHECK(ptr != nullptr);
    }
    // The header is a trivial copy, written inline; only payloads need threaded decode.
    std::memcpy(ptr, &header, sizeof(GSMesh));
    auto AppendBlobWrite = [&](FBlobRef const& blob, uint32_t absOffset)
    {
        outWrites.push_back({blob, ptr + (absOffset - offset), static_cast<size_t>(blob.decodedSize)});
    };
    auto const& lod0 = src.lods[0];
    AppendBlobWrite(src.vertices, header.vtxOffset);
    AppendBlobWrite(lod0.indices, header.idxOffset);
    AppendBlobWrite(src.dagGroups, header.groupOffset);
    AppendBlobWrite(src.dagMeshlets, header.meshletOffset);
    AppendBlobWrite(src.dagMeshletVtx, header.meshletVtxOffset);
    AppendBlobWrite(src.dagMeshletTri, header.meshletTriOffset);
    return size;
}

GPUScene::Result GPUSceneImpl::ReserveCurve(FSerializedCurve const& src, GSCurveSet& outData, uint32_t& outOffset)
{
    static_assert(sizeof(FCurvePoint) == sizeof(GSCurvePoint));
    static_assert(alignof(FCurvePoint) == alignof(GSCurvePoint));
    static_assert(sizeof(FSerializedCurveSegment) == sizeof(GSCurveSegment));
    static_assert(alignof(FSerializedCurveSegment) == alignof(GSCurveSegment));
    static_assert(sizeof(FSerializedCurveAABB) == sizeof(RHIAccelerationStructureAABB));
    static_assert(alignof(FSerializedCurveAABB) == alignof(RHIAccelerationStructureAABB));

    if (src.points.decodedSize == 0 || src.segments.count == 0)
        return Result::InvalidInput;
    CHECK_MSG(src.points.stride == sizeof(GSCurvePoint), "Serialized curve point stride mismatch");
    CHECK_MSG(src.segments.stride == sizeof(GSCurveSegment), "Serialized curve segment stride mismatch");
    CHECK_MSG(src.aabbs.stride == sizeof(RHIAccelerationStructureAABB), "Serialized curve AABB stride mismatch");
    CHECK_MSG(src.points.decodedSize % sizeof(GSCurvePoint) == 0, "Serialized curve point blob size mismatch");
    CHECK_MSG(src.segments.decodedSize == sizeof(GSCurveSegment) * src.segments.count,
              "Serialized curve segment blob size mismatch");
    CHECK_MSG(src.aabbs.count == src.segments.count, "Serialized curve AABB count mismatch");
    CHECK_MSG(src.aabbs.decodedSize == sizeof(RHIAccelerationStructureAABB) * src.aabbs.count,
              "Serialized curve AABB blob size mismatch");

    const size_t pointCount = static_cast<size_t>(src.points.decodedSize / sizeof(GSCurvePoint));
    const size_t size = GPUScene::CalculateCurvePrimitiveSize(src);
    constexpr size_t kAlign = 4;
    uint64_t base = mPrimitiveAlloc->Allocate(size, kAlign);
    if (base == RHIVirtualAllocator::kInvalidOffset)
    {
        LOG(GPUScene, LogError, "Primitive buffer overflow for serialized curve. Need {} bytes, {} used of {}",
            size, mPrimitiveAlloc->GetUsedBytes(), mPrimitiveAlloc->GetCapacity());
        return Result::OutOfMemory;
    }
    outOffset = static_cast<uint32_t>(base);

    const size_t aabbSize = GPUScene::CalculateCurveAABBSize(src);
    uint64_t aabbBase = mCurveAABBAlloc->Allocate(aabbSize, alignof(RHIAccelerationStructureAABB));
    if (aabbBase == RHIVirtualAllocator::kInvalidOffset)
    {
        mPrimitiveAlloc->Free(outOffset);
        LOG(GPUScene, LogError, "Curve AABB buffer overflow. Need {} bytes, {} used of {}",
            aabbSize, mCurveAABBAlloc->GetUsedBytes(), mCurveAABBAlloc->GetCapacity());
        return Result::OutOfMemory;
    }

    outData = GSCurveSet{};
    uint32_t cursor = 0;
    auto Skip = [&](size_t bytes) { uint32_t off = cursor; cursor += static_cast<uint32_t>(bytes); return off; };
    Skip(sizeof(GSCurveSet));
    outData.pointCount = static_cast<uint32_t>(pointCount);
    outData.pointOffset = outOffset + Skip(static_cast<size_t>(src.points.decodedSize));
    outData.segmentCount = static_cast<uint32_t>(src.segments.count);
    outData.segmentOffset = outOffset + Skip(static_cast<size_t>(src.segments.decodedSize));
    outData.materialIndex = src.materialIndex;
    outData.aabbOffset = static_cast<uint32_t>(aabbBase);
    CHECK_MSG(cursor == size, "Curve layout mismatch: expected {} got {}", size, cursor);
    return Result::InProgress;
}

size_t GPUSceneImpl::StageCurve(ImmediateUpload* ctx, FSerializedCurve const& src, GSCurveSet const& header,
                            uint32_t offset, Vector<GPUSceneBlobWrite>& outWrites)
{
    const size_t size = GPUScene::CalculateCurvePrimitiveSize(src);
    const size_t aabbSize = GPUScene::CalculateCurveAABBSize(src);
    char* ptr = nullptr;
    char* aabbPtr = nullptr;
    if (mDirectGeometryUpload)
    {
        CHECK(mPrimitiveMapped != nullptr);
        CHECK(mCurveAABBMapped != nullptr);
        ptr = mPrimitiveMapped + offset;
        aabbPtr = mCurveAABBMapped + header.aabbOffset;
    }
    else
    {
        if (ctx->ptr + size + aabbSize > ctx->end)
            return 0;
        ptr = ctx->Upload(owner.mPrimitiveBuffer.Get(), size, offset);
        CHECK(ptr != nullptr);
        aabbPtr = ctx->Upload(mCurveAABBBuffer.Get(), aabbSize, header.aabbOffset);
        CHECK(aabbPtr != nullptr);
    }
    std::memcpy(ptr, &header, sizeof(GSCurveSet));
    auto AppendBlobWrite = [&](FBlobRef const& blob, char* dstPtr)
    {
        outWrites.push_back({blob, dstPtr, static_cast<size_t>(blob.decodedSize)});
    };
    AppendBlobWrite(src.points, ptr + (header.pointOffset - offset));
    AppendBlobWrite(src.segments, ptr + (header.segmentOffset - offset));
    AppendBlobWrite(src.aabbs, aabbPtr);
    return size + aabbSize;
}

static bool IsTexture3DView(RHITextureDimension dimension)
{
    return dimension == RHITextureDimension::E3D;
}

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

// Views an in-memory FTexture as an FSerializedTexture whose subresource blobs point
// directly into `source.bytes` (codec=None), so the same async upload queue can carry
// CPU-resident textures (built-in LUTs, env map, view LUTs) as well as scene textures.
static FSerializedTexture MakeInMemoryTextureAdaptor(FTexture const& source, Allocator* alloc)
{
    FSerializedTexture adaptor(alloc);
    static_cast<FTextureHeader&>(adaptor) = static_cast<FTextureHeader const&>(source);
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
    auto range = RHITextureSubresourceRange::Create(
        RHITextureAspectFlagBits::Color,
        0, metadata.GetNumMips(),
        0, texture->mDesc.arrayLayers);
    cmd->BeginTransition();
    cmd->SetImageTransition(texture, {
                                .srcImgLayout = from,
                                .dstImgLayout = to,
                                .srcImgRange = range
                            });
    cmd->EndTransition();
}

size_t GPUSceneImpl::StageTextureSubresource(ImmediateUpload* ctx, FSerializedTexture const& source, RHITexture* texture,
                                         uint32_t layer, uint32_t mip, Vector<GPUSceneBlobWrite>& outWrites)
{
    CHECK(texture != nullptr);
    CHECK_MSG(source.IsValid(), "Serialized texture is invalid");

    FTextureHeader const& metadata = static_cast<FTextureHeader const&>(source);
    FBlobRef const& subresourceBlob = source.GetSubresourceBlob(layer, mip);
    size_t const subresourceSize = metadata.GetSubresourceSize(layer, mip);
    CHECK_MSG(subresourceBlob.decodedSize == subresourceSize,
              "Serialized texture subresource size mismatch: layer {}, mip {}, blob {}, expected {}",
              layer, mip, subresourceBlob.decodedSize, subresourceSize);

    uint32_t const alignment = GetTextureUploadAlignment(metadata);
    char* preflight = reinterpret_cast<char*>(AlignUp(reinterpret_cast<uintptr_t>(ctx->ptr), alignment));
    if (preflight >= ctx->end || static_cast<size_t>(ctx->end - preflight) < subresourceSize)
        return 0;

    CHECK(ctx->Align(alignment));
    RHIExtent3D const mipExtent = metadata.GetMipExtent(mip);
    char* ptr = ctx->Upload(texture, subresourceSize,
                            {
                                .aspect = RHITextureAspectFlagBits::Color,
                                .mipLevel = mip, .baseArrayLayer = layer, .layerCount = 1
                            },
                            {0, 0, 0}, mipExtent);
    CHECK(ptr != nullptr);
    outWrites.push_back({subresourceBlob, ptr, subresourceSize});
    return subresourceSize;
}

/* --- Public resource upload work queue --- */

GPUScene::Result GPUSceneImpl::Upload(FBlobDeserializer* blobs, FSerializedMesh const& source, GeometryHandle& outHandle)
{
    CHECK(blobs != nullptr);
    PendingGeometryUpload pending;
    {
        // Reserve final memory + claim the residency slot under the residency lock: this both
        // serializes concurrent producers and keeps the renderer's mGeometry reads consistent.
        std::lock_guard<Mutex> lock(mResidencyMutex);
        GSMesh header{};
        uint32_t offset = 0;
        Result r = ReserveMesh(source, header, offset);
        if (r != Result::InProgress)
            return r;
        uint32_t slot = AcquireGeometrySlot();
        if (slot == UINT32_MAX)
            return Result::OutOfMemory;
        GeometryResidency& g = mGeometry[slot];
        uint32_t generation = g.generation;
        g = GeometryResidency{};
        g.generation = generation;
        g.type = kGSInstanceTypeMesh;
        g.blasSlot = UINT32_MAX;
        g.resourceOffset = offset;
        g.mesh = header;
        g.state = ResourceState::Queued;
        g.live = true;
        outHandle = {slot, generation};
        pending = {.handle = outHandle, .blobs = *blobs, .mesh = &source, .curve = nullptr,
                   .footprint = mDirectGeometryUpload ? 1 : GPUScene::CalculateMeshPrimitiveSize(source)};
    }
    EnqueueUpload(mGeometryQueue, std::move(pending));
    return Result::InProgress;
}

GPUScene::Result GPUSceneImpl::Upload(FBlobDeserializer* blobs, FSerializedCurve const& source, GeometryHandle& outHandle)
{
    CHECK(blobs != nullptr);
    PendingGeometryUpload pending;
    {
        std::lock_guard<Mutex> lock(mResidencyMutex);
        GSCurveSet header{};
        uint32_t offset = 0;
        Result r = ReserveCurve(source, header, offset);
        if (r != Result::InProgress)
            return r;
        uint32_t slot = AcquireGeometrySlot();
        if (slot == UINT32_MAX)
            return Result::OutOfMemory;
        GeometryResidency& g = mGeometry[slot];
        uint32_t generation = g.generation;
        g = GeometryResidency{};
        g.generation = generation;
        g.type = kGSInstanceTypeCurve;
        g.blasSlot = UINT32_MAX;
        g.resourceOffset = offset;
        g.curve = header;
        g.state = ResourceState::Queued;
        g.live = true;
        outHandle = {slot, generation};
        const size_t footprint = GPUScene::CalculateCurvePrimitiveSize(source) + GPUScene::CalculateCurveAABBSize(source);
        pending = {.handle = outHandle, .blobs = *blobs, .mesh = nullptr, .curve = &source,
                   .footprint = mDirectGeometryUpload ? 1 : footprint};
    }
    EnqueueUpload(mGeometryQueue, std::move(pending));
    return Result::InProgress;
}

GPUScene::Result GPUSceneImpl::Upload(FBlobDeserializer* blobs, FSerializedTexture const& source,
                                  TextureHandle& outTextureIndex, const char* debugName, bool pinned)
{
    CHECK(blobs != nullptr);
    if (!source.IsValid())
        return Result::InvalidInput;
    // Create the destination texture + view up front and bind the bindless slot now so
    // the handle is usable as a Query key while contents/layout become Ready during
    // Join(). A valid `outTextureIndex` updates that slot in place (env map / view LUT
    // reload); otherwise a new slot is allocated.
    FTextureHeader const& metadata = static_cast<FTextureHeader const&>(source);
    bool const is3D = IsTexture3DView(metadata.GetViewDimension());
    auto texture = mDevice->CreateTexture(metadata.GetDesc());
    if (debugName)
        texture->DebugSetObjectName(debugName);
    auto view = texture->CreateTextureView({
        .format = metadata.GetFormat(),
        .dimension = metadata.GetViewDimension(),
        .range = RHITextureSubresourceRange::Create(
            RHITextureAspectFlagBits::Color,
            0, metadata.GetNumMips(),
            0, texture->mDesc.arrayLayers)
    });
    PendingTextureUpload pending;
    {
        // Claim/refresh the slot under the residency lock: serializes producers and keeps the
        // renderer's texture-slot reads (Query) consistent. Slots are reserved to budget, so
        // the resize never reallocates under those concurrent reads.
        std::lock_guard<Mutex> lock(mResidencyMutex);
        BindlessPool& pool = TexturePool(is3D);
        Vector<TextureSlot>& slots = TextureSlots(is3D);
        if (!outTextureIndex.IsValid())
        {
            uint32_t slot = pool.Allocate(std::move(texture), std::move(view));
            if (slot >= slots.size())
                slots.resize(slot + 1);
            slots[slot].live = true;
            slots[slot].pinned = pinned;
            slots[slot].resident = false;
            outTextureIndex = {slot, slots[slot].generation, is3D};
        }
        else
        {
            // In-place refresh (env map / view LUT reload) keeps the same slot + generation.
            CHECK_MSG(outTextureIndex.is3D == is3D && outTextureIndex.index < slots.size() &&
                          slots[outTextureIndex.index].live &&
                          slots[outTextureIndex.index].generation == outTextureIndex.generation,
                      "Upload in-place update on a stale texture handle (slot {}, generation {})",
                      outTextureIndex.index, outTextureIndex.generation);
            slots[outTextureIndex.index].pinned = pinned;
            slots[outTextureIndex.index].resident = false; // re-upload: not Ready until the drain completes
            pool.Update(outTextureIndex.index, std::move(texture), std::move(view));
        }
        pending = {.index = outTextureIndex.index, .blobs = *blobs, .source = &source};
    }
    EnqueueUpload(mTextureQueue, std::move(pending));
    return Result::InProgress;
}

GPUScene::Result GPUSceneImpl::Upload(FTexture const& source, TextureHandle& outTextureIndex, const char* debugName,
                                  bool pinned)
{
    if (!source.IsValid())
        return Result::InvalidInput;
    // Route CPU-resident textures through the same work queue via an in-memory adaptor.
    // `adaptor` / `blobs` reference `source` and stay alive until Join() drains.
    FSerializedTexture adaptor = MakeInMemoryTextureAdaptor(source, mAllocator);
    FBlobDeserializer blobs(Span<const unsigned char>(source.bytes.data(), source.bytes.size()));
    Result r = Upload(&blobs, adaptor, outTextureIndex, debugName, pinned);
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
    EnqueueUpload(mBufferQueue, std::move(pending));
    return Result::InProgress;
}

GPUScene::Result GPUSceneImpl::Query(GeometryHandle handle) const
{
    GeometryResidency const* g = ResolveGeometry(handle);
    if (!g)
        return Result::InvalidHandle;
    switch (g->state)
    {
    case ResourceState::Ready: return Result::Ready;
    case ResourceState::Failed: return Result::DecodeFailed;
    default: return Result::InProgress;
    }
}

GPUScene::Result GPUSceneImpl::Query(TextureHandle texture) const
{
    Vector<TextureSlot> const& slots = TextureSlots(texture.is3D);
    if (!texture.IsValid() || texture.index >= slots.size() || !slots[texture.index].live ||
        slots[texture.index].generation != texture.generation)
        return Result::InvalidHandle;
    // The background drain flips `resident` once the image is uploaded + transitioned, so this
    // is safe to call every frame while Poll() streams (the renderer falls back to defaults
    // until then).
    std::lock_guard<Mutex> lock(mResidencyMutex);
    return slots[texture.index].resident ? Result::Ready : Result::InProgress;
}

template <typename T>
void GPUSceneImpl::EnqueueUpload(MPMCQueue<T>& queue, T&& item)
{
    mOutstanding.fetch_add(1, std::memory_order_release);
    // The queues are sized to budget, so Push only transiently fails under extreme bursts;
    // yield-and-retry rather than dropping work.
    while (!queue.Push(std::move(item)))
        std::this_thread::yield();
    {
        std::lock_guard<Mutex> lock(mWorkMutex);
        mHasWork = true;
    }
    mWorkCV.notify_one();
}

bool GPUSceneImpl::DrainUploadBatch()
{
    // Pull everything currently queued into one batch so geometry/texture/buffer uploads keep
    // their batched transfer + BLAS submissions (processing items one-by-one would be far less
    // efficient). Work enqueued mid-drain is picked up by the next call.
    Vector<PendingGeometryUpload> geometry(mAllocator);
    Vector<PendingTextureUpload> textures(mAllocator);
    Vector<PendingBufferUpload> buffers(mAllocator);
    PendingGeometryUpload g;
    while (mGeometryQueue.Pop(g))
        geometry.push_back(std::move(g));
    PendingTextureUpload t;
    while (mTextureQueue.Pop(t))
        textures.push_back(std::move(t));
    PendingBufferUpload b;
    while (mBufferQueue.Pop(b))
        buffers.push_back(std::move(b));

    size_t const count = geometry.size() + textures.size() + buffers.size();
    if (count == 0)
        return false;

    try
    {
        ProcessUploads(geometry, textures, buffers);
    }
    catch (std::exception const& e)
    {
        mUploadFailed.store(true, std::memory_order_release);
        LOG(GPUScene, LogError, "Background upload failed: {}", e.what());
    }
    catch (...)
    {
        mUploadFailed.store(true, std::memory_order_release);
        LOG(GPUScene, LogError, "Background upload failed");
    }

    mOutstanding.fetch_sub(count, std::memory_order_release);
    {
        std::lock_guard<Mutex> lock(mWorkMutex);
    }
    mDoneCV.notify_all();
    return true;
}

void GPUSceneImpl::UploadWorker()
{
    while (true)
    {
        while (DrainUploadBatch())
        {
        }
        std::unique_lock<Mutex> lock(mWorkMutex);
        if (mStop)
            return;
        // Re-check under the lock: a producer may have enqueued between the empty drain above
        // and acquiring the lock (it sets mHasWork under the same lock), so we can't miss it.
        if (mHasWork)
        {
            mHasWork = false;
            continue;
        }
        mWorkCV.wait(lock, [this] { return mHasWork || mStop; });
        mHasWork = false;
        if (mStop)
            return;
    }
}

void GPUSceneImpl::Join()
{
    // Started: wait for the persistent worker to make everything resident. Not started yet
    // (e.g. constructor default uploads): drain synchronously on the calling thread.
    bool started;
    {
        std::lock_guard<Mutex> lock(mWorkMutex);
        started = mWorkerStarted;
    }
    if (started)
    {
        std::unique_lock<Mutex> lock(mWorkMutex);
        mDoneCV.wait(lock, [this] {
            return mOutstanding.load(std::memory_order_acquire) == 0 ||
                   mUploadFailed.load(std::memory_order_acquire);
        });
        return;
    }
    while (DrainUploadBatch())
    {
    }
}

GPUScene::Result GPUSceneImpl::Poll()
{
    if (mUploadFailed.load(std::memory_order_acquire))
        return Result::SubmitFailed;
    if (mOutstanding.load(std::memory_order_acquire) == 0)
        return Result::Ready;
    // Lazily spin up the persistent worker the first time we're polled with outstanding work.
    {
        std::lock_guard<Mutex> lock(mWorkMutex);
        if (!mWorkerStarted)
        {
            mWorkerStarted = true;
            mUploadThread = Thread([this] { UploadWorker(); });
        }
    }
    return Result::InProgress;
}

void GPUSceneImpl::ProcessUploads(Vector<PendingGeometryUpload>& geometry,
                                  Vector<PendingTextureUpload>& textures,
                                  Vector<PendingBufferUpload>& buffers)
{
    // Alias the batch vectors to the queue names so the (substantial) drain body reads
    // naturally; they hold just this batch's work, pulled off the lock-free queues.
    auto& mPendingGeometry = geometry;
    auto& mPendingTextures = textures;
    auto& mPendingBuffers = buffers;
    const bool direct = mDirectGeometryUpload;
    const bool hadTextures = !mPendingTextures.empty();

    // --- Budgets: staging lane size and per-worker blob decode scratch. ---
    size_t maxFootprint = 0;
    size_t maxBlob = 0;
    auto IncludeBlobScratch = [&](FBlobRef const& b)
    {
        if (b.codec != FBlobCodec::None)
            maxBlob = std::max(maxBlob, static_cast<size_t>(b.decodedSize));
    };
    size_t totalFootprint = 0;
    if (!direct)
        for (auto const& p : mPendingGeometry)
        {
            maxFootprint = std::max(maxFootprint, p.footprint);
            totalFootprint += p.footprint;
        }
    for (auto const& p : mPendingGeometry)
    {
        if (p.mesh)
        {
            IncludeBlobScratch(p.mesh->vertices);
            if (!p.mesh->lods.empty())
                IncludeBlobScratch(p.mesh->lods[0].indices);
            IncludeBlobScratch(p.mesh->dagGroups);
            IncludeBlobScratch(p.mesh->dagMeshlets);
            IncludeBlobScratch(p.mesh->dagMeshletVtx);
            IncludeBlobScratch(p.mesh->dagMeshletTri);
        }
        else if (p.curve)
        {
            IncludeBlobScratch(p.curve->points);
            IncludeBlobScratch(p.curve->segments);
            IncludeBlobScratch(p.curve->aabbs);
        }
    }
    size_t textureSubresCount = 0;
    for (auto const& p : mPendingTextures)
    {
        FTextureHeader const& md = static_cast<FTextureHeader const&>(*p.source);
        for (uint32_t layer = 0; layer < md.GetNumLayers(); ++layer)
            for (uint32_t mip = 0; mip < md.GetNumMips(); ++mip)
            {
                size_t fp = GPUSceneTextureSubresourceFootprint(md, layer, mip) + kUploadBudgetSlack;
                maxFootprint = std::max(maxFootprint, fp);
                totalFootprint += fp;
                IncludeBlobScratch(p.source->GetSubresourceBlob(layer, mip));
                ++textureSubresCount;
            }
    }
    for (auto const& b : mPendingBuffers)
    {
        maxFootprint = std::max(maxFootprint, b.data.size());
        totalFootprint += b.data.size();
    }
    // The lane must fit the largest resource; the extra slack lets several resources
    // pack per flush during big loads, but is clamped to the total so single small
    // uploads (LUTs, env CDFs) don't over-allocate staging.
    const size_t stagingSlack = std::min(totalFootprint, kUploadStagingBudgetSlack);
    const size_t stagingBudget = std::max<size_t>(maxFootprint, 1u) + stagingSlack;

    // --- Threaded blob decode pool + per-worker scratch lanes. ---
    const size_t taskUpper = mPendingGeometry.size() * 7u + textureSubresCount;
    const size_t workerCount = std::min<size_t>(std::max<size_t>(1u, std::thread::hardware_concurrency()),
                                                std::max<size_t>(1u, taskUpper));
    ThreadPool pool(workerCount, ThreadPool::CalcTaskSize(std::max<size_t>(taskUpper, 1u) + 2u), mAllocator,
                    "GPUSceneUpload");
    const size_t laneBudget = std::max<size_t>(maxBlob + kUploadBudgetSlack, alignof(std::max_align_t));
    ScopedArena scratchArena(mAllocator, laneBudget * workerCount);
    CHECK(scratchArena);
    Vector<Arena> scratchArenas(workerCount, mAllocator);
    Vector<AllocatorStack> scratchAllocators(workerCount, mAllocator);
    char* scratchMem = static_cast<char*>(scratchArena.arena.memory);
    for (size_t i = 0; i < workerCount; ++i)
    {
        scratchArenas[i] = Arena{scratchMem + i * laneBudget, laneBudget};
        scratchAllocators[i].Reset(scratchArenas[i]);
    }
    Span<Arena> arenaSpan(scratchArenas.data(), scratchArenas.size());
    Span<AllocatorStack> allocSpan(scratchAllocators.data(), scratchAllocators.size());

    ImmediateUpload upload(mDevice, stagingBudget, RHIDeviceQueueType::Transfer, kUploadStagingBuffers);
    upload.Begin();
    Atomic<size_t> pendingJobs{0};
    Vector<GPUSceneBlobWrite> writes(mAllocator);
    auto ScheduleWrites = [&](FBlobDeserializer const& blobs)
    {
        if (writes.empty())
            return;
        pendingJobs.fetch_add(writes.size(), std::memory_order_relaxed);
        for (auto const& w : writes)
            pool.PushImpl<GPUSceneBlobDecodeJob>(w, blobs, arenaSpan, allocSpan, &pendingJobs);
        writes.clear();
    };
    auto FlushUpload = [&]
    {
        GPUSceneWaitJobs(&pendingJobs);
        upload.End();
        upload.Begin();
    };

    auto uploadTimeline = mDevice->CreateSemaphore(true);
    constexpr size_t kGeometryReady = 1u;
    constexpr size_t kTextureReady = 2u;

    // --- Geometry: first-fit-decreasing staging, threaded decode, transfer, BLAS. ---
    {
        Vector<size_t> order(mPendingGeometry.size(), mAllocator);
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::sort(order.begin(), order.end(),
                  [&](size_t a, size_t b) { return mPendingGeometry[a].footprint > mPendingGeometry[b].footprint; });
        for (size_t idx : order)
        {
            PendingGeometryUpload& p = mPendingGeometry[idx];
            GeometryResidency* g = ResolveGeometry(p.handle);
            CHECK_MSG(g, "Pending geometry references a freed slot");
            auto Stage = [&] {
                return p.mesh ? StageMesh(&upload, *p.mesh, g->mesh, g->resourceOffset, writes)
                              : StageCurve(&upload, *p.curve, g->curve, g->resourceOffset, writes);
            };
            size_t staged = Stage();
            if (staged == 0)
            {
                FlushUpload();
                staged = Stage();
                CHECK_MSG(staged != 0, "Staging buffer too small for a single geometry upload");
            }
            ScheduleWrites(p.blobs);
        }
        // Plain buffer copies share the geometry transfer submit (no decode jobs).
        for (auto const& b : mPendingBuffers)
        {
            char* ptr = upload.Upload(b.dst, b.data.size(), b.dstOffset);
            if (!ptr)
            {
                FlushUpload();
                ptr = upload.Upload(b.dst, b.data.size(), b.dstOffset);
                CHECK_MSG(ptr != nullptr, "Staging buffer too small for a single buffer upload");
            }
            std::memcpy(ptr, b.data.data(), b.data.size());
        }
        GPUSceneWaitJobs(&pendingJobs);
        if (direct)
            FlushDirectGeometryUpload();
        RHIDeviceQueue::TimelinePair geomSignal{uploadTimeline.Get(), kGeometryReady};
        upload.End(ImmediateSubmitDesc{.timelineSignals = {&geomSignal, 1}});
    }

    if (!mPendingGeometry.empty())
    {
        ImmediateContext blasCtx(RHIDeviceQueueType::Compute, mDevice);
        Vector<GeometryHandle> meshHandles(mAllocator), curveHandles(mAllocator);
        Vector<GSMesh> meshHeaders(mAllocator);
        Vector<GSCurveSet> curveHeaders(mAllocator);
        for (auto const& p : mPendingGeometry)
        {
            GeometryResidency* g = ResolveGeometry(p.handle);
            CHECK(g);
            if (g->type == kGSInstanceTypeCurve)
                curveHandles.push_back(p.handle), curveHeaders.push_back(g->curve);
            else
                meshHandles.push_back(p.handle), meshHeaders.push_back(g->mesh);
        }
        RHIDeviceQueue::TimelinePair wait{uploadTimeline.Get(), kGeometryReady};
        RHIPipelineStage waitStage = RHIPipelineStageBits::AccelerationBuild;
        ImmediateSubmitDesc firstSubmit{.timelineWaits = {&wait, 1}, .waitStages = {&waitStage, 1}};
        constexpr size_t kBLASBuildBatch = 32u;
        bool usedFirst = false;
        // Each BuildBLAS batch waits on the geometry transfer (first batch) and host-waits its
        // own compute work, so a batch's meshes are fully GPU-resident once it returns. Publish
        // residency per batch under mResidencyMutex so the renderer streams instances into the
        // TLAS as batches land instead of all at the end.
        auto Publish = [&](Span<const GeometryHandle> handles, Span<const uint32_t> slots)
        {
            std::lock_guard<Mutex> lock(mResidencyMutex);
            for (size_t j = 0; j < handles.size(); ++j)
            {
                GeometryResidency* g = ResolveGeometry(handles[j]);
                CHECK(g);
                g->blasSlot = slots[j];
                g->state = ResourceState::Ready;
            }
        };
        Vector<uint32_t> meshSlots(meshHeaders.size(), mAllocator);
        for (size_t i = 0; i < meshHeaders.size(); i += kBLASBuildBatch)
        {
            size_t bs = std::min(kBLASBuildBatch, meshHeaders.size() - i);
            BuildBLAS(&blasCtx, Span<const GSMesh>(meshHeaders.data() + i, bs), Span<uint32_t>(meshSlots.data() + i, bs),
                      usedFirst ? ImmediateSubmitDesc{} : firstSubmit);
            usedFirst = true;
            Publish(Span<const GeometryHandle>(meshHandles.data() + i, bs),
                    Span<const uint32_t>(meshSlots.data() + i, bs));
        }
        Vector<uint32_t> curveSlots(curveHeaders.size(), mAllocator);
        for (size_t i = 0; i < curveHeaders.size(); i += kBLASBuildBatch)
        {
            size_t bs = std::min(kBLASBuildBatch, curveHeaders.size() - i);
            BuildCurveBLAS(&blasCtx, Span<const GSCurveSet>(curveHeaders.data() + i, bs),
                           Span<uint32_t>(curveSlots.data() + i, bs), usedFirst ? ImmediateSubmitDesc{} : firstSubmit);
            usedFirst = true;
            Publish(Span<const GeometryHandle>(curveHandles.data() + i, bs),
                    Span<const uint32_t>(curveSlots.data() + i, bs));
        }
    }
    mPendingGeometry.clear();

    // --- Textures: stage subresources, transition, transfer, mark Ready. ---
    if (!mPendingTextures.empty())
    {
        upload.Begin();
        struct Subresource { size_t slot; uint32_t layer; uint32_t mip; size_t footprint; };
        Vector<Subresource> subs(mAllocator);
        for (size_t slot = 0; slot < mPendingTextures.size(); ++slot)
        {
            FTextureHeader const& md = static_cast<FTextureHeader const&>(*mPendingTextures[slot].source);
            for (uint32_t layer = 0; layer < md.GetNumLayers(); ++layer)
                for (uint32_t mip = 0; mip < md.GetNumMips(); ++mip)
                    subs.push_back({slot, layer, mip, GPUSceneTextureSubresourceFootprint(md, layer, mip)});
        }
        std::sort(subs.begin(), subs.end(),
                  [](Subresource const& a, Subresource const& b) { return a.footprint > b.footprint; });
        Vector<uint8_t> transferDstDone(mPendingTextures.size(), 0u, mAllocator);
        for (Subresource const& sub : subs)
        {
            PendingTextureUpload& pt = mPendingTextures[sub.slot];
            FTextureHeader const& md = static_cast<FTextureHeader const&>(*pt.source);
            RHITexture* tex = SelectTexturePool(md.GetViewDimension()).GetResource(pt.index);
            CHECK(tex != nullptr);
            auto StageSub = [&] {
                if (!transferDstDone[sub.slot])
                {
                    TransitionTextureLayout(upload.Get(), tex, md, RHITextureLayout::Undefined,
                                            RHITextureLayout::TransferDst);
                    transferDstDone[sub.slot] = 1u;
                }
                return StageTextureSubresource(&upload, *pt.source, tex, sub.layer, sub.mip, writes);
            };
            size_t staged = StageSub();
            if (staged == 0)
            {
                FlushUpload();
                staged = StageSub();
                CHECK_MSG(staged != 0, "Staging buffer too small for a single texture subresource");
            }
            ScheduleWrites(pt.blobs);
        }
        GPUSceneWaitJobs(&pendingJobs);
        for (size_t slot = 0; slot < mPendingTextures.size(); ++slot)
        {
            if (!transferDstDone[slot])
                continue;
            PendingTextureUpload& pt = mPendingTextures[slot];
            FTextureHeader const& md = static_cast<FTextureHeader const&>(*pt.source);
            RHITexture* tex = SelectTexturePool(md.GetViewDimension()).GetResource(pt.index);
            TransitionTextureLayout(upload.Get(), tex, md, RHITextureLayout::TransferDst,
                                    RHITextureLayout::ShaderReadOnly);
        }
        RHIDeviceQueue::TimelinePair texSignal{uploadTimeline.Get(), kTextureReady};
        upload.End(ImmediateSubmitDesc{.timelineSignals = {&texSignal, 1}});
    }
    // Remember which slots this drain transitioned so they can be published as resident once
    // the transfer completes below (the render thread gates material textures on this).
    Vector<std::pair<uint32_t, bool>> uploadedTextures(mAllocator);
    uploadedTextures.reserve(mPendingTextures.size());
    for (auto const& pt : mPendingTextures)
        uploadedTextures.emplace_back(
            pt.index, IsTexture3DView(static_cast<FTextureHeader const&>(*pt.source).GetViewDimension()));
    mPendingTextures.clear();
    mPendingBuffers.clear();

    // Block until the last submitted transfer completes. The texture submit (if any)
    // chains after the geometry+buffer submit on the Transfer queue, so waiting the
    // highest signaled value covers everything.
    RHIDeviceQueue::TimelinePair finalWait{uploadTimeline.Get(), hadTextures ? kTextureReady : kGeometryReady};
    mDevice->WaitForTimelineSemaphores(Span<const RHIDeviceQueue::TimelinePair>(&finalWait, 1), -1);

    if (!uploadedTextures.empty())
    {
        std::lock_guard<Mutex> lock(mResidencyMutex);
        for (auto const& [index, is3D] : uploadedTextures)
        {
            Vector<TextureSlot>& slots = TextureSlots(is3D);
            if (index < slots.size())
                slots[index].resident = true;
        }
    }

    pool.Join();
}

// Reference:
// - https://github.com/zeux/niagara/blob/master/src/scenert.cpp
// - https://docs.vulkan.org/tutorial/latest/courses/18_Ray_tracing/02_Acceleration_structures.html
// - https://docs.vulkan.org/tutorial/latest/courses/18_Ray_tracing/04_TLAS_animation.html
void GPUSceneImpl::BuildBLAS(ImmediateContext* ctx, Span<const GSMesh> meshes, Span<uint32_t> outBLASIndices,
                         ImmediateSubmitDesc const& firstSubmitDesc)
{
    CHECK_MSG(meshes.size() == outBLASIndices.size(), "Mismatched BLAS indices size");
    if (meshes.empty())
        return;

    auto* device = mDevice;
    // Build
    Vector<RHIAccelerationStructureGeometryInfo> geometries(meshes.size(), mAllocator);
    Vector<RHIAccelerationStructureBuildRangeInfo> buildRanges(meshes.size(), mAllocator);
    Vector<RHIAccelerationStructureBuildDesc> buildDesc(meshes.size(), mAllocator);
    Vector<RHIAccelerationStructureSizeInfo> sizeInfo(meshes.size(), mAllocator);
    Vector<uint32_t> blasOffsets(meshes.size(), mAllocator);
    Vector<uint32_t> scratchOffsets(meshes.size(), mAllocator);
    StackArena<4096> sizeInfoArena;
    AllocatorStack sizeInfoScratch(sizeInfoArena);
    auto* primitiveBuffer = owner.mPrimitiveBuffer.Get();
    uint32_t scratchOffset = 0, blasOffset = 0;
    for (size_t i = 0; i < meshes.size(); i++)
    {
        auto const& mesh = meshes[i];
        auto& geo = geometries[i];
        auto& range = buildRanges[i];
        geo.type = RHIAccelerationGeometryType::Triangles;
        geo.triangleData = RHIAccelerationStructureGeometryTriangleData{
            // FP16 positions
            .vertexFormat = RHIResourceFormat::R16G16B16A16SignedFloat,
            .vertexBuffer = primitiveBuffer,
            .vertexOffset = mesh.vtxOffset,
            .vertexCount = mesh.vtxCount,
            .vertexStride = sizeof(FQVertex),
            .indexFormat = RHIResourceFormat::R32Uint,
            .indexBuffer = primitiveBuffer,
            .indexOffset = mesh.idxOffset,
            .indexCount = mesh.idxCount
        };
        range = RHIAccelerationStructureBuildRangeInfo{
            .primitiveCount = mesh.idxCount / 3
        };
        auto& desc = buildDesc[i];
        desc = RHIAccelerationStructureBuildDesc{
            .type = RHIAccelerationStructureType::BottomLevel,
            .flags = RHIAccelerationStructureBuildFlagsBits::PreferFastTrace |
            RHIAccelerationStructureBuildFlagsBits::AllowUpdate |
            RHIAccelerationStructureBuildFlagsBits::AllowCompaction,
            .operation = RHIAccelerationStructureBuildOp::Build,
            .geometries = Span<const RHIAccelerationStructureGeometryInfo>{&geo, 1},
            .ranges = Span<const RHIAccelerationStructureBuildRangeInfo>{&range, 1}
        };
        sizeInfoScratch.Reset(sizeInfoArena);
        sizeInfo[i] = device->GetAccelerationStructureSizeInfo(desc, sizeInfoScratch.Ptr());
        blasOffsets[i] = blasOffset;
        // minAccelerationStructureScratchOffsetAlignment is 256
        blasOffset = AlignUp(blasOffset + sizeInfo[i].accelerationStructureSize, 256u);
        scratchOffsets[i] = scratchOffset;
        scratchOffset = AlignUp(scratchOffset + sizeInfo[i].buildScratchSize, 256u);
    }
    auto scratch = mDevice->CreateBuffer(
    {
        .resource = {
            .heap = RHIDeviceHeapType::Local,
            .shared = false
        },
        .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress,
        .size = scratchOffset,
        .alignment = 256 // Aligned to Vulkan spec. Should be large enough for other APIs as well?
    });
    auto buffer = mDevice->CreateBuffer(
    {
        .resource = {
            .heap = RHIDeviceHeapType::Local,
            .shared = false
        },
        .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress |
        RHIBufferUsageBits::AccelerationStructureStorage,
        .size = blasOffset
    });
    Vector<RHIDeviceHandle<RHIAccelerationStructure>> newBLASHandles(mAllocator);
    Vector<RHIAccelerationStructure*> newBLASPtrs(mAllocator);
    newBLASHandles.reserve(meshes.size());
    newBLASPtrs.reserve(meshes.size());
    auto* cmd = ctx->Get();
    cmd->Begin();
    for (size_t i = 0; i < meshes.size(); i++)
    {
        RHIAccelerationStructureDesc as{
            .type = RHIAccelerationStructureType::BottomLevel,
            .flags = RHIAccelerationStructureBuildFlagsBits::PreferFastTrace |
            RHIAccelerationStructureBuildFlagsBits::AllowCompaction,
            .buffer = buffer.Get(),
            .offset = blasOffsets[i],
            .size = sizeInfo[i].accelerationStructureSize
        };
        auto blasHandle = device->CreateAccelerationStructure(as);
        RHIAccelerationStructure* blas = blasHandle.Get();
        newBLASHandles.push_back(blasHandle.Release());
        newBLASPtrs.push_back(blas);
        auto& desc = buildDesc[i];
        cmd->BeginTransition();
        cmd->SetBufferTransition(scratch.Get(), {
                                     .srcStage = RHIPipelineStageBits::AccelerationBuild,
                                     .dstStage = RHIPipelineStageBits::AccelerationBuild
                                 });
        cmd->EndTransition();
        desc.scratchBuffer = scratch.Get();
        desc.scratchBufferOffset = scratchOffsets[i];
        desc.dstAS = blas;
        cmd->BuildAccelerationStructure({{{desc}}});
    }
    cmd->End();
    ctx->Submit(firstSubmitDesc), ctx->WaitIdle();
    // Compact
    auto queryPool = device->CreateQueryPool({
        .type = RHIDeviceQueryPool::QueryPoolDesc::AccelerationStructureCompactedSize,
        .count = static_cast<uint32_t>(meshes.size())
    });
    queryPool->Reset();
    cmd->Begin();
    cmd->WriteAccelerationStructureCompactedSize(newBLASPtrs, queryPool.Get(), 0);
    cmd->End(), ctx->Submit(), ctx->WaitIdle();
    uint32_t compactOffset = 0;
    Vector<uint32_t> compactOffsets(meshes.size(), mAllocator);
    auto compactSizes = queryPool->GetResults();
    for (size_t i = 0; i < meshes.size(); i++)
    {
        auto compactedSize = compactSizes[i];
        compactOffsets[i] = compactOffset;
        compactOffset = AlignUp(compactOffset + static_cast<uint32_t>(compactedSize), 256u);
    }
    auto& compactBuffer = mBLASBuffers.emplace_back(mDevice->CreateBuffer(
    {
        .resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
        .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress |
        RHIBufferUsageBits::AccelerationStructureStorage,
        .size = compactOffset
    }));
    cmd->Begin();
    for (size_t i = 0; i < meshes.size(); i++)
    {
        RHIAccelerationStructureDesc as{
            .type = RHIAccelerationStructureType::BottomLevel,
            .flags = RHIAccelerationStructureBuildFlagsBits::PreferFastTrace |
            RHIAccelerationStructureBuildFlagsBits::AllowCompaction,
            .buffer = compactBuffer.Get(),
            .offset = compactOffsets[i],
            .size = static_cast<uint32_t>(compactSizes[i])
        };
        uint32_t slot = AcquireMeshBLASSlot();
        outBLASIndices[i] = slot;
        mBLASes[slot] = device->CreateAccelerationStructure(as);
        cmd->CopyAccelerationStructure(newBLASPtrs[i], mBLASes[slot].Get(), true /* compact */);
    }
    cmd->End(), ctx->Submit(), ctx->WaitIdle();
    for (auto handle : newBLASHandles)
        RHIDeviceScopedHandle<RHIAccelerationStructure>(handle.mFactory, handle.mHandle).Reset();
    LOG(GPUScene, LogDebug, "BLAS Upload Complete: {} BLASes, {} MB used (compacted from {} MB)",
        meshes.size(),
        compactOffset / 1e6,
        blasOffset / 1e6);
}

void GPUSceneImpl::BuildCurveBLAS(ImmediateContext* ctx, Span<const GSCurveSet> curves, Span<uint32_t> outBLASIndices,
                              ImmediateSubmitDesc const& firstSubmitDesc)
{
    CHECK_MSG(curves.size() == outBLASIndices.size(), "Mismatched curve BLAS indices size");
    if (curves.empty())
        return;

    auto* device = mDevice;
    Vector<RHIAccelerationStructureGeometryInfo> geometries(curves.size(), mAllocator);
    Vector<RHIAccelerationStructureBuildRangeInfo> buildRanges(curves.size(), mAllocator);
    Vector<RHIAccelerationStructureBuildDesc> buildDesc(curves.size(), mAllocator);
    Vector<RHIAccelerationStructureSizeInfo> sizeInfo(curves.size(), mAllocator);
    Vector<uint32_t> blasOffsets(curves.size(), mAllocator);
    Vector<uint32_t> scratchOffsets(curves.size(), mAllocator);
    StackArena<4096> sizeInfoArena;
    AllocatorStack sizeInfoScratch(sizeInfoArena);
    uint32_t scratchOffset = 0, blasOffset = 0;
    for (size_t i = 0; i < curves.size(); i++)
    {
        auto const& curve = curves[i];
        auto& geo = geometries[i];
        auto& range = buildRanges[i];
        geo.type = RHIAccelerationGeometryType::AABBs;
        geo.aabbData = RHIAccelerationStructureGeometryAABBData{
            .aabbBuffer = mCurveAABBBuffer.Get(),
            .offset = curve.aabbOffset,
            .count = curve.segmentCount,
            .stride = sizeof(RHIAccelerationStructureAABB)
        };
        range = RHIAccelerationStructureBuildRangeInfo{.primitiveCount = curve.segmentCount};
        auto& desc = buildDesc[i];
        desc = RHIAccelerationStructureBuildDesc{
            .type = RHIAccelerationStructureType::BottomLevel,
            .flags = RHIAccelerationStructureBuildFlagsBits::PreferFastTrace,
            .operation = RHIAccelerationStructureBuildOp::Build,
            .geometries = Span<const RHIAccelerationStructureGeometryInfo>{&geo, 1},
            .ranges = Span<const RHIAccelerationStructureBuildRangeInfo>{&range, 1}
        };
        sizeInfoScratch.Reset(sizeInfoArena);
        sizeInfo[i] = device->GetAccelerationStructureSizeInfo(desc, sizeInfoScratch.Ptr());
        blasOffsets[i] = blasOffset = AlignUp(blasOffset, 256u);
        blasOffset += sizeInfo[i].accelerationStructureSize;
        scratchOffsets[i] = scratchOffset = AlignUp(scratchOffset, 256u);
        scratchOffset += sizeInfo[i].buildScratchSize;
    }

    mCurveBLASBuffers.emplace_back(device->CreateBuffer({
        .resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
        .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress |
        RHIBufferUsageBits::AccelerationStructureStorage,
        .size = blasOffset
    }));
    auto scratchBuffer = device->CreateBuffer({
        .resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
        .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress,
        .size = scratchOffset,
        .alignment = 256
    });

    RHIBuffer* blasBuffer = mCurveBLASBuffers.back().Get();
    Vector<RHIDeviceScopedHandle<RHIAccelerationStructure>> newBLASes(curves.size(), mAllocator);
    auto* cmd = ctx->Get();
    cmd->Begin();
    for (size_t i = 0; i < curves.size(); i++)
    {
        auto& desc = buildDesc[i];
        RHIAccelerationStructureDesc as{
            .type = RHIAccelerationStructureType::BottomLevel,
            .flags = RHIAccelerationStructureBuildFlagsBits::PreferFastTrace,
            .buffer = blasBuffer,
            .offset = blasOffsets[i],
            .size = sizeInfo[i].accelerationStructureSize
        };
        uint32_t slot = AcquireCurveBLASSlot();
        outBLASIndices[i] = slot;
        mCurveBLASes[slot] = device->CreateAccelerationStructure(as);
        desc.scratchBuffer = scratchBuffer.Get();
        desc.scratchBufferOffset = scratchOffsets[i];
        desc.dstAS = mCurveBLASes[slot].Get();
        cmd->BuildAccelerationStructure({{{desc}}});
    }
    cmd->End(), ctx->Submit(firstSubmitDesc), ctx->WaitIdle();
    LOG(GPUScene, LogDebug, "Curve BLAS Upload Complete: {} BLASes, {} MB used",
        curves.size(), blasOffset / 1e6);
}

/* --- Dynamic (CPU-updateable) geometry --- */

void GPUSceneImpl::WriteDynamicHeader(GeometryResidency& g, uint32_t slot)
{
    CHECK(mDynamicPrimitiveMapped != nullptr);
    uint32_t const base = DynamicRegionBase(g, slot);
    GSMesh h{};
    h.vtxOffset = base + sizeof(GSMesh);
    h.vtxCount = g.mesh.vtxCount;
    h.idxOffset = h.vtxOffset + g.dynamicVtxBytes;
    h.idxCount = g.mesh.idxCount;
    // No DAG / meshlets for dynamic geometry: it is drawn through a plain vertex/index path,
    // never the meshlet pipeline. All group/meshlet fields stay zero.
    std::memcpy(mDynamicPrimitiveMapped + base, &h, sizeof(GSMesh));
}

void GPUSceneImpl::AllocateDynamicBLAS(GeometryResidency& g)
{
    CHECK(mDynamicPrimitiveBuffer);
    // PreferFastBuild + AllowUpdate, NO compaction: the AS must stay refittable in place and its
    // size stable across refits/rebuilds (compacted ASes are immutable). The size is independent
    // of which ring slot the geometry is read from, so query against slot 0.
    uint32_t const base = DynamicRegionBase(g, 0);
    RHIAccelerationStructureGeometryInfo geo{
        .type = RHIAccelerationGeometryType::Triangles,
        .triangleData = {
            .vertexFormat = RHIResourceFormat::R16G16B16A16SignedFloat,
            .vertexBuffer = mDynamicPrimitiveBuffer.Get(),
            .vertexOffset = base + sizeof(GSMesh),
            .vertexCount = g.mesh.vtxCount,
            .vertexStride = sizeof(FQVertex),
            .indexFormat = RHIResourceFormat::R32Uint,
            .indexBuffer = mDynamicPrimitiveBuffer.Get(),
            .indexOffset = base + sizeof(GSMesh) + g.dynamicVtxBytes,
            .indexCount = g.mesh.idxCount,
        }};
    RHIAccelerationStructureBuildRangeInfo range{.primitiveCount = g.mesh.idxCount / 3};
    RHIAccelerationStructureBuildFlags const flags =
        RHIAccelerationStructureBuildFlagsBits::PreferFastBuild | RHIAccelerationStructureBuildFlagsBits::AllowUpdate;
    RHIAccelerationStructureBuildDesc desc{
        .type = RHIAccelerationStructureType::BottomLevel,
        .flags = flags,
        .operation = RHIAccelerationStructureBuildOp::Build,
        .geometries = Span<const RHIAccelerationStructureGeometryInfo>{&geo, 1},
        .ranges = Span<const RHIAccelerationStructureBuildRangeInfo>{&range, 1}};
    StackArena<4096> sizeInfoArena;
    AllocatorStack sizeInfoScratch(sizeInfoArena);
    auto size = mDevice->GetAccelerationStructureSizeInfo(desc, sizeInfoScratch.Ptr());
    uint32_t const scratchSize = AlignUp(std::max(size.buildScratchSize, size.updateScratchSize), 256u);
    g.dynBLASBuffer = mDevice->CreateBuffer(
        {.resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
         .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress |
                  RHIBufferUsageBits::AccelerationStructureStorage,
         .size = AlignUp(size.accelerationStructureSize, 256u)});
    g.dynScratchBuffer = mDevice->CreateBuffer(
        {.resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
         .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress,
         .size = scratchSize,
         .alignment = 256});
    if (g.blasSlot == UINT32_MAX)
        g.blasSlot = AcquireMeshBLASSlot();
    mBLASes[g.blasSlot] = mDevice->CreateAccelerationStructure({
        .type = RHIAccelerationStructureType::BottomLevel,
        .flags = flags,
        .buffer = g.dynBLASBuffer.Get(),
        .offset = 0,
        .size = size.accelerationStructureSize});
    // No GPU build here: the first RefitDynamicGeometry records it (operation=Build) inside the
    // render graph, ordered before the TLAS update by the AS barrier. Until then dynBuilt is
    // false so the geometry is skipped by BuildTLAS (it "streams in" a frame later, like uploads).
    g.dynBuilt = false;
    g.framesSinceRebuild = 0;
}

GPUScene::Result GPUSceneImpl::UploadDynamic(FBlobDeserializer* blobs, FSerializedMesh const& source,
                                             GeometryHandle& outHandle)
{
    CHECK(blobs != nullptr);
    if (!mDynamicPrimitiveBuffer)
    {
        LOG(GPUScene, LogError, "UploadDynamic called but GPUSceneDesc::dynamicGeometryBudget is 0 (feature disabled)");
        return Result::InvalidInput;
    }
    if (source.lods.empty())
        return Result::InvalidInput;
    auto const& lod0 = source.lods[0];
    uint32_t const vtxBytes = static_cast<uint32_t>(source.vertices.decodedSize);
    uint32_t const idxBytes = static_cast<uint32_t>(lod0.indices.decodedSize);
    if (vtxBytes == 0 || idxBytes == 0)
        return Result::InvalidInput;
    uint32_t const footprint = static_cast<uint32_t>(sizeof(GSMesh)) + vtxBytes + idxBytes;
    // Per-slot stride is the geo's own footprint aligned to 16, so every slot's header (4B reads),
    // verts and indices (idxOffset/4 must be integral for the raster index buffer) stay aligned.
    uint32_t const stride = AlignUp(footprint, 16u);

    GeometryHandle handle{};
    GeometryResidency* gp = nullptr;
    {
        // Claim the residency slot + ring region under the residency lock (consistent with the
        // immutable Upload paths and with the render thread's mGeometry reads). One contiguous
        // (frames * stride) block holds this geo's per-frame slots.
        std::lock_guard<Mutex> lock(mResidencyMutex);
        uint64_t base = mDynamicPrimitiveAlloc->Allocate(static_cast<uint64_t>(stride) * mDynamicFrameCount, 16);
        if (base == RHIVirtualAllocator::kInvalidOffset)
        {
            LOG(GPUScene, LogError, "Dynamic geometry ring overflow. Need {} bytes ({}/slot x {}), {} used of {}",
                stride * mDynamicFrameCount, stride, mDynamicFrameCount, mDynamicPrimitiveAlloc->GetUsedBytes(),
                mDynamicPrimitiveAlloc->GetCapacity());
            return Result::OutOfMemory;
        }
        uint32_t slot = AcquireGeometrySlot();
        if (slot == UINT32_MAX)
        {
            mDynamicPrimitiveAlloc->Free(base);
            return Result::OutOfMemory;
        }
        GeometryResidency& g = mGeometry[slot];
        uint32_t const generation = g.generation;
        g = GeometryResidency{}; // releases any retained handles from a recycled slot
        g.generation = generation;
        g.type = kGSInstanceTypeMesh;
        g.dynamic = true;
        g.blasSlot = UINT32_MAX;
        g.resourceOffset = static_cast<uint32_t>(base); // slot 0 base; slot s = base + s*stride
        g.dynamicFootprint = footprint;
        g.dynamicStride = stride;
        g.dynamicVtxBytes = vtxBytes;
        g.dynamicIdxBytes = idxBytes;
        g.mesh = GSMesh{};
        g.mesh.vtxCount = source.vertexCount;
        g.mesh.idxCount = lod0.indexCount;
        g.live = true;
        g.state = ResourceState::Ready; // resident synchronously below
        handle = {slot, generation};
        gp = &g;
        mDynamicGeometrySlots.push_back(slot);
    }
    GeometryResidency& g = *gp;

    // Decode the rest pose into slot 0 (with scratch for compressed blobs), then replicate the
    // whole region to every other slot and fix each slot's header offsets.
    uint32_t const base0 = DynamicRegionBase(g, 0);
    char* v0 = mDynamicPrimitiveMapped + base0 + sizeof(GSMesh);
    char* i0 = v0 + vtxBytes;
    size_t scratchBytes = 0;
    if (source.vertices.codec != FBlobCodec::None)
        scratchBytes = std::max<size_t>(scratchBytes, vtxBytes);
    if (lod0.indices.codec != FBlobCodec::None)
        scratchBytes = std::max<size_t>(scratchBytes, idxBytes);
    if (scratchBytes != 0)
    {
        ScopedArena arena(mAllocator, scratchBytes + 0x100 /* TODO Arbitrary padding, how is this unaccounted for? */);
        AllocatorStack st(arena.arena);
        blobs->ReadBytes(source.vertices, v0, vtxBytes, &st);
        st.Reset(arena.arena);
        blobs->ReadBytes(lod0.indices, i0, idxBytes, &st);
    }
    else
    {
        blobs->ReadBytes(source.vertices, v0, vtxBytes, nullptr);
        blobs->ReadBytes(lod0.indices, i0, idxBytes, nullptr);
    }
    WriteDynamicHeader(g, 0);
    for (uint32_t s = 1; s < mDynamicFrameCount; ++s)
    {
        std::memcpy(mDynamicPrimitiveMapped + DynamicRegionBase(g, s), mDynamicPrimitiveMapped + base0, footprint);
        WriteDynamicHeader(g, s); // overwrite header with slot-correct absolute offsets
    }
    // Allocate the BLAS (host-only) and mark dirty: the first in-graph refit performs the initial
    // GPU build (no synchronous stall). The geo becomes TLAS-visible once that build lands.
    AllocateDynamicBLAS(g);
    g.dirty = true;
    outHandle = handle;
    return Result::Ready;
}

bool GPUSceneImpl::HasDynamicGeometry() const { return !mDynamicGeometrySlots.empty(); }

void GPUSceneImpl::BeginDynamicGeometryUpdate()
{
    CHECK_MSG(!mDynamicUpdateOpen, "BeginDynamicGeometryUpdate called while a window is already open");
    mDynamicUpdateOpen = true;
    if (mDynamicFrameCount != 0)
        mDynamicFrameSlot = (mDynamicFrameSlot + 1u) % mDynamicFrameCount;
}

Span<std::byte> GPUSceneImpl::UpdateDynamicGeometry(GeometryHandle handle)
{
    CHECK_MSG(mDynamicUpdateOpen,
              "UpdateDynamicGeometry must be called inside a BeginDynamicGeometryUpdate / EndDynamicGeometryUpdate window");
    GeometryResidency* g = ResolveGeometry(handle);
    CHECK_MSG(g && g->dynamic, "UpdateDynamicGeometry on a non-dynamic or invalid geometry handle");
    g->dirty = true; // rewriting this slot -> needs a BLAS refit
    uint32_t const base = DynamicRegionBase(*g, mDynamicFrameSlot);
    return Span<std::byte>(reinterpret_cast<std::byte*>(mDynamicPrimitiveMapped + base + sizeof(GSMesh)),
                           g->dynamicVtxBytes);
}

void GPUSceneImpl::EndDynamicGeometryUpdate()
{
    CHECK_MSG(mDynamicUpdateOpen, "EndDynamicGeometryUpdate called without a matching BeginDynamicGeometryUpdate");
    mDynamicUpdateOpen = false;
}

void GPUSceneImpl::BuildBLAS(RHICommandList* cmd)
{
    CHECK_MSG(!mDynamicUpdateOpen,
              "BuildBLAS recorded while a dynamic update window is still open "
              "(call EndDynamicGeometryUpdate before the refit pass)");
    mLastRefitCount = 0;
    mLastRebuildCount = 0;
    if (mDynamicGeometrySlots.empty())
        return;
    // First build (initial, post-upload) and periodic full rebuilds (to recover refit quality
    // decay under large deformation) use operation=Build; everything else refits in place
    // (operation=Update). The single AS stays the same size (AllowUpdate, no compaction) so no
    // realloc. Cadence 0 = refit only (after the initial build).
    bool any = false;
    for (uint32_t slot : mDynamicGeometrySlots)
    {
        GeometryResidency& g = mGeometry[slot];
        if (!g.live || !g.dynamic || g.blasSlot == UINT32_MAX || !g.dirty)
            continue;
        bool const periodic = mDynamicRebuildCadence != 0 && (g.framesSinceRebuild >= mDynamicRebuildCadence);
        bool const rebuild = !g.dynBuilt || periodic; // initial build or cadence -> full Build
        if (rebuild)
            g.framesSinceRebuild = 0;
        else
            ++g.framesSinceRebuild;
        ++mLastRefitCount;
        mLastRebuildCount += rebuild ? 1u : 0u;
        uint32_t const base = DynamicRegionBase(g, mDynamicFrameSlot);
        RHIAccelerationStructureGeometryInfo geo{
            .type = RHIAccelerationGeometryType::Triangles,
            .triangleData = {
                .vertexFormat = RHIResourceFormat::R16G16B16A16SignedFloat,
                .vertexBuffer = mDynamicPrimitiveBuffer.Get(),
                .vertexOffset = base + sizeof(GSMesh),
                .vertexCount = g.mesh.vtxCount,
                .vertexStride = sizeof(FQVertex),
                .indexFormat = RHIResourceFormat::R32Uint,
                .indexBuffer = mDynamicPrimitiveBuffer.Get(),
                .indexOffset = base + sizeof(GSMesh) + g.dynamicVtxBytes,
                .indexCount = g.mesh.idxCount,
            }};
        RHIAccelerationStructureBuildRangeInfo range{.primitiveCount = g.mesh.idxCount / 3};
        RHIAccelerationStructureBuildDesc desc{
            .type = RHIAccelerationStructureType::BottomLevel,
            .flags = RHIAccelerationStructureBuildFlagsBits::PreferFastBuild |
                     RHIAccelerationStructureBuildFlagsBits::AllowUpdate,
            .operation = rebuild ? RHIAccelerationStructureBuildOp::Build : RHIAccelerationStructureBuildOp::Update,
            .geometries = Span<const RHIAccelerationStructureGeometryInfo>{&geo, 1},
            .ranges = Span<const RHIAccelerationStructureBuildRangeInfo>{&range, 1}};
        desc.scratchBuffer = g.dynScratchBuffer.Get();
        desc.scratchBufferOffset = 0;
        desc.dstAS = mBLASes[g.blasSlot].Get();
        if (!rebuild)
            desc.srcAS = mBLASes[g.blasSlot].Get(); // in-place refit (srcAS == dstAS)
        cmd->BuildAccelerationStructure(Span<const RHIAccelerationStructureBuildDesc>{&desc, 1});
        g.dirty = false;
        g.dynBuilt = true; // initial build done -> TLAS-visible; subsequent dirties refit in place
        any = true;
    }
    if (!any)
        return;
    // Make the refit's BLAS writes visible to the subsequent TLAS build's BLAS reads (the graph
    // schedules this pass before "TLAS Update" via the shared TLAS producer edge; this barrier
    // covers the build->build read hazard). Cross-frame reuse of the single AS relies on the same
    // frames-in-flight fencing as the (likewise single, in-place) TLAS.
    cmd->BeginTransition();
    for (uint32_t slot : mDynamicGeometrySlots)
    {
        GeometryResidency const& g = mGeometry[slot];
        if (g.live && g.dynamic && g.blasSlot != UINT32_MAX)
            cmd->SetAccelerationStructureTransition(mBLASes[g.blasSlot].Get(),
                                                    {.srcAccess = RHIResourceAccessBits::AccelerationStructureWrite,
                                                     .dstAccess = RHIResourceAccessBits::AccelerationStructureRead,
                                                     .srcStage = RHIPipelineStageBits::AccelerationBuild,
                                                     .dstStage = RHIPipelineStageBits::AccelerationBuild});
    }
    cmd->EndTransition();
}

uint32_t GPUSceneImpl::AcquireMeshBLASSlot()
{
    if (!mFreeBLASSlots.empty())
    {
        uint32_t slot = mFreeBLASSlots.back();
        mFreeBLASSlots.pop_back();
        return slot;
    }
    mBLASes.emplace_back();
    return static_cast<uint32_t>(mBLASes.size()) - 1;
}

uint32_t GPUSceneImpl::AcquireCurveBLASSlot()
{
    if (!mFreeCurveBLASSlots.empty())
    {
        uint32_t slot = mFreeCurveBLASSlots.back();
        mFreeCurveBLASSlots.pop_back();
        return slot;
    }
    mCurveBLASes.emplace_back();
    return static_cast<uint32_t>(mCurveBLASes.size()) - 1;
}

/* --- Residency table helpers --- */

GPUSceneImpl::GeometryResidency* GPUSceneImpl::ResolveGeometry(GeometryHandle handle)
{
    if (!handle.IsValid() || handle.index >= mGeometry.size())
        return nullptr;
    GeometryResidency& g = mGeometry[handle.index];
    if (!g.live || g.generation != handle.generation)
        return nullptr;
    return &g;
}

GPUSceneImpl::GeometryResidency const* GPUSceneImpl::ResolveGeometry(GeometryHandle handle) const
{
    return const_cast<GPUSceneImpl*>(this)->ResolveGeometry(handle);
}

uint32_t GPUSceneImpl::AcquireGeometrySlot()
{
    if (!mFreeGeometrySlots.empty())
    {
        uint32_t slot = mFreeGeometrySlots.back();
        mFreeGeometrySlots.pop_back();
        return slot;
    }
    // The render thread reads mGeometry concurrently with the upload worker, so the vector
    // must never reallocate (it is reserved to geometryBudget). Refuse rather than grow.
    if (mGeometry.size() == mGeometry.capacity())
        return UINT32_MAX;
    mGeometry.emplace_back();
    return static_cast<uint32_t>(mGeometry.size()) - 1;
}

void GPUSceneImpl::FreeGeometry(uint32_t slot)
{
    CHECK(slot < mGeometry.size());
    GeometryResidency& g = mGeometry[slot];
    if (!g.live)
        return;
    if (g.dynamic)
    {
        // Dynamic geo lives in the host-coherent ring + its own AllowUpdate BLAS (single slot in
        // mBLASes) backed by per-geo buffers; release all three and drop it from the refit set.
        if (mDynamicPrimitiveAlloc)
            mDynamicPrimitiveAlloc->Free(g.resourceOffset);
        if (g.blasSlot < mBLASes.size())
        {
            mBLASes[g.blasSlot].Reset();
            mFreeBLASSlots.push_back(g.blasSlot);
        }
        g.dynBLASBuffer.Reset();
        g.dynScratchBuffer.Reset();
        for (size_t i = 0; i < mDynamicGeometrySlots.size(); ++i)
            if (mDynamicGeometrySlots[i] == slot)
            {
                mDynamicGeometrySlots[i] = mDynamicGeometrySlots.back();
                mDynamicGeometrySlots.pop_back();
                break;
            }
    }
    else
    {
        mPrimitiveAlloc->Free(g.resourceOffset);
        if (g.type == kGSInstanceTypeCurve)
        {
            mCurveAABBAlloc->Free(g.curve.aabbOffset);
            if (g.blasSlot < mCurveBLASes.size())
            {
                mCurveBLASes[g.blasSlot].Reset();
                mFreeCurveBLASSlots.push_back(g.blasSlot);
            }
        }
        else if (g.blasSlot < mBLASes.size())
        {
            mBLASes[g.blasSlot].Reset();
            mFreeBLASSlots.push_back(g.blasSlot);
        }
    }
    g.live = false;
    g.dynamic = false;
    g.state = ResourceState::Queued;
    ++g.generation; // invalidate outstanding handles to this slot
    mFreeGeometrySlots.push_back(slot);
}

void GPUSceneImpl::FreeTextureSlot(bool is3D, uint32_t slot)
{
    Vector<TextureSlot>& slots = TextureSlots(is3D);
    CHECK(slot < slots.size());
    TextureSlot& s = slots[slot];
    if (!s.live)
        return;
    TexturePool(is3D).Free(slot); // releases the bindless binding + owned resource
    s.live = false;
    ++s.generation; // invalidate outstanding handles to this slot
}

void GPUSceneImpl::Collect()
{
    // Collect runs against a quiescent scene: drain any outstanding uploads first so no
    // in-flight work still references geometry/textures we might otherwise free.
    Join();
    // Mark geometry referenced by the committed instance table; free the rest.
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

    // Mark 2D textures referenced by the committed material table (scene material textures
    // live in the 2D pool); free the rest, except pinned singletons (LUTs / defaults / env
    // map) and slots backing in-flight uploads. The 3D pool holds only pinned LUTs.
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
        TextureSlot const& s = mTexture2DSlots[slot];
        if (s.live && !s.pinned && !referencedTex[slot])
        {
            FreeTextureSlot(false, slot);
            ++freedTex;
        }
    }
    if (freedTex)
        LOG(GPUScene, LogDebug, "Collect freed {} unreferenced textures", freedTex);
}

uint32_t GPUSceneImpl::CountLiveInstances() const
{
    return static_cast<uint32_t>(owner.mCommittedInstances.size());
}

uint32_t GPUSceneImpl::CountTLASInstances() const
{
    uint32_t numAreaLights = 0;
    for (const auto& light : owner.mCommittedLights)
    {
        if (light.type == 3 || light.type == 4)
            numAreaLights++;
    }
    uint32_t numInstances = CountLiveInstances();
    CHECK_MSG(numInstances <= UINT32_MAX - numAreaLights,
              "TLAS instance count overflow: {} scene instances and {} area lights",
              numInstances, numAreaLights);
    return numInstances + numAreaLights;
}

void GPUSceneImpl::EnsureTLASCapacity(uint32_t totalInstances)
{
    if (totalInstances == 0)
        return;
    CHECK_MSG(totalInstances <= UINT32_MAX / mTLASInstanceStride,
              "TLAS instance byte count overflow: {} instances with stride {}",
              totalInstances, mTLASInstanceStride);
    RHIAccelerationStructureGeometryInstanceData instance{
        .instanceBuffer = mTLASInstances.mBuffer.Get(),
        .instanceOffset = 0,
        .totalPrimitives = totalInstances
    };
    RHIAccelerationStructureGeometryInfo geometry{
        .type = RHIAccelerationGeometryType::Instances,
        .instanceData = instance
    };
    RHIAccelerationStructureBuildRangeInfo range{.primitiveCount = totalInstances};
    RHIAccelerationStructureBuildDesc desc{
        .type = RHIAccelerationStructureType::TopLevel,
        .flags = RHIAccelerationStructureBuildFlagsBits::PreferFastTrace |
            RHIAccelerationStructureBuildFlagsBits::AllowUpdate |
            RHIAccelerationStructureBuildFlagsBits::AllowCompaction,
        .operation = RHIAccelerationStructureBuildOp::Build,
        .geometries = Span<const RHIAccelerationStructureGeometryInfo>{&geometry, 1},
        .ranges = Span<const RHIAccelerationStructureBuildRangeInfo>{&range, 1}
    };
    StackArena<4096> sizeInfoArena;
    AllocatorStack sizeInfoScratch(sizeInfoArena);
    auto* device = mDevice;
    auto size = device->GetAccelerationStructureSizeInfo(desc, sizeInfoScratch.Ptr());
    size_t requiredTLASSize = static_cast<size_t>(AlignUp(size.accelerationStructureSize, 256u));
    size_t requiredScratchSize = static_cast<size_t>(AlignUp(std::max(size.buildScratchSize, size.updateScratchSize), 256u));
    // The TLAS lives in the buffers pre-allocated at construction (tlasBudget / tlasScratchBudget).
    // We deliberately never grow them: the AS device object stays stable (so the render graph's
    // captured handle never goes stale and no renderer rebuild is ever needed). Exceeding the
    // budget is a hard configuration error - raise tlasBudget instead.
    CHECK_MSG(requiredTLASSize <= mTLASBuffer->mDesc.size && requiredScratchSize <= mScratchBufferTLAS->mDesc.size,
              "TLAS for {} instances needs {:.1f} MB (scratch {:.1f} MB) but the pre-allocated budget is "
              "{:.1f} MB (scratch {:.1f} MB). Increase GPUSceneDesc::tlasBudget / tlasScratchBudget.",
              totalInstances, requiredTLASSize / 1e6, requiredScratchSize / 1e6,
              mTLASBuffer->mDesc.size / 1e6, mScratchBufferTLAS->mDesc.size / 1e6);
}

GPUScene::TLASBuildResult GPUSceneImpl::BuildTLAS(RHICommandList* cmd, bool update)
{
    // The background drain publishes geometry residency (state/blasSlot) and texture residency
    // under mResidencyMutex. Hold it across the residency reads + instance writes below so a
    // concurrent drain can't flip a slot mid-build. The BLAS vectors are reserved up front, so
    // resident slot indices stay valid for the element reads here (we never read .size()).
    std::lock_guard<Mutex> residencyLock(mResidencyMutex);
    auto GeometryReady = [&](uint32_t resourceIndex) -> bool
    {
        if (resourceIndex >= mGeometry.size())
            return false;
        GeometryResidency const& g = mGeometry[resourceIndex];
        if (!g.live || g.state != ResourceState::Ready)
            return false;
        // Dynamic geo's BLAS is GPU-built by the first in-graph refit; until then its AS contents
        // are undefined, so skip it (it streams into the TLAS the frame after its initial build).
        if (g.dynamic && !g.dynBuilt)
            return false;
        // state == Ready is published only after blasSlot is assigned, so the slot is valid.
        return g.blasSlot != UINT32_MAX;
    };

    // Reserve capacity for the full instance set so streaming residency in (a growing
    // ready-instance count over frames) doesn't churn TLAS buffers.
    uint32_t capacityInstances = CountTLASInstances();
    EnsureTLASCapacity(capacityInstances);

    // Only instances whose geometry is Ready can be written this build; the rest are
    // skipped and will appear once their upload + BLAS completes (nonfatal).
    uint32_t areaLights = 0;
    for (GSLight const& light : owner.mCommittedLights)
        if (light.type == 3 || light.type == 4)
            ++areaLights;
    uint32_t readyInstances = 0;
    for (GSInstance const& inst : owner.mCommittedInstances)
        if (GeometryReady(inst.resourceIndex))
            ++readyInstances;
    uint32_t totalInstances = readyInstances + areaLights;
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
        float3 u = src->dpdu;
        float3 v = src->dpdv;
        if (src->type == 3) // Disk
        {
            u *= src->radius.x;
            v *= src->radius.y;
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
        res.blas = (src->type == 3) ? mDiskBLAS.Get() : mRectBLAS.Get();
        res.shaderBindingTableRecordOffset = (src->type == 3) ? kDiskLightSBTOffset : kRectLightSBTOffset;
        return res;
    };

    // An all-empty scene still builds a valid 0-instance TLAS so the always-present ray passes
    // can bind a traceable AS while geometry streams in; instances/lights are written only when
    // some are resident. Ready instances are written in committed order; the TLAS instanceID is
    // their compacted write position, mapped back to the committed index via mPickMap.
    owner.mPickMap.clear();
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
            GeometryResidency const* g = &mGeometry[inst.resourceIndex];
            auto data = ConvertInstance(inst, static_cast<uint32_t>(owner.mPickMap.size()));
            data.blas = (g->type == kGSInstanceTypeCurve) ? mCurveBLASes[g->blasSlot].Get() : mBLASes[g->blasSlot].Get();
            pInstances += mDevice->WriteAccelerationStructureInstanceData(data, pInstances);
            owner.mPickMap.push_back(i);
        }
        for (uint32_t i = 0; i < owner.mCommittedLights.size(); ++i)
        {
            GSLight const& light = owner.mCommittedLights[i];
            if (light.type == 3 || light.type == 4)
            {
                auto data = ConvertLight(&light, i);
                pInstances += mDevice->WriteAccelerationStructureInstanceData(data, pInstances);
            }
        }
    }
    RHIAccelerationStructureGeometryInstanceData instance{
        .instanceBuffer = mTLASInstances.mBuffer.Get(),
        .instanceOffset = instancesOffset,
        .totalPrimitives = totalInstances
    };
    RHIAccelerationStructureGeometryInfo geometry{
        .type = RHIAccelerationGeometryType::Instances,
        .instanceData = instance
    };
    RHIAccelerationStructureBuildRangeInfo range{.primitiveCount = totalInstances};
    RHIAccelerationStructureBuildFlags buildFlags = RHIAccelerationStructureBuildFlagsBits::PreferFastTrace |
        RHIAccelerationStructureBuildFlagsBits::AllowUpdate |
        RHIAccelerationStructureBuildFlagsBits::AllowCompaction;
    RHIAccelerationStructureBuildDesc desc{
        .type = RHIAccelerationStructureType::TopLevel,
        .flags = buildFlags,
        .operation = update ? RHIAccelerationStructureBuildOp::Update : RHIAccelerationStructureBuildOp::Build,
        .geometries = Span<const RHIAccelerationStructureGeometryInfo>{&geometry, 1},
        .ranges = Span<const RHIAccelerationStructureBuildRangeInfo>{&range, 1}
    };
    desc.scratchBuffer = mScratchBufferTLAS.Get();
    desc.scratchBufferOffset = 0;
    desc.dstAS = owner.mTLAS.Get();
    if (update)
        desc.srcAS = owner.mTLAS.Get();
    cmd->BuildAccelerationStructure({{{desc}}});
    return TLASBuildResult::Built;
}

GPUScene::Result GPUSceneImpl::UploadEnvMap(FTexture const& source)
{
    if (Result r = Upload(source, owner.mEnvMapIndex, "Environment Map", true); r != Result::Ready)
        return r;
    // Compute CDFs for importance sampling
    uint32_t width = source.GetWidth();
    uint32_t height = source.GetHeight();
    Span<const unsigned char> data = source.GetSubresource(0, 0);
    const float4* pixels = reinterpret_cast<const float4*>(data.data());
    
    Vector<float> f(width * height, mAllocator);
    for (uint32_t y = 0; y < height; ++y)
    {
        float v = (y + 0.5f) / height;
        float sinTheta = std::sin(pi<float>() * v);
        for (uint32_t x = 0; x < width; ++x)
        {
            float4 pixel = pixels[y * width + x];
            float luminance = max(pixel.x, max(pixel.y, pixel.z));
            // dw = dPhi dTheta sinTheta, account for solid angle (area)
            f[y * width + x] = luminance * sinTheta;
        }
    }
    
    PiecewiseConstant2D cdf(f, width, height, mAllocator);
    
    // Upload Marginal CDF as Texture2D
    FTexture marginalTex(mAllocator);
    marginalTex.Initialize(RHIResourceFormat::R32SignedFloat, RHITextureDimension::E2D,
                           cdf.mMarginal->mCDF.size(), 1);
    const size_t marginalSize = cdf.mMarginal->mCDF.size() * sizeof(float);
    marginalTex.bytes.resize(marginalSize);
    std::memcpy(marginalTex.bytes.data(), cdf.mMarginal->mCDF.data(), marginalSize);
    if (Result r = Upload(marginalTex, owner.mEnvMapMarginalCDFIndex, "Environment Map Marginal CDF", true);
        r != Result::Ready)
        return r;
    
    // Upload Conditional CDF as Texture2D
    FTexture conditionalTex(mAllocator);
    conditionalTex.Initialize(RHIResourceFormat::R32SignedFloat, RHITextureDimension::E2D,
                              cdf.mConditional[0]->mCDF.size(), height);
    
    size_t conditionalSize = height * cdf.mConditional[0]->mCDF.size() * sizeof(float);
    conditionalTex.bytes.resize(conditionalSize);
    
    for (uint32_t y = 0; y < height; ++y)
    {
        std::memcpy(conditionalTex.bytes.data() + y * cdf.mConditional[0]->mCDF.size() * sizeof(float),
                    cdf.mConditional[y]->mCDF.data(),
                    cdf.mConditional[y]->mCDF.size() * sizeof(float));
    }
    
    if (Result r = Upload(conditionalTex, owner.mEnvMapConditionalCDFIndex, "Environment Map Conditional CDF", true);
        r != Result::Ready)
        return r;

    LOG(GPUScene, LogInfo, "Environment map uploaded: {}x{}", source.GetWidth(), source.GetHeight());
    return Result::Ready;
}

static void CheckViewLUT(FTexture const& source, StringView name)
{
    CHECK_MSG(source.IsValid(), "{} view LUT is invalid", name);
    CHECK_MSG(source.GetDimension() == RHITextureDimension::E3D,
              "{} view LUT must be a 3D texture, got {}", name, static_cast<uint32_t>(source.GetDimension()));
    const RHIResourceFormat format = source.GetFormat();
    CHECK_MSG(format == RHIResourceFormat::A2B10G10R10Unorm ||
                  format == RHIResourceFormat::R16G16B16A16SignedFloat ||
                  format == RHIResourceFormat::R32G32B32A32SignedFloat,
              "{} view LUT must be RGB10A2, RGBA16F, or RGBA32F, got {}", name, format);
}

GPUScene::Result GPUSceneImpl::UploadViewLUTs(FTexture const& sdr, FTexture const& hdr)
{
    CheckViewLUT(sdr, "SDR");
    CheckViewLUT(hdr, "HDR");
    if (Result r = Upload(sdr, owner.mLUTViewSdrIndex, "View LUT SDR", true); r != Result::Ready)
        return r;
    return Upload(hdr, owner.mLUTViewHdrIndex, "View LUT HDR", true);
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
    return ResolvePoolTexture(const_cast<BindlessPool&>(mImpl->mTexture2DPool), mFoundationDefaultTexture2DFloatIndex.index);
}

void GPUSceneImpl::Reset()
{
    // Stop the upload worker and discard any queued / in-flight work before tearing down.
    {
        std::lock_guard<Mutex> lock(mWorkMutex);
        mStop = true;
    }
    mWorkCV.notify_all();
    if (mUploadThread.joinable())
        mUploadThread.join();
    PendingGeometryUpload g;
    while (mGeometryQueue.Pop(g)) {}
    PendingTextureUpload t;
    while (mTextureQueue.Pop(t)) {}
    PendingBufferUpload b;
    while (mBufferQueue.Pop(b)) {}
    mOutstanding.store(0, std::memory_order_release);
    mUploadFailed.store(false, std::memory_order_release);
    {
        std::lock_guard<Mutex> lock(mWorkMutex);
        mStop = false;
        mHasWork = false;
        mWorkerStarted = false;
    }
    mPrimitiveAlloc->Clear();
    mCurveAABBAlloc->Clear();
    mMeshletGlobalCounter = 0;
    owner.mLastTLASInstancesCount = 0;
    // Release dynamic-geo BLAS backing/scratch + return the ring to empty before clearing
    // residency (clearing the vector destroys the element handles too, but the ring allocator
    // must be explicitly emptied so its VMA block has no live suballocations).
    for (auto& g : mGeometry)
    {
        g.dynBLASBuffer.Reset();
        g.dynScratchBuffer.Reset();
    }
    if (mDynamicPrimitiveAlloc)
        mDynamicPrimitiveAlloc->Clear();
    mDynamicGeometrySlots.clear();
    mDynamicFrameSlot = 0;
    mDynamicUpdateOpen = false;
    mBLASes.clear();
    mBLASBuffers.clear();
    mFreeBLASSlots.clear();
    mCurveBLASes.clear();
    mCurveBLASBuffers.clear();
    mFreeCurveBLASSlots.clear();
    mGeometry.clear();
    mFreeGeometrySlots.clear();
    owner.mCommittedInstances.clear();
    owner.mCommittedLights.clear();
    owner.mCommittedMaterials.clear();
    owner.mPickMap.clear();
    mOpenTables = OpenTables{};
    mMaterialBuffer.Reset();
    mInstanceBuffer.Reset();
    mLightBuffer.Reset();
    mLightAliasTableBuffer.Reset();
    // NOTE: texture pools are append-only; old bindings become dead entries.
    //       mTLAS is kept alive and rebuilt in-place by BuildTLAS.
}

/* --- Facade forwarders to the implementation (GPUSceneImpl) --- */

GPUScene::Result GPUScene::Upload(FBlobDeserializer* blobs, FSerializedMesh const& source, GeometryHandle& outHandle)
{
    return mImpl->Upload(blobs, source, outHandle);
}

GPUScene::Result GPUScene::Upload(FBlobDeserializer* blobs, FSerializedCurve const& source, GeometryHandle& outHandle)
{
    return mImpl->Upload(blobs, source, outHandle);
}

GPUScene::Result GPUScene::UploadDynamic(FBlobDeserializer* blobs, FSerializedMesh const& source, GeometryHandle& outHandle)
{
    return mImpl->UploadDynamic(blobs, source, outHandle);
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
GPUScene::Result GPUScene::UploadViewLUTs(FTexture const& sdr, FTexture const& hdr) { return mImpl->UploadViewLUTs(sdr, hdr); }
GPUScene::Result GPUScene::UploadEnvMap(FTexture const& source) { return mImpl->UploadEnvMap(source); }

GPUScene::GPUSceneTables GPUScene::BeginScene(uint32_t instanceCount, uint32_t materialCount, uint32_t lightCount)
{
    return mImpl->BeginScene(instanceCount, materialCount, lightCount);
}

GPUScene::UpdateResult GPUScene::EndScene(GPUSceneTables& tables) { return mImpl->EndScene(tables); }
void GPUScene::DbgGetMemoryStatistics(Vector<MemoryStat>& outStats) const { mImpl->DbgGetMemoryStatistics(outStats); }
String GPUScene::DbgGetBufferStatistics() const { return mImpl->DbgGetBufferStatistics(); }
GPUScene::TLASBuildResult GPUScene::BuildTLAS(RHICommandList* cmd, bool update) { return mImpl->BuildTLAS(cmd, update); }
void GPUScene::Collect() { mImpl->Collect(); }
void GPUScene::Reset() { mImpl->Reset(); }

bool GPUScene::HasDynamicGeometry() const { return mImpl->HasDynamicGeometry(); }
void GPUScene::BeginDynamicGeometryUpdate() { mImpl->BeginDynamicGeometryUpdate(); }
Span<std::byte> GPUScene::UpdateDynamicGeometry(GeometryHandle handle) { return mImpl->UpdateDynamicGeometry(handle); }
void GPUScene::EndDynamicGeometryUpdate() { mImpl->EndDynamicGeometryUpdate(); }
void GPUScene::BuildBLAS(RHICommandList* cmd) { mImpl->BuildBLAS(cmd); }
void GPUScene::SetDynamicGeometryRebuildRate(uint32_t framesOrZero) { mImpl->mDynamicRebuildCadence = framesOrZero; }
uint32_t GPUScene::GetDynamicRefitCount() const { return mImpl->mLastRefitCount; }
uint32_t GPUScene::GetDynamicRebuildCount() const { return mImpl->mLastRebuildCount; }

RHIBuffer* GPUScene::GetDynamicPrimitiveBuffer() const
{
    // Fall back to the immutable primitive buffer when dynamic geometry is disabled so the
    // renderer's second-primitive-buffer binding is always valid (no instance selects it).
    return mImpl->mDynamicPrimitiveBuffer ? mImpl->mDynamicPrimitiveBuffer.Get() : mPrimitiveBuffer.Get();
}
RHIBuffer* GPUScene::GetInstanceBuffer() const { return mImpl->mInstanceBuffer.mBuffer.Get(); }
RHIBuffer* GPUScene::GetMaterialBuffer() const { return mImpl->mMaterialBuffer.mBuffer.Get(); }
RHIBuffer* GPUScene::GetLightBuffer() const { return mImpl->mLightBuffer.mBuffer.Get(); }
RHIBuffer* GPUScene::GetLightAliasTableBuffer() const { return mImpl->mLightAliasTableBuffer.mBuffer.Get(); }
BindlessPool* GPUScene::GetTexture2DPool() { return &mImpl->mTexture2DPool; }
BindlessPool* GPUScene::GetTexture3DPool() { return &mImpl->mTexture3DPool; }
uint32_t GPUScene::GetLightCapacity() const { return mImpl->mLightBuffer.Capacity(); }