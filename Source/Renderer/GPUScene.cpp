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
#include <Core/Paths.hpp>
#include <Core/ThreadPool.hpp>
#include <Math/Quantize.hpp>
#include <algorithm>
#include <atomic>
#include <limits>
#include <thread>

// Per-resource staging footprint slack, mirrored from the previous Editor scheduler.
static constexpr size_t kUploadBudgetSlack = 1ull * (1ull << 20);
static constexpr size_t kUploadStagingBudgetSlack = 32ull * (1ull << 20);
static constexpr size_t kUploadStagingBuffers = 3u;

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
static void GPUSceneCompleteJob(std::atomic<size_t>* counter)
{
    if (!counter)
        return;
    size_t const previous = counter->fetch_sub(1, std::memory_order_release);
    CHECK_MSG(previous > 0, "GPUScene upload job counter underflow");
    if (previous == 1)
        counter->notify_all();
}

static void GPUSceneWaitJobs(std::atomic<size_t>* counter)
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
struct GPUSceneBlobDecodeJob final : Foundation::Core::ThreadPoolJob
{
    GPUSceneBlobWrite write{};
    FBlobDeserializer blobs{Span<const unsigned char>{}};
    Span<Arena> scratchArenas{};
    Span<AllocatorStack> scratchAllocators{};
    std::atomic<size_t>* counter{nullptr};

    GPUSceneBlobDecodeJob(GPUSceneBlobWrite const& write, FBlobDeserializer const& blobs,
                          Span<Arena> scratchArenas, Span<AllocatorStack> scratchAllocators,
                          std::atomic<size_t>* counter) :
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

void GPUScene::FlushDirectGeometryUpload()
{
    if (!mDirectGeometryUpload)
        return;
    if (mPrimitiveAlloc->GetHighWaterMark())
        mPrimitiveBuffer->Flush(0, mPrimitiveAlloc->GetHighWaterMark());
    if (mCurveAABBAlloc->GetHighWaterMark())
        mCurveAABBBuffer->Flush(0, mCurveAABBAlloc->GetHighWaterMark());
}

GPUScene::GPUScene(RHIDevice* device, Allocator* allocator, GPUSceneDesc const& desc,
                   AllocatorStack* frameScratch) :
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
    mCommittedInstances(allocator),
    mCommittedLights(allocator),
    mCommittedMaterials(allocator),
    mPickMap(allocator),
    mPendingGeometry(allocator),
    mPendingTextures(allocator),
    mPendingBuffers(allocator),
    mInstanceScratch(allocator),
    mTLASInstanceStride(mDevice->WriteAccelerationStructureInstanceData({}, nullptr)),
    mTLASInstances(device, desc.tlasInstanceBudget * mTLASInstanceStride)
{
    CHECK(mDevice != nullptr);
    CHECK(mAllocator != nullptr);
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
    mPrimitiveBuffer = mDevice->CreateBuffer(
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
        mPrimitiveMapped = mPrimitiveBuffer->Map<char>();
        mCurveAABBMapped = mCurveAABBBuffer->Map<char>();
        LOG(GPUScene, LogInfo, "Direct GPU Memory Access available ({} MiB budget used). Uploading via direct copy.",
            directGeometryBudget / (1u << 20));
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
    mTLAS = mDevice->CreateAccelerationStructure(tlasDesc);
    
    mSobolMatricesBuffer = mDevice->CreateBuffer(
    {
        .resource = {
            .heap = RHIDeviceHeapType::Local,
            .shared = false
        },
        .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination,
        .size = sizeof(kSobolMatrices32)
    });
    mSobolMatricesBuffer->DebugSetObjectName("Sobol Matrices");

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
        mFoundationDefaultBufferFloat = mDevice->CreateBuffer({
            .resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
            .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination,
            .size = foundationDefaultBufferFloatSize
        });
        // Textures (incl. view LUTs) go through the unified upload queue.
        // Pinned: GPUScene-owned singletons that Collect must never reclaim.
        Upload(lutE, mLUTGGXEIndex, nullptr, true);
        Upload(lutEavg, mLUTGGXEavgIndex, nullptr, true);
        Upload(lutEIOR, mLUTGGXEIORIndex, nullptr, true);
        Upload(lutEIORavg, mLUTGGXEIORavgIndex, nullptr, true);
        Upload(lutEIORInv, mLUTGGXEIORInvIndex, nullptr, true);
        Upload(lutEIORInvavg, mLUTGGXEIORInvavgIndex, nullptr, true);
        Upload(sheenLtc, mLUTSheenLTCIndex, nullptr, true);
        Upload(foundationDefaultTexture2D, mFoundationDefaultTexture2DIndex, "_FoundationDefaultTexture2D", true);
        Upload(foundationDefaultTexture2DFloat, mFoundationDefaultTexture2DFloatIndex,
               "_FoundationDefaultTexture2DFloat", true);
        UploadViewLUTs(defaultViewLutSdr, defaultViewLutHdr);

        // Plain device-local buffers ride the same upload queue.
        const float foundationDefaultBufferFloat = 1.0f;
        Upload(mFoundationDefaultBufferFloat.Get(),
               Span<const unsigned char>(reinterpret_cast<const unsigned char*>(&foundationDefaultBufferFloat),
                                         foundationDefaultBufferFloatSize));
        Upload(mSobolMatricesBuffer.Get(),
               Span<const unsigned char>(reinterpret_cast<const unsigned char*>(kSobolMatrices32),
                                         sizeof(kSobolMatrices32)));
        Join();
    }
}

GPUScene::~GPUScene()
{
    if (mUploadThread.joinable())
        mUploadThread.join();
}

Pair<GSInstance*, uint32_t> GPUScene::AllocateInstance(uint32_t count)
{
    return mInstanceBuffer.Allocate(count);
}

Pair<GSMaterial*, uint32_t> GPUScene::AllocateMaterial(uint32_t count)
{
    return mMaterialBuffer.Allocate(count);
}

Pair<GSLight*, uint32_t> GPUScene::AllocateLight(uint32_t count)
{
    return mLightBuffer.Allocate(count);
}

Pair<Alias*, uint32_t> GPUScene::AllocateLightAliasTable(uint32_t count)
{
    return mLightAliasTableBuffer.Allocate(count);
}

GPUScene::GPUSceneTables GPUScene::BeginScene(uint32_t instanceCount, uint32_t materialCount, uint32_t lightCount)
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

GPUScene::UpdateResult GPUScene::EndScene(GPUSceneTables& tables)
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
    mCommittedInstances.resize(tables.instances.size());
    for (size_t i = 0; i < tables.instances.size(); ++i)
    {
        InstanceDesc const& desc = tables.instances[i];
        GeometryResidency const* g = ResolveGeometry(desc.geometry);
        CHECK_MSG(g, "EndScene instance references invalid geometry (index {}, generation {})",
                  desc.geometry.index, desc.geometry.generation);
        GSInstance inst{
            .transform = desc.transform,
            .rotation = desc.rotation,
            .scale = desc.scale,
            .resourceOffset = g->resourceOffset,
            .materialIndex = desc.materialIndex,
            .resourceIndex = desc.geometry.index,
            .type = g->type,
        };
        mOpenTables.instancePtr[i] = inst; // ring (GPU)
        mCommittedInstances[i] = inst;     // committed snapshot
    }

    mCommittedMaterials.assign(tables.materials.begin(), tables.materials.end());
    mCommittedLights.assign(tables.lights.begin(), tables.lights.end());

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
        std::memcpy(mOpenTables.aliasPtr, table.mBins.data(), table.mBins.size() * sizeof(Alias));
        res.firstLight = tables.firstLight;
        res.firstLightAliasTable = mOpenTables.firstAliasTable;
        res.numLights = static_cast<uint32_t>(tables.lights.size());
        res.sceneLightWeightSum = weightSum;
    }
    mOpenTables = OpenTables{};
    return res;
}

void GPUScene::FillGlobals(UBO& globals, bool hdr) const
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

void GPUScene::DbgGetMemoryStatistics(Vector<MemoryStat>& outStats) const
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

    size_t primitiveBytes = AddBufferSize(mPrimitiveBuffer);
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
    size_t sobolBytes = AddBufferSize(mSobolMatricesBuffer);
    size_t defaultBufferBytes = AddBufferSize(mFoundationDefaultBufferFloat);

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

String GPUScene::DbgGetBufferStatistics() const
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
                   mPrimitiveBuffer->GetAllocationSize() / static_cast<float>(1 << 20u),
                   mPrimitiveAlloc->GetUsedBytes() / static_cast<float>(1 << 20u),
                   mPrimitiveBuffer->mDesc.size / static_cast<float>(1 << 20u));
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

GPUScene::Result GPUScene::ReserveMesh(FSerializedMesh const& src, GSMesh& outData, uint32_t& outOffset)
{
    if (src.lods.empty())
        return Result::InvalidInput;
    auto const& lod0 = src.lods[0];
    const size_t size = CalculateMeshPrimitiveSize(src);
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

size_t GPUScene::StageMesh(ImmediateUpload* ctx, FSerializedMesh const& src, GSMesh const& header,
                           uint32_t offset, Vector<GPUSceneBlobWrite>& outWrites)
{
    const size_t size = CalculateMeshPrimitiveSize(src);
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
        ptr = ctx->Upload(mPrimitiveBuffer.Get(), size, offset);
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

GPUScene::Result GPUScene::ReserveCurve(FSerializedCurve const& src, GSCurveSet& outData, uint32_t& outOffset)
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
    const size_t size = CalculateCurvePrimitiveSize(src);
    constexpr size_t kAlign = 4;
    uint64_t base = mPrimitiveAlloc->Allocate(size, kAlign);
    if (base == RHIVirtualAllocator::kInvalidOffset)
    {
        LOG(GPUScene, LogError, "Primitive buffer overflow for serialized curve. Need {} bytes, {} used of {}",
            size, mPrimitiveAlloc->GetUsedBytes(), mPrimitiveAlloc->GetCapacity());
        return Result::OutOfMemory;
    }
    outOffset = static_cast<uint32_t>(base);

    const size_t aabbSize = CalculateCurveAABBSize(src);
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

size_t GPUScene::StageCurve(ImmediateUpload* ctx, FSerializedCurve const& src, GSCurveSet const& header,
                            uint32_t offset, Vector<GPUSceneBlobWrite>& outWrites)
{
    const size_t size = CalculateCurvePrimitiveSize(src);
    const size_t aabbSize = CalculateCurveAABBSize(src);
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
        ptr = ctx->Upload(mPrimitiveBuffer.Get(), size, offset);
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

BindlessPool& GPUScene::SelectTexturePool(RHITextureDimension viewDimension)
{
    if (IsTexture3DView(viewDimension))
        return mTexture3DPool;
    CHECK_MSG(viewDimension != RHITextureDimension::E1D && viewDimension != RHITextureDimension::E1DArray,
              "Unsupported bindless texture view dimension {}", static_cast<uint32_t>(viewDimension));
    return mTexture2DPool;
}

BindlessPool const& GPUScene::SelectTexturePool(RHITextureDimension viewDimension) const
{
    return const_cast<GPUScene*>(this)->SelectTexturePool(viewDimension);
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

size_t GPUScene::StageTextureSubresource(ImmediateUpload* ctx, FSerializedTexture const& source, RHITexture* texture,
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

GPUScene::Result GPUScene::Upload(FBlobDeserializer* blobs, FSerializedMesh const& source, GeometryHandle& outHandle)
{
    CHECK(blobs != nullptr);
    GSMesh header{};
    uint32_t offset = 0;
    Result r = ReserveMesh(source, header, offset);
    if (r != Result::InProgress)
        return r;
    uint32_t slot = AcquireGeometrySlot();
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
    mPendingGeometry.push_back({.handle = outHandle, .blobs = *blobs, .mesh = &source, .curve = nullptr,
                                .footprint = mDirectGeometryUpload ? 1 : CalculateMeshPrimitiveSize(source)});
    return Result::InProgress;
}

GPUScene::Result GPUScene::Upload(FBlobDeserializer* blobs, FSerializedCurve const& source, GeometryHandle& outHandle)
{
    CHECK(blobs != nullptr);
    GSCurveSet header{};
    uint32_t offset = 0;
    Result r = ReserveCurve(source, header, offset);
    if (r != Result::InProgress)
        return r;
    uint32_t slot = AcquireGeometrySlot();
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
    const size_t footprint = CalculateCurvePrimitiveSize(source) + CalculateCurveAABBSize(source);
    mPendingGeometry.push_back({.handle = outHandle, .blobs = *blobs, .mesh = nullptr, .curve = &source,
                                .footprint = mDirectGeometryUpload ? 1 : footprint});
    return Result::InProgress;
}

GPUScene::Result GPUScene::Upload(FBlobDeserializer* blobs, FSerializedTexture const& source,
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
    BindlessPool& pool = TexturePool(is3D);
    Vector<TextureSlot>& slots = TextureSlots(is3D);
    if (!outTextureIndex.IsValid())
    {
        uint32_t slot = pool.Allocate(std::move(texture), std::move(view));
        if (slot >= slots.size())
            slots.resize(slot + 1);
        slots[slot].live = true;
        slots[slot].pinned = pinned;
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
        pool.Update(outTextureIndex.index, std::move(texture), std::move(view));
    }
    mPendingTextures.push_back({.index = outTextureIndex.index, .blobs = *blobs, .source = &source});
    return Result::InProgress;
}

GPUScene::Result GPUScene::Upload(FTexture const& source, TextureHandle& outTextureIndex, const char* debugName,
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

GPUScene::Result GPUScene::Upload(RHIBuffer* dst, Span<const unsigned char> data, uint32_t dstOffset)
{
    if (!dst || data.empty())
        return Result::InvalidInput;
    PendingBufferUpload pending{dst, dstOffset, Vector<unsigned char>(mAllocator)};
    pending.data.assign(data.begin(), data.end());
    mPendingBuffers.push_back(std::move(pending));
    return Result::InProgress;
}

GPUScene::Result GPUScene::Query(GeometryHandle handle) const
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

GPUScene::Result GPUScene::Query(TextureHandle texture) const
{
    Vector<TextureSlot> const& slots = TextureSlots(texture.is3D);
    if (!texture.IsValid() || texture.index >= slots.size() || !slots[texture.index].live ||
        slots[texture.index].generation != texture.generation)
        return Result::InvalidHandle;
    for (auto const& p : mPendingTextures)
        if (p.index == texture.index &&
            IsTexture3DView(static_cast<FTextureHeader const&>(*p.source).GetViewDimension()) == texture.is3D)
            return Result::InProgress;
    return Result::Ready;
}

void GPUScene::Join()
{
    if (mUploadThread.joinable())
    {
        mUploadThread.join();
        return;
    }
    if (mPendingGeometry.empty() && mPendingTextures.empty() && mPendingBuffers.empty())
        return;
    ProcessPendingUploads();
}

void GPUScene::StartUploads()
{
    CHECK_MSG(!mUploadThread.joinable(), "StartUploads called while a drain is already in flight");
    if (mPendingGeometry.empty() && mPendingTextures.empty() && mPendingBuffers.empty())
        return;
    mUploadFailed.store(false, std::memory_order_release);
    mUploadsDone.store(false, std::memory_order_release);
    mUploadThread = std::thread([this]
    {
        try
        {
            ProcessPendingUploads();
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
        mUploadsDone.store(true, std::memory_order_release);
    });
}

GPUScene::Result GPUScene::Poll()
{
    if (!mUploadThread.joinable())
    {
        // Lazily kick the background drain the first time we're polled with queued work.
        if (mPendingGeometry.empty() && mPendingTextures.empty() && mPendingBuffers.empty())
            return Result::Ready;
        StartUploads();
        return Result::InProgress;
    }
    if (!mUploadsDone.load(std::memory_order_acquire))
        return Result::InProgress;
    mUploadThread.join(); // reap the finished worker
    return mUploadFailed.load(std::memory_order_acquire) ? Result::SubmitFailed : Result::Ready;
}

void GPUScene::ProcessPendingUploads()
{
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
    ThreadPool pool(workerCount, ThreadPool::getTaskSize(std::max<size_t>(taskUpper, 1u) + 2u), mAllocator,
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
    std::atomic<size_t> pendingJobs{0};
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
            g->state = ResourceState::Uploading;
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
        Vector<uint32_t> meshSlots(meshHeaders.size(), mAllocator);
        for (size_t i = 0; i < meshHeaders.size(); i += kBLASBuildBatch)
        {
            size_t bs = std::min(kBLASBuildBatch, meshHeaders.size() - i);
            BuildBLAS(&blasCtx, Span<const GSMesh>(meshHeaders.data() + i, bs), Span<uint32_t>(meshSlots.data() + i, bs),
                      usedFirst ? ImmediateSubmitDesc{} : firstSubmit);
            usedFirst = true;
        }
        Vector<uint32_t> curveSlots(curveHeaders.size(), mAllocator);
        for (size_t i = 0; i < curveHeaders.size(); i += kBLASBuildBatch)
        {
            size_t bs = std::min(kBLASBuildBatch, curveHeaders.size() - i);
            BuildCurveBLAS(&blasCtx, Span<const GSCurveSet>(curveHeaders.data() + i, bs),
                           Span<uint32_t>(curveSlots.data() + i, bs), usedFirst ? ImmediateSubmitDesc{} : firstSubmit);
            usedFirst = true;
        }
        for (size_t i = 0; i < meshHandles.size(); ++i)
            ResolveGeometry(meshHandles[i])->blasSlot = meshSlots[i];
        for (size_t i = 0; i < curveHandles.size(); ++i)
            ResolveGeometry(curveHandles[i])->blasSlot = curveSlots[i];
    }
    for (auto const& p : mPendingGeometry)
        if (GeometryResidency* g = ResolveGeometry(p.handle))
            g->state = ResourceState::Ready;
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
    mPendingTextures.clear();
    mPendingBuffers.clear();

    // Block until the last submitted transfer completes. The texture submit (if any)
    // chains after the geometry+buffer submit on the Transfer queue, so waiting the
    // highest signaled value covers everything.
    RHIDeviceQueue::TimelinePair finalWait{uploadTimeline.Get(), hadTextures ? kTextureReady : kGeometryReady};
    mDevice->WaitForTimelineSemaphores(Span<const RHIDeviceQueue::TimelinePair>(&finalWait, 1), -1);

    pool.Join();
}

// Reference:
// - https://github.com/zeux/niagara/blob/master/src/scenert.cpp
// - https://docs.vulkan.org/tutorial/latest/courses/18_Ray_tracing/02_Acceleration_structures.html
// - https://docs.vulkan.org/tutorial/latest/courses/18_Ray_tracing/04_TLAS_animation.html
void GPUScene::BuildBLAS(ImmediateContext* ctx, Span<const GSMesh> meshes, Span<uint32_t> outBLASIndices,
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
    auto* primitiveBuffer = mPrimitiveBuffer.Get();
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

void GPUScene::BuildCurveBLAS(ImmediateContext* ctx, Span<const GSCurveSet> curves, Span<uint32_t> outBLASIndices,
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

uint32_t GPUScene::AcquireMeshBLASSlot()
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

uint32_t GPUScene::AcquireCurveBLASSlot()
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

GPUScene::GeometryResidency* GPUScene::ResolveGeometry(GeometryHandle handle)
{
    if (!handle.IsValid() || handle.index >= mGeometry.size())
        return nullptr;
    GeometryResidency& g = mGeometry[handle.index];
    if (!g.live || g.generation != handle.generation)
        return nullptr;
    return &g;
}

GPUScene::GeometryResidency const* GPUScene::ResolveGeometry(GeometryHandle handle) const
{
    return const_cast<GPUScene*>(this)->ResolveGeometry(handle);
}

uint32_t GPUScene::AcquireGeometrySlot()
{
    if (!mFreeGeometrySlots.empty())
    {
        uint32_t slot = mFreeGeometrySlots.back();
        mFreeGeometrySlots.pop_back();
        return slot;
    }
    mGeometry.emplace_back();
    return static_cast<uint32_t>(mGeometry.size()) - 1;
}

void GPUScene::FreeGeometry(uint32_t slot)
{
    CHECK(slot < mGeometry.size());
    GeometryResidency& g = mGeometry[slot];
    if (!g.live)
        return;
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
    g.live = false;
    g.state = ResourceState::Queued;
    ++g.generation; // invalidate outstanding handles to this slot
    mFreeGeometrySlots.push_back(slot);
}

void GPUScene::FreeTextureSlot(bool is3D, uint32_t slot)
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

void GPUScene::Collect()
{
    // Mark geometry referenced by the committed instance table; free the rest. Pending
    // uploads keep their geometry live (handles are referenced by mPendingGeometry).
    Vector<uint8_t> referenced(mGeometry.size(), 0u, mAllocator);
    for (GSInstance const& inst : mCommittedInstances)
        if (inst.resourceIndex < referenced.size())
            referenced[inst.resourceIndex] = 1u;
    for (auto const& p : mPendingGeometry)
        if (p.handle.index < referenced.size())
            referenced[p.handle.index] = 1u;

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
    for (GSMaterial const& m : mCommittedMaterials)
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
    for (auto const& p : mPendingTextures)
        if (!IsTexture3DView(static_cast<FTextureHeader const&>(*p.source).GetViewDimension()))
            MarkTexture(p.index);

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

GSInstance GPUScene::GetInstance(uint32_t index) const
{
    CHECK_MSG(index < mCommittedInstances.size(), "GetInstance index {} out of range ({})", index,
              mCommittedInstances.size());
    return mCommittedInstances[index];
}

uint32_t GPUScene::ResolvePickedInstance(uint32_t pickID) const
{
    // pickID is a TLAS instanceID = index into the last build's written (ready) set.
    if (pickID >= mPickMap.size())
        return UINT32_MAX;
    return mPickMap[pickID];
}

GSLight GPUScene::GetLight(uint32_t index) const
{
    CHECK_MSG(index < mCommittedLights.size(), "GetLight index {} out of range ({})", index, mCommittedLights.size());
    return mCommittedLights[index];
}

GSMaterial GPUScene::GetMaterial(uint32_t index) const
{
    CHECK_MSG(index < mCommittedMaterials.size(), "GetMaterial index {} out of range ({})", index,
              mCommittedMaterials.size());
    return mCommittedMaterials[index];
}

uint32_t GPUScene::CountLiveInstances() const
{
    return static_cast<uint32_t>(mCommittedInstances.size());
}

uint32_t GPUScene::CountTLASInstances() const
{
    uint32_t numAreaLights = 0;
    for (const auto& light : mCommittedLights)
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

bool GPUScene::EnsureTLASCapacity(uint32_t totalInstances, bool allowRecreate)
{
    if (totalInstances == 0)
        return true;
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
    bool hasCapacity = requiredTLASSize <= mTLASBuffer->mDesc.size &&
        requiredScratchSize <= mScratchBufferTLAS->mDesc.size;
    if (hasCapacity)
        return true;
    if (!allowRecreate)
        return false;

    requiredTLASSize = static_cast<size_t>(AlignUp(std::max(requiredTLASSize, size_t(mTLASBuffer->mDesc.size) * 2u), size_t(256)));
    requiredScratchSize = static_cast<size_t>(AlignUp(std::max(requiredScratchSize, size_t(mScratchBufferTLAS->mDesc.size) * 2u), size_t(256)));
    CHECK_MSG(requiredTLASSize <= UINT32_MAX && requiredScratchSize <= UINT32_MAX,
              "TLAS resize exceeds RHI uint32 size range: TLAS {} scratch {}",
              requiredTLASSize, requiredScratchSize);

    auto newTLASBuffer = device->CreateBuffer({
        .resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
        .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress |
            RHIBufferUsageBits::AccelerationStructureStorage,
        .size = static_cast<uint32_t>(requiredTLASSize)
    });
    auto newScratchBuffer = device->CreateBuffer({
        .resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
        .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress,
        .size = static_cast<uint32_t>(requiredScratchSize),
        .alignment = 256
    });
    auto newTLAS = device->CreateAccelerationStructure({
        .type = RHIAccelerationStructureType::TopLevel,
        .flags = desc.flags,
        .buffer = newTLASBuffer.Get(),
        .size = static_cast<uint32_t>(requiredTLASSize)
    });
    mTLAS.Reset();
    mTLASBuffer.Reset();
    mScratchBufferTLAS.Reset();
    mTLASBuffer = std::move(newTLASBuffer);
    mScratchBufferTLAS = std::move(newScratchBuffer);
    mTLAS = std::move(newTLAS);
    LOG(GPUScene, LogInfo, "Resized TLAS buffers: TLAS {:.1f} MB, scratch {:.1f} MB",
        requiredTLASSize / static_cast<float>(1 << 20u),
        requiredScratchSize / static_cast<float>(1 << 20u));
    return true;
}

GPUScene::TLASBuildResult GPUScene::BuildTLAS(RHICommandList* cmd, bool update)
{
    auto GeometryReady = [&](uint32_t resourceIndex) -> bool
    {
        if (resourceIndex >= mGeometry.size())
            return false;
        GeometryResidency const& g = mGeometry[resourceIndex];
        if (!g.live || g.state != ResourceState::Ready)
            return false;
        return g.type == kGSInstanceTypeCurve ? g.blasSlot < mCurveBLASes.size() : g.blasSlot < mBLASes.size();
    };

    // Reserve capacity for the full instance set so streaming residency in (a growing
    // ready-instance count over frames) doesn't churn TLAS buffers.
    uint32_t capacityInstances = CountTLASInstances();
    if (capacityInstances != 0 && !EnsureTLASCapacity(capacityInstances, !update))
        return TLASBuildResult::NeedsRendererRebuild;

    // Only instances whose geometry is Ready can be written this build; the rest are
    // skipped and will appear once their upload + BLAS completes (nonfatal).
    uint32_t areaLights = 0;
    for (GSLight const& light : mCommittedLights)
        if (light.type == 3 || light.type == 4)
            ++areaLights;
    uint32_t readyInstances = 0;
    for (GSInstance const& inst : mCommittedInstances)
        if (GeometryReady(inst.resourceIndex))
            ++readyInstances;
    uint32_t totalInstances = readyInstances + areaLights;
    if (totalInstances == 0)
    {
        mLastTLASInstancesCount = 0;
        mPickMap.clear();
        return TLASBuildResult::Empty;
    }
    if (totalInstances != mLastTLASInstancesCount)
    {
        update = false;
        mLastTLASInstancesCount = totalInstances;
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

    auto [pInstances, instancesOffset] = mTLASInstances.Allocate(mTLASInstanceStride * totalInstances);
    // Ready instances are written in committed order; the TLAS instanceID is their
    // compacted write position, mapped back to the committed index via mPickMap.
    mPickMap.clear();
    for (uint32_t i = 0; i < mCommittedInstances.size(); ++i)
    {
        GSInstance const& inst = mCommittedInstances[i];
        if (!GeometryReady(inst.resourceIndex))
            continue;
        GeometryResidency const* g = &mGeometry[inst.resourceIndex];
        auto data = ConvertInstance(inst, static_cast<uint32_t>(mPickMap.size()));
        data.blas = (g->type == kGSInstanceTypeCurve) ? mCurveBLASes[g->blasSlot].Get() : mBLASes[g->blasSlot].Get();
        pInstances += mDevice->WriteAccelerationStructureInstanceData(data, pInstances);
        mPickMap.push_back(i);
    }
    for (uint32_t i = 0; i < mCommittedLights.size(); ++i)
    {
        GSLight const& light = mCommittedLights[i];
        if (light.type == 3 || light.type == 4)
        {
            auto data = ConvertLight(&light, i);
            pInstances += mDevice->WriteAccelerationStructureInstanceData(data, pInstances);
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
    desc.dstAS = mTLAS.Get();
    if (update)
        desc.srcAS = mTLAS.Get();
    cmd->BuildAccelerationStructure({{{desc}}});
    return TLASBuildResult::Built;
}

GPUScene::Result GPUScene::UploadEnvMap(FTexture const& source)
{
    if (Result r = Upload(source, mEnvMapIndex, "Environment Map", true); r != Result::Ready)
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
    if (Result r = Upload(marginalTex, mEnvMapMarginalCDFIndex, "Environment Map Marginal CDF", true);
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
    
    if (Result r = Upload(conditionalTex, mEnvMapConditionalCDFIndex, "Environment Map Conditional CDF", true);
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

GPUScene::Result GPUScene::UploadViewLUTs(FTexture const& sdr, FTexture const& hdr)
{
    CheckViewLUT(sdr, "SDR");
    CheckViewLUT(hdr, "HDR");
    if (Result r = Upload(sdr, mLUTViewSdrIndex, "View LUT SDR", true); r != Result::Ready)
        return r;
    return Upload(hdr, mLUTViewHdrIndex, "View LUT HDR", true);
}

static RHITexture* ResolvePoolTexture(BindlessPool& pool, uint32_t index)
{
    if (index == UINT32_MAX)
        return nullptr;
    return pool.GetResource(index);
}

RHITexture* GPUScene::GetFoundationDefaultTexture2D() const
{
    return ResolvePoolTexture(const_cast<BindlessPool&>(mTexture2DPool), mFoundationDefaultTexture2DIndex.index);
}

RHITexture* GPUScene::GetFoundationDefaultTexture2DFloat() const
{
    return ResolvePoolTexture(const_cast<BindlessPool&>(mTexture2DPool), mFoundationDefaultTexture2DFloatIndex.index);
}

RHIBuffer* GPUScene::GetSobolMatricesBuffer() const
{
    return mSobolMatricesBuffer.Get();
}

void GPUScene::Reset()
{
    if (mUploadThread.joinable())
        mUploadThread.join();
    mPrimitiveAlloc->Clear();
    mCurveAABBAlloc->Clear();
    mMeshletGlobalCounter = 0;
    mLastTLASInstancesCount = 0;
    mBLASes.clear();
    mBLASBuffers.clear();
    mFreeBLASSlots.clear();
    mCurveBLASes.clear();
    mCurveBLASBuffers.clear();
    mFreeCurveBLASSlots.clear();
    mGeometry.clear();
    mFreeGeometrySlots.clear();
    mPendingGeometry.clear();
    mPendingTextures.clear();
    mPendingBuffers.clear();
    mCommittedInstances.clear();
    mCommittedLights.clear();
    mCommittedMaterials.clear();
    mPickMap.clear();
    mOpenTables = OpenTables{};
    mMaterialBuffer.Reset();
    mInstanceBuffer.Reset();
    mLightBuffer.Reset();
    mLightAliasTableBuffer.Reset();
    // NOTE: texture pools are append-only; old bindings become dead entries.
    //       mTLAS is kept alive and rebuilt in-place by BuildTLAS.
}