#include "GPUScene.hpp"
#include "../Render/Precompute.hpp"
#include "../Render/Tables.hpp"
#include "../Render/ViewLUTs.hpp"
#include <Core/AllocatorStack.hpp>
#include <Core/Paths.hpp>
#include <Math/Quantize.hpp>
#include <algorithm>
#include <limits>
#include "Scene.hpp"

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

static constexpr uint32_t kGPUSceneRingFrameSlack = 3u;
static constexpr size_t kMinDirectGeometryUploadHeapSize = 512ull * (1ull << 20);
static constexpr uint32_t kGPUScenePersistentTexture2DBindings = 1u; // GGX LUT.
static constexpr uint32_t kGPUScenePersistentTexture3DBindings = 2u; // default SDR/HDR view LUTs.
static constexpr uint32_t kGPUSceneDefaultTextureBindings = 2u; // _FoundationDefault Texture2D + Texture2DFloat.
static constexpr uint32_t kGPUSceneEnvMapBindings = 3u; // Env map + marginal/conditional CDF textures.
static constexpr uint32_t kGPUSceneTextureBindingSlack = 8u;
static constexpr size_t kGPUSceneByteBudgetSlack = 64u << 10u;

static uint32_t CountBudget(size_t count)
{
    count = std::max<size_t>(count, 1u);
    CHECK_MSG(count <= UINT32_MAX, "GPUScene count budget {} exceeds uint32_t range", count);
    return static_cast<uint32_t>(count);
}

static uint32_t RingBudget(size_t count)
{
    return CountBudget(count * kGPUSceneRingFrameSlack);
}

static uint32_t ByteBudget(size_t bytes, size_t minBytes, size_t alignment, size_t maxBytes = UINT32_MAX)
{
    if (bytes != 0)
        bytes += kGPUSceneByteBudgetSlack;
    size_t budget = AlignUp(std::max(bytes, minBytes), alignment);
    CHECK_MSG(budget <= maxBytes, "GPUScene byte budget {} exceeds maximum {}", budget, maxBytes);
    CHECK_MSG(budget <= UINT32_MAX, "GPUScene byte budget {} exceeds uint32_t range", budget);
    return static_cast<uint32_t>(budget);
}

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

GPUScene::GPUSceneDesc GPUScene::CalculateSceneBudget(FScene const& scene, RHIDeviceCapabilities const& caps)
{
    GPUSceneDesc desc{};
    size_t primitiveBytes = 0;
    for (auto const& mesh : scene.GetMeshes())
    {
        primitiveBytes = AlignUp(primitiveBytes, size_t(4));
        primitiveBytes += CalculateMeshPrimitiveSize(mesh);
    }
    size_t curveAABBBytes = 0;
    for (auto const& curve : scene.GetCurves())
    {
        primitiveBytes = AlignUp(primitiveBytes, size_t(4));
        primitiveBytes += CalculateCurvePrimitiveSize(curve);

        curveAABBBytes = AlignUp(curveAABBBytes, alignof(RHIAccelerationStructureAABB));
        curveAABBBytes += CalculateCurveAABBSize(curve);
    }

    const size_t maxStorageBufferRange = std::min<size_t>(caps.maxStorageBufferRange, UINT32_MAX);
    desc.primitiveBudget = ByteBudget(primitiveBytes, desc.primitiveBudget, size_t(4), maxStorageBufferRange);
    desc.curveAABBBudget = ByteBudget(curveAABBBytes, desc.curveAABBBudget, alignof(RHIAccelerationStructureAABB));

    size_t areaLightCount = 0;
    for (auto const& light : scene.GetLights())
    {
        if (light.type == FLightType::Disk || light.type == FLightType::Rect)
            areaLightCount++;
    }
    const size_t tlasInstanceCount = scene.GetInstances().size() + areaLightCount;
    desc.instanceBudget = RingBudget(scene.GetInstances().size());
    desc.tlasInstanceBudget = RingBudget(tlasInstanceCount);
    desc.materialBudget = RingBudget(scene.GetMaterials().size());
    desc.lightBudget = RingBudget(scene.GetLights().size());

    size_t textureBindings = kGPUScenePersistentTexture2DBindings + kGPUSceneDefaultTextureBindings +
        kGPUSceneTextureBindingSlack;
    for (auto const& texture : scene.GetTextures())
        textureBindings += texture.IsValid() ? 1u : 0u;
    if (scene.GetSceneGlobals().type == FSceneEnvironmentType::EnvMap)
        textureBindings += kGPUSceneEnvMapBindings;
    textureBindings += kGPUScenePersistentTexture3DBindings;
    desc.texturesBudget = CountBudget(textureBindings);
    return desc;
}

bool GPUScene::StagedUploadJob::NeedsScratch() const
{
    return kind == Kind::Blob && blob.codec != FBlobCodec::None;
}

void GPUScene::StagedUploadJob::Write(Allocator* scratchAlloc) const
{
    switch (kind)
    {
    case Kind::None:
        return;
    case Kind::MeshHeader:
        CHECK(ptr != nullptr);
        CHECK(size == sizeof(GSMesh));
        std::memcpy(ptr, &meshData, sizeof(GSMesh));
        return;
    case Kind::CurveHeader:
        CHECK(ptr != nullptr);
        CHECK(size == sizeof(GSCurveSet));
        std::memcpy(ptr, &curveData, sizeof(GSCurveSet));
        return;
    case Kind::Blob:
    {
        CHECK(scene != nullptr);
        CHECK(ptr != nullptr || size == 0);
        if (NeedsScratch())
            CHECK(scratchAlloc != nullptr);
        CHECK(size == static_cast<size_t>(blob.decodedSize));
        CHECK(scene->ReadBlob(blob, ptr, size, scratchAlloc));
        return;
    }
    default:
        CHECK_MSG(false, "Unsupported staged upload job kind {}", static_cast<uint32_t>(kind));
        return;
    }
}

void GPUScene::FlushDirectGeometryUpload()
{
    if (!mDirectGeometryUpload)
        return;
    if (mPrimitiveOffset)
        mPrimitiveBuffer->Flush(0, mPrimitiveOffset);
    if (mCurveAABBOffset)
        mCurveAABBBuffer->Flush(0, mCurveAABBOffset);
}

GPUScene::GPUScene(FContext* ctx, GPUSceneDesc const& desc) :
    mContext(ctx), mInstanceBuffer(ctx->device.Get(), desc.instanceBudget),
    mMaterialBuffer(ctx->device.Get(), desc.materialBudget),
    mLightBuffer(ctx->device.Get(), desc.lightBudget),
    mLightAliasTableBuffer(ctx->device.Get(), desc.lightBudget),
    mTexture2DPool(ctx->device.Get(), ctx->allocator, {.maxBindings = desc.texturesBudget}),
    mTexture3DPool(ctx->device.Get(), ctx->allocator,
                   {.maxBindings = kGPUScenePersistentTexture3DBindings + kGPUSceneTextureBindingSlack}),
    mBLASes(ctx->allocator),
    mBLASBuffers(ctx->allocator),
    mCurveBLASes(ctx->allocator), mCurveBLASBuffers(ctx->allocator),
    mTLASInstanceStride(mContext->device->WriteAccelerationStructureInstanceData({}, nullptr)),
    mTLASInstances(ctx->device.Get(), desc.tlasInstanceBudget * mTLASInstanceStride)
{
    auto caps = mContext->device->GetCapabilities();
    size_t directGeometryBudget = static_cast<size_t>(desc.primitiveBudget) + desc.curveAABBBudget;
    size_t minDirectGeometryHeapSize = std::max(directGeometryBudget, kMinDirectGeometryUploadHeapSize);
    mDirectGeometryUpload = caps.deviceLocalHostVisibleBuffers &&
                            caps.deviceLocalHostVisibleHeapSize >= minDirectGeometryHeapSize;
    RHIResourceDesc geometryResource{
        .heap = RHIDeviceHeapType::Local,
        .hostAccess = mDirectGeometryUpload ? RHIResourceHostAccess::WriteOnly : RHIResourceHostAccess::Invisible,
        .shared = true,
    };
    mPrimitiveBuffer = mContext->device->CreateBuffer(
    {.resource = geometryResource,
     .usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::StorageBuffer |
     RHIBufferUsageBits::DeviceAddress | RHIBufferUsageBits::AccelerationStructureBuildReadOnly,
     .size = desc.primitiveBudget});
    geometryResource.shared = false;
    mCurveAABBBuffer = mContext->device->CreateBuffer(
    {.resource = geometryResource,
     .usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::StorageBuffer |
     RHIBufferUsageBits::DeviceAddress | RHIBufferUsageBits::AccelerationStructureBuildReadOnly,
     .size = desc.curveAABBBudget});
    if (mDirectGeometryUpload)
    {
        mPrimitiveMapped = mPrimitiveBuffer->Map<char>();
        mCurveAABBMapped = mCurveAABBBuffer->Map<char>();
        LOG(GPUScene, LogInfo, "ReBAR available ({} MB budget used). Uploading via direct copy.",
            directGeometryBudget / 1000000.0);
    }
    mTLASBuffer = mContext->device->CreateBuffer(
    {
        .resource = {
            .heap = RHIDeviceHeapType::Local,
            .shared = false
        },
        .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress |
        RHIBufferUsageBits::AccelerationStructureStorage,
        .size = desc.tlasBudget
    });
    mScratchBufferTLAS = mContext->device->CreateBuffer(
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
    mTLAS = mContext->device->CreateAccelerationStructure(tlasDesc);
    
    mSobolMatricesBuffer = mContext->device->CreateBuffer(
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

        mLightGeometryBuffer = mContext->device->CreateBuffer({
            .resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
            .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::DeviceAddress | RHIBufferUsageBits::AccelerationStructureBuildReadOnly,
            .size = sizeof(LightAABBs)
        });

        ImmediateUpload upload(mContext->device.Get(), sizeof(LightAABBs));
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
        auto rectSize = mContext->device->GetAccelerationStructureSizeInfo(rectDesc, sizeInfoScratch.Ptr());
        sizeInfoScratch.Reset(sizeInfoArena);
        auto diskSize = mContext->device->GetAccelerationStructureSizeInfo(diskDesc, sizeInfoScratch.Ptr());

        uint32_t rectOffset = 0;
        uint32_t diskOffset = AlignUp(rectSize.accelerationStructureSize, 256u);
        uint32_t totalSize = diskOffset + diskSize.accelerationStructureSize;

        mLightBLASBuffer = mContext->device->CreateBuffer({
            .resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
            .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress | RHIBufferUsageBits::AccelerationStructureStorage,
            .size = totalSize
        });

        uint32_t scratchSize = std::max(rectSize.buildScratchSize, diskSize.buildScratchSize);
        auto scratch = mContext->device->CreateBuffer({
            .resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
            .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress,
            .size = scratchSize,
            .alignment = 256
        });

        mRectBLAS = mContext->device->CreateAccelerationStructure({
            .type = RHIAccelerationStructureType::BottomLevel,
            .buffer = mLightBLASBuffer.Get(),
            .offset = rectOffset,
            .size = rectSize.accelerationStructureSize
        });
        mDiskBLAS = mContext->device->CreateAccelerationStructure({
            .type = RHIAccelerationStructureType::BottomLevel,
            .buffer = mLightBLASBuffer.Get(),
            .offset = diskOffset,
            .size = diskSize.accelerationStructureSize
        });

        ImmediateContext ctx(mContext->device.Get());
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
        FTexture foundationDefaultTexture2D(mContext->allocator);
        foundationDefaultTexture2D.Initialize(RHIResourceFormat::R32G32B32A32SignedFloat, RHITextureDimension::E2D, 1, 1);
        foundationDefaultTexture2D.bytes.assign(foundationDefaultTexture2D.GetSize(), 0u);
        FTexture foundationDefaultTexture2DFloat(mContext->allocator);
        foundationDefaultTexture2DFloat.Initialize(RHIResourceFormat::R32SignedFloat, RHITextureDimension::E2D, 1, 1);
        foundationDefaultTexture2DFloat.bytes.resize(sizeof(float));
        *reinterpret_cast<float*>(foundationDefaultTexture2DFloat.bytes.data()) = 1.0f;
        auto defaultViewLutSdr =
            LoadLUT(mContext->allocator, kViewLUTsSdr[kDefaultViewLUTSdr].path);
        auto defaultViewLutHdr =
            LoadLUT(mContext->allocator, kViewLUTsHdr[kDefaultViewLUTHdr].path);
        const size_t foundationDefaultBufferFloatSize = sizeof(float);
        mFoundationDefaultBufferFloat = mContext->device->CreateBuffer({
            .resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
            .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination,
            .size = foundationDefaultBufferFloatSize
        });
        const size_t budget = lutE.GetSize() + foundationDefaultTexture2D.GetSize() +
            foundationDefaultTexture2DFloat.GetSize() + sizeof(kSobolMatrices32) + foundationDefaultBufferFloatSize +
            defaultViewLutSdr.GetSize() + defaultViewLutHdr.GetSize();
        ImmediateUpload upload(mContext->device.Get(), budget);
        upload.Begin();
        Upload(&upload, lutE, mLUTGGXEIndex);
        Upload(&upload, foundationDefaultTexture2D, mFoundationDefaultTexture2DIndex, "_FoundationDefaultTexture2D");
        Upload(&upload, foundationDefaultTexture2DFloat, mFoundationDefaultTexture2DFloatIndex,
               "_FoundationDefaultTexture2DFloat");
        float foundationDefaultBufferFloat = 1.0f;
        char* defaultBufferPtr = upload.Upload(mFoundationDefaultBufferFloat.Get(), foundationDefaultBufferFloatSize, 0);
        std::memcpy(defaultBufferPtr, &foundationDefaultBufferFloat, foundationDefaultBufferFloatSize);
        char* ptr = upload.Upload(mSobolMatricesBuffer.Get(), sizeof(kSobolMatrices32), 0);
        std::memcpy(ptr, kSobolMatrices32, sizeof(kSobolMatrices32));
        UploadViewLUTs(&upload, defaultViewLutSdr, defaultViewLutHdr);
        upload.End();
        upload.WaitIdle();
    }
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

GPUScene::UpdateResult GPUScene::UpdateGPUScene(Span<const GSInstance> instances, Span<const GSMaterial> materials, Span<const GSLight> lights)
{
    UpdateResult res{};
    if (!instances.empty()) {
        auto [ptr, off] = AllocateInstance(static_cast<uint32_t>(instances.size()));
        std::memcpy(ptr, instances.data(), instances.size() * sizeof(GSInstance));
        res.firstInstance = off;
        res.numInstances = static_cast<uint32_t>(instances.size());
    }
    if (!materials.empty()) {
        auto [ptr, off] = AllocateMaterial(static_cast<uint32_t>(materials.size()));
        std::memcpy(ptr, materials.data(), materials.size() * sizeof(GSMaterial));
        res.firstMaterial = off;
        res.numMaterials = static_cast<uint32_t>(materials.size());
    }
    if (!lights.empty()) {
        auto [ptr, off] = AllocateLight(static_cast<uint32_t>(lights.size()));
        auto [aliasPtr, aliasOff] = AllocateLightAliasTable(static_cast<uint32_t>(lights.size()));
        Allocator* scratch = mContext->editorFrameScratch ? mContext->editorFrameScratch.get() : mContext->allocator;

        // Alias table
        Vector<float> powers(lights.size(), scratch);
        float weightSum = 0.0f;
        for (size_t i = 0; i < lights.size(); ++i)
        {
            powers[i] = lights[i].selectionWeight;
            weightSum += powers[i];
        }
        res.firstLightAliasTable = aliasOff;
        res.sceneLightWeightSum = weightSum;
        AliasTable table(powers, scratch);
        std::memcpy(aliasPtr, table.mBins.data(), table.mBins.size() * sizeof(Alias));

        // Lights
        std::memcpy(ptr, lights.data(), lights.size() * sizeof(GSLight));
        res.firstLight = off;
        res.numLights = static_cast<uint32_t>(lights.size());

    }
    return res;
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
                   mPrimitiveOffset / static_cast<float>(1 << 20u),
                   mPrimitiveBuffer->mDesc.size / static_cast<float>(1 << 20u));
    fmt::format_to(std::back_inserter(res), "Curve AABB Buffer: {:.1f} MB allocated, used {:.1f} / {:.1f} MB\n",
                   mCurveAABBBuffer->GetAllocationSize() / static_cast<float>(1 << 20u),
                   mCurveAABBOffset / static_cast<float>(1 << 20u),
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

size_t GPUScene::BeginUpload(ImmediateUpload* ctx, FScene const& scene, FSerializedMesh const& src,
                             GSMesh& outData, uint32_t& outOffset, Vector<StagedUploadJob>& outJobs)
{
    CHECK_MSG(!src.lods.empty(), "Serialized mesh has no LODs");
    auto const& lod0 = src.lods[0];
    const size_t size = CalculateMeshPrimitiveSize(src);
    constexpr size_t kAlign = 4;
    outOffset = AlignUp(mPrimitiveOffset, kAlign);
    CHECK_MSG(outOffset + size <= mPrimitiveBuffer->mDesc.size,
              "GPUScene primitive buffer overflow for serialized mesh. Need {} bytes more, have {} left",
              size, mPrimitiveBuffer->mDesc.size - outOffset);
    char* ptr = nullptr;
    if (mDirectGeometryUpload)
    {
        CHECK(mPrimitiveMapped != nullptr);
        ptr = mPrimitiveMapped + outOffset;
    }
    else
    {
        if (ctx->ptr + size > ctx->end)
            return 0;

        ptr = ctx->Upload(mPrimitiveBuffer.Get(), size, outOffset);
        CHECK(ptr != nullptr);
    }
    char* dst = ptr;
    auto Skip = [&](size_t bytes)
    {
        uint32_t off = static_cast<uint32_t>(dst - ptr);
        dst += bytes;
        return off;
    };
    auto AppendBlobJob = [&](FBlobRef const& blob, char* dstPtr)
    {
        StagedUploadJob job{};
        job.scene = &scene;
        job.kind = StagedUploadJob::Kind::Blob;
        job.blob = blob;
        job.ptr = dstPtr;
        job.size = static_cast<size_t>(blob.decodedSize);
        outJobs.push_back(job);
    };

    Skip(sizeof(GSMesh));
    outData.vtxCount = src.vertexCount;
    uint32_t vtxOffset = Skip(static_cast<size_t>(src.vertices.decodedSize));
    outData.vtxOffset = outOffset + vtxOffset;
    AppendBlobJob(src.vertices, ptr + vtxOffset);
    outData.idxCount = lod0.indexCount;
    uint32_t idxOffset = Skip(static_cast<size_t>(lod0.indices.decodedSize));
    outData.idxOffset = outOffset + idxOffset;
    AppendBlobJob(lod0.indices, ptr + idxOffset);
    outData.groupCount = src.dagGroups.count;
    uint32_t groupOffset = Skip(static_cast<size_t>(src.dagGroups.decodedSize));
    outData.groupOffset = outOffset + groupOffset;
    AppendBlobJob(src.dagGroups, ptr + groupOffset);
    outData.meshletCount = src.dagMeshlets.count;
    uint32_t meshletOffset = Skip(static_cast<size_t>(src.dagMeshlets.decodedSize));
    outData.meshletOffset = outOffset + meshletOffset;
    AppendBlobJob(src.dagMeshlets, ptr + meshletOffset);
    uint32_t meshletVtxOffset = Skip(static_cast<size_t>(src.dagMeshletVtx.decodedSize));
    outData.meshletVtxOffset = outOffset + meshletVtxOffset;
    AppendBlobJob(src.dagMeshletVtx, ptr + meshletVtxOffset);
    uint32_t meshletTriOffset = Skip(static_cast<size_t>(src.dagMeshletTri.decodedSize));
    outData.meshletTriOffset = outOffset + meshletTriOffset;
    AppendBlobJob(src.dagMeshletTri, ptr + meshletTriOffset);
    outData.meshletGlobalIndex = mMeshletGlobalCounter;

    StagedUploadJob headerJob{};
    headerJob.kind = StagedUploadJob::Kind::MeshHeader;
    headerJob.ptr = ptr;
    headerJob.size = sizeof(GSMesh);
    headerJob.meshData = outData;
    outJobs.push_back(headerJob);

    size_t written = dst - ptr;
    CHECK_MSG(written == size, "Write mismatch: expected {} got {}", size, written);
    mPrimitiveOffset = outOffset + size;
    mMeshletGlobalCounter += outData.meshletCount;
    return written;
}

size_t GPUScene::BeginUpload(ImmediateUpload* ctx, FScene const& scene, FSerializedCurve const& src,
                             GSCurveSet& outData, uint32_t& outOffset, Vector<StagedUploadJob>& outJobs)
{
    static_assert(sizeof(FCurvePoint) == sizeof(GSCurvePoint));
    static_assert(alignof(FCurvePoint) == alignof(GSCurvePoint));
    static_assert(sizeof(FSerializedCurveSegment) == sizeof(GSCurveSegment));
    static_assert(alignof(FSerializedCurveSegment) == alignof(GSCurveSegment));
    static_assert(sizeof(FSerializedCurveAABB) == sizeof(RHIAccelerationStructureAABB));
    static_assert(alignof(FSerializedCurveAABB) == alignof(RHIAccelerationStructureAABB));

    CHECK_MSG(src.points.decodedSize != 0 && src.segments.count > 0, "Curve set has no renderable segments");
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
    const size_t segmentCount = src.segments.count;
    const size_t size = CalculateCurvePrimitiveSize(src);
    constexpr size_t kAlign = 4;
    outOffset = AlignUp(mPrimitiveOffset, kAlign);
    CHECK_MSG(outOffset + size <= mPrimitiveBuffer->mDesc.size,
              "GPUScene primitive buffer overflow for serialized curve. Need {} bytes more, have {} left",
              size, mPrimitiveBuffer->mDesc.size - outOffset);

    const size_t aabbSize = CalculateCurveAABBSize(src);
    uint32_t aabbOffset = static_cast<uint32_t>(AlignUp(mCurveAABBOffset, alignof(RHIAccelerationStructureAABB)));
    CHECK_MSG(aabbOffset + aabbSize <= mCurveAABBBuffer->mDesc.size,
              "GPUScene curve AABB buffer overflow. Need {} bytes more, have {} left",
              aabbSize, mCurveAABBBuffer->mDesc.size - aabbOffset);
    char* ptr = nullptr;
    char* aabbPtr = nullptr;
    if (mDirectGeometryUpload)
    {
        CHECK(mPrimitiveMapped != nullptr);
        CHECK(mCurveAABBMapped != nullptr);
        ptr = mPrimitiveMapped + outOffset;
        aabbPtr = mCurveAABBMapped + aabbOffset;
    }
    else
    {
        if (ctx->ptr + size + aabbSize > ctx->end)
            return 0;

        ptr = ctx->Upload(mPrimitiveBuffer.Get(), size, outOffset);
        CHECK(ptr != nullptr);
        aabbPtr = ctx->Upload(mCurveAABBBuffer.Get(), aabbSize, aabbOffset);
        CHECK(aabbPtr != nullptr);
    }

    char* dst = ptr;
    auto Skip = [&](size_t bytes)
    {
        uint32_t off = static_cast<uint32_t>(dst - ptr);
        dst += bytes;
        return off;
    };
    auto AppendBlobJob = [&](FBlobRef const& blob, char* dstPtr)
    {
        StagedUploadJob job{};
        job.scene = &scene;
        job.kind = StagedUploadJob::Kind::Blob;
        job.blob = blob;
        job.ptr = dstPtr;
        job.size = static_cast<size_t>(blob.decodedSize);
        outJobs.push_back(job);
    };

    Skip(sizeof(GSCurveSet));
    outData.pointCount = static_cast<uint32_t>(pointCount);
    uint32_t pointOffset = Skip(static_cast<size_t>(src.points.decodedSize));
    outData.pointOffset = outOffset + pointOffset;
    AppendBlobJob(src.points, ptr + pointOffset);
    outData.segmentCount = static_cast<uint32_t>(segmentCount);
    uint32_t segmentOffset = Skip(static_cast<size_t>(src.segments.decodedSize));
    outData.segmentOffset = outOffset + segmentOffset;
    AppendBlobJob(src.segments, ptr + segmentOffset);
    outData.materialIndex = src.materialIndex;
    outData.aabbOffset = aabbOffset;
    AppendBlobJob(src.aabbs, aabbPtr);

    StagedUploadJob headerJob{};
    headerJob.kind = StagedUploadJob::Kind::CurveHeader;
    headerJob.ptr = ptr;
    headerJob.size = sizeof(GSCurveSet);
    headerJob.curveData = outData;
    outJobs.push_back(headerJob);

    size_t written = dst - ptr;
    CHECK_MSG(written == size, "Write mismatch: expected {} got {}", size, written);
    mPrimitiveOffset = outOffset + size;
    mCurveAABBOffset = outData.aabbOffset + aabbSize;
    return written + aabbSize;
}

size_t GPUScene::Upload(ImmediateUpload* ctx, FTexture const& source, uint32_t& outIndex, const char* debugName)
{
    return Upload(ctx, static_cast<FTextureHeader const&>(source),
                  Span<const unsigned char>(source.bytes.data(), source.bytes.size()), outIndex, debugName);
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

size_t GPUScene::UploadOrUpdateTexture(ImmediateUpload* ctx, FTexture const& source, uint32_t& index,
                                       const char* debugName)
{
    CHECK_MSG(source.IsValid(), "Texture is invalid");
    auto const& metadata = static_cast<FTextureHeader const&>(source);
    CHECK_MSG(source.bytes.size() == metadata.GetSize(), "Texture data size mismatch: data {} header {}",
              source.bytes.size(), metadata.GetSize());

    auto texture = mContext->device->CreateTexture(metadata.GetDesc());
    if (debugName)
        texture->DebugSetObjectName(debugName);
    size_t written = 0;
    auto range = RHITextureSubresourceRange::Create(
        RHITextureAspectFlagBits::Color,
        0, metadata.GetNumMips(),
        0, texture->mDesc.arrayLayers);
    auto* cmd = ctx->ctx.Get();
    cmd->BeginTransition();
    cmd->SetImageTransition(texture.Get(), {
                                .dstImgLayout = RHITextureLayout::TransferDst,
                                .srcImgRange = range
                            });
    cmd->EndTransition();

    uint32_t blockSize = metadata.GetBlockSize(), blockDim = 4;
    if (!blockSize)
        blockSize = metadata.GetBpp() / 8, blockDim = 1;
    CHECK_MSG(blockSize && blockDim, "Unsupported texture format {}", metadata.GetFormat());

    for (uint32_t layer = 0; layer < metadata.GetNumLayers(); ++layer)
    {
        uint64_t layerOffset =
            uint64_t(layer) * FTextureHeader::CalculateTextureImageSize(
                                  metadata.GetWidth(), metadata.GetHeight(), metadata.GetDepth(),
                                  metadata.GetNumMips(), blockSize, blockDim);
        for (uint32_t mip = 0; mip < metadata.GetNumMips(); ++mip)
        {
            uint64_t mipOffset = FTextureHeader::CalculateTextureImageSize(
                metadata.GetWidth(), metadata.GetHeight(), metadata.GetDepth(), mip, blockSize, blockDim);
            RHIExtent3D mipExtent = metadata.GetMipExtent(mip);
            uint64_t mipSize64 = FTextureHeader::CalculateTextureImageSize(
                mipExtent.x, mipExtent.y, mipExtent.z, 1u, blockSize, blockDim);
            CHECK_MSG(mipSize64 <= std::numeric_limits<size_t>::max(),
                      "Texture subresource size {} exceeds addressable range", mipSize64);
            size_t mipSize = static_cast<size_t>(mipSize64);
            uint64_t subresourceOffset = layerOffset + mipOffset;
            uint64_t subresourceEnd = subresourceOffset + mipSize64;
            CHECK_MSG(subresourceEnd <= source.bytes.size(),
                      "Texture subresource out of range: layer {}, mip {} (size {}), data size {}",
                      layer, mip, mipSize, source.bytes.size());
            if (!ctx->Align(std::max(metadata.GetBpp() / 8, metadata.GetBlockSize())))
                return 0;
            char* ptr = ctx->Upload(texture.Get(), mipSize,
                                    {
                                        .aspect = RHITextureAspectFlagBits::Color,
                                        .mipLevel = mip, .baseArrayLayer = layer, .layerCount = 1
                                    },
                                    {0, 0, 0}, mipExtent);
            if (ptr == nullptr)
                return 0;
            std::memcpy(ptr, source.bytes.data() + subresourceOffset, mipSize);
            written += mipSize;
        }
    }
    cmd->BeginTransition();
    cmd->SetImageTransition(texture.Get(), {
                                .srcImgLayout = RHITextureLayout::TransferDst,
                                .dstImgLayout = RHITextureLayout::ShaderReadOnly,
                                .srcImgRange = range
                            });
    cmd->EndTransition();
    auto view = texture->CreateTextureView({
        .format = metadata.GetFormat(),
        .dimension = metadata.GetViewDimension(),
        .range = RHITextureSubresourceRange::Create(
            RHITextureAspectFlagBits::Color,
            0, metadata.GetNumMips(),
            0, texture->mDesc.arrayLayers)
    });
    BindlessPool& texturePool = SelectTexturePool(metadata.GetViewDimension());
    if (index == UINT32_MAX)
        index = texturePool.Allocate(std::move(texture), std::move(view));
    else
        texturePool.Update(index, std::move(texture), std::move(view));
    return written;
}

size_t GPUScene::Upload(ImmediateUpload* ctx, FTextureHeader const& metadata, Span<const unsigned char> data,
                        uint32_t& outIndex, const char* debugName)
{
    CHECK_MSG(metadata.IsValid(), "Texture is invalid");
    CHECK_MSG(data.size_bytes() == metadata.GetSize(), "Texture data size mismatch: data {} header {}",
              data.size_bytes(), metadata.GetSize());

    auto texture = mContext->device->CreateTexture(metadata.GetDesc());
    if (debugName)
        texture->DebugSetObjectName(debugName);
    size_t written = 0;
    auto range = RHITextureSubresourceRange::Create(
        RHITextureAspectFlagBits::Color,
        0, metadata.GetNumMips(),
        0, texture->mDesc.arrayLayers);
    auto* cmd = ctx->ctx.Get();
    cmd->BeginTransition();
    cmd->SetImageTransition(texture.Get(), {
                                .dstImgLayout = RHITextureLayout::TransferDst,
                                .srcImgRange = range
                            });
    cmd->EndTransition();

    uint32_t blockSize = metadata.GetBlockSize(), blockDim = 4;
    if (!blockSize)
        blockSize = metadata.GetBpp() / 8, blockDim = 1;
    CHECK_MSG(blockSize && blockDim, "Unsupported texture format {}", metadata.GetFormat());

    for (uint32_t layer = 0; layer < metadata.GetNumLayers(); ++layer)
    {
        uint64_t layerOffset =
            uint64_t(layer) * FTextureHeader::CalculateTextureImageSize(
                                  metadata.GetWidth(), metadata.GetHeight(), metadata.GetDepth(),
                                  metadata.GetNumMips(), blockSize, blockDim);
        for (uint32_t mip = 0; mip < metadata.GetNumMips(); ++mip)
        {
            uint64_t mipOffset = FTextureHeader::CalculateTextureImageSize(
                metadata.GetWidth(), metadata.GetHeight(), metadata.GetDepth(), mip, blockSize, blockDim);
            RHIExtent3D mipExtent = metadata.GetMipExtent(mip);
            uint64_t mipSize64 = FTextureHeader::CalculateTextureImageSize(
                mipExtent.x, mipExtent.y, mipExtent.z, 1u, blockSize, blockDim);
            CHECK_MSG(mipSize64 <= std::numeric_limits<size_t>::max(),
                      "Texture subresource size {} exceeds addressable range", mipSize64);
            size_t mipSize = static_cast<size_t>(mipSize64);
            uint64_t subresourceOffset = layerOffset + mipOffset;
            uint64_t subresourceEnd = subresourceOffset + mipSize64;
            CHECK_MSG(subresourceEnd <= data.size_bytes(),
                      "Texture subresource out of range: layer {}, mip {} (size {}), data size {}",
                      layer, mip, mipSize, data.size_bytes());
            if (!ctx->Align(std::max(metadata.GetBpp() / 8, metadata.GetBlockSize())))
                return 0;
            char* ptr = ctx->Upload(texture.Get(), mipSize,
                                    {
                                        .aspect = RHITextureAspectFlagBits::Color,
                                        .mipLevel = mip, .baseArrayLayer = layer, .layerCount = 1
                                    },
                                    {0, 0, 0}, mipExtent);
            if (ptr == nullptr)
                return 0;
            std::memcpy(ptr, data.data() + subresourceOffset, mipSize);
            written += mipSize;
        }
    }
    cmd->BeginTransition();
    cmd->SetImageTransition(texture.Get(), {
                                .srcImgLayout = RHITextureLayout::TransferDst,
                                .dstImgLayout = RHITextureLayout::ShaderReadOnly,
                                .srcImgRange = range
                            });
    cmd->EndTransition();
    auto view = texture->CreateTextureView({
        .format = metadata.GetFormat(),
        .dimension = metadata.GetViewDimension(),
        .range = RHITextureSubresourceRange::Create(
            RHITextureAspectFlagBits::Color,
            0, metadata.GetNumMips(),
            0, texture->mDesc.arrayLayers)
    });
    outIndex = SelectTexturePool(metadata.GetViewDimension()).Allocate(std::move(texture), std::move(view));
    return written;
}

GPUScene::TextureUpload GPUScene::BeginTextureUpload(ImmediateUpload* ctx, FSerializedTexture const& source,
                                                     const char* debugName)
{
    CHECK_MSG(source.IsValid(), "Serialized texture is invalid");

    FTextureHeader const& metadata = static_cast<FTextureHeader const&>(source);
    TextureUpload upload{};
    upload.metadata = metadata;
    upload.texture = mContext->device->CreateTexture(metadata.GetDesc());
    if (debugName)
        upload.texture->DebugSetObjectName(debugName);

    auto range = RHITextureSubresourceRange::Create(
        RHITextureAspectFlagBits::Color,
        0, metadata.GetNumMips(),
        0, upload.texture->mDesc.arrayLayers);
    auto* cmd = ctx->ctx.Get();
    cmd->BeginTransition();
    cmd->SetImageTransition(upload.texture.Get(), {
                                .dstImgLayout = RHITextureLayout::TransferDst,
                                .srcImgRange = range
                            });
    cmd->EndTransition();
    return upload;
}

size_t GPUScene::BeginTextureSubresourceUpload(ImmediateUpload* ctx, FScene const& scene,
                                               FSerializedTexture const& source, TextureUpload& upload,
                                               uint32_t layer, uint32_t mip,
                                               Vector<StagedUploadJob>& outJobs)
{
    CHECK(upload.IsValid());
    CHECK_MSG(source.IsValid(), "Serialized texture is invalid");

    FTextureHeader const& metadata = static_cast<FTextureHeader const&>(source);
    CHECK_MSG(upload.metadata.GetSize() == metadata.GetSize(), "Texture upload metadata mismatch");
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
    char* ptr = ctx->Upload(upload.texture.Get(), subresourceSize,
                            {
                                .aspect = RHITextureAspectFlagBits::Color,
                                .mipLevel = mip, .baseArrayLayer = layer, .layerCount = 1
                            },
                            {0, 0, 0}, mipExtent);
    CHECK(ptr != nullptr);

    StagedUploadJob job{};
    job.scene = &scene;
    job.kind = StagedUploadJob::Kind::Blob;
    job.blob = subresourceBlob;
    job.ptr = ptr;
    job.size = subresourceSize;
    outJobs.push_back(job);
    return subresourceSize;
}

void GPUScene::EndTextureUpload(ImmediateUpload* ctx, TextureUpload&& upload, uint32_t& outIndex)
{
    CHECK(upload.IsValid());
    FTextureHeader const& metadata = upload.metadata;
    auto range = RHITextureSubresourceRange::Create(
        RHITextureAspectFlagBits::Color,
        0, metadata.GetNumMips(),
        0, upload.texture->mDesc.arrayLayers);
    auto* cmd = ctx->ctx.Get();
    cmd->BeginTransition();
    cmd->SetImageTransition(upload.texture.Get(), {
                                .srcImgLayout = RHITextureLayout::TransferDst,
                                .dstImgLayout = RHITextureLayout::ShaderReadOnly,
                                .srcImgRange = range
                            });
    cmd->EndTransition();

    auto view = upload.texture->CreateTextureView({
        .format = metadata.GetFormat(),
        .dimension = metadata.GetViewDimension(),
        .range = RHITextureSubresourceRange::Create(
            RHITextureAspectFlagBits::Color,
            0, metadata.GetNumMips(),
            0, upload.texture->mDesc.arrayLayers)
    });
    outIndex = SelectTexturePool(metadata.GetViewDimension()).Allocate(std::move(upload.texture), std::move(view));
}

size_t GPUScene::BeginUpload(ImmediateUpload* ctx, FScene const& scene, FSerializedTexture const& source,
                             uint32_t& outIndex, Vector<StagedUploadJob>& outJobs, const char* debugName)
{
    CHECK_MSG(source.IsValid(), "Serialized texture is invalid");

    FTextureHeader const& metadata = static_cast<FTextureHeader const&>(source);
    uint32_t alignment = GetTextureUploadAlignment(metadata);
    char* preflight = ctx->ptr;
    for (uint32_t layer = 0; layer < metadata.GetNumLayers(); ++layer)
    {
        for (uint32_t mip = 0; mip < metadata.GetNumMips(); ++mip)
        {
            FBlobRef const& subresourceBlob = source.GetSubresourceBlob(layer, mip);
            size_t const subresourceSize = metadata.GetSubresourceSize(layer, mip);
            CHECK_MSG(subresourceBlob.decodedSize == subresourceSize,
                      "Serialized texture subresource size mismatch: layer {}, mip {}, blob {}, expected {}",
                      layer, mip, subresourceBlob.decodedSize, subresourceSize);
            preflight = reinterpret_cast<char*>(AlignUp(reinterpret_cast<uintptr_t>(preflight), alignment));
            if (preflight >= ctx->end || static_cast<size_t>(ctx->end - preflight) < subresourceSize)
                return 0;
            preflight += subresourceSize;
        }
    }

    auto texture = mContext->device->CreateTexture(metadata.GetDesc());
    if (debugName)
        texture->DebugSetObjectName(debugName);
    size_t written = 0;
    auto range = RHITextureSubresourceRange::Create(
        RHITextureAspectFlagBits::Color,
        0, metadata.GetNumMips(),
        0, texture->mDesc.arrayLayers);
    auto* cmd = ctx->ctx.Get();
    cmd->BeginTransition();
    cmd->SetImageTransition(texture.Get(), {
                                .dstImgLayout = RHITextureLayout::TransferDst,
                                .srcImgRange = range
                            });
    cmd->EndTransition();

    for (uint32_t layer = 0; layer < metadata.GetNumLayers(); ++layer)
    {
        for (uint32_t mip = 0; mip < metadata.GetNumMips(); ++mip)
        {
            FBlobRef const& subresourceBlob = source.GetSubresourceBlob(layer, mip);
            size_t const subresourceSize = metadata.GetSubresourceSize(layer, mip);
            CHECK(ctx->Align(alignment));
            RHIExtent3D mipExtent = metadata.GetMipExtent(mip);
            char* ptr = ctx->Upload(texture.Get(), subresourceSize,
                                    {
                                        .aspect = RHITextureAspectFlagBits::Color,
                                        .mipLevel = mip, .baseArrayLayer = layer, .layerCount = 1
                                    },
                                    {0, 0, 0}, mipExtent);
            CHECK(ptr != nullptr);

            StagedUploadJob job{};
            job.scene = &scene;
            job.kind = StagedUploadJob::Kind::Blob;
            job.blob = subresourceBlob;
            job.ptr = ptr;
            job.size = subresourceSize;
            outJobs.push_back(job);
            written += subresourceSize;
        }
    }
    cmd->BeginTransition();
    cmd->SetImageTransition(texture.Get(), {
                                .srcImgLayout = RHITextureLayout::TransferDst,
                                .dstImgLayout = RHITextureLayout::ShaderReadOnly,
                                .srcImgRange = range
                            });
    cmd->EndTransition();
    auto view = texture->CreateTextureView({
        .format = metadata.GetFormat(),
        .dimension = metadata.GetViewDimension(),
        .range = RHITextureSubresourceRange::Create(
            RHITextureAspectFlagBits::Color,
            0, metadata.GetNumMips(),
            0, texture->mDesc.arrayLayers)
    });
    outIndex = SelectTexturePool(metadata.GetViewDimension()).Allocate(std::move(texture), std::move(view));
    return written;
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

    auto* device = mContext->device.Get();
    // Build
    Vector<RHIAccelerationStructureGeometryInfo> geometries(meshes.size(), mContext->allocator);
    Vector<RHIAccelerationStructureBuildRangeInfo> buildRanges(meshes.size(), mContext->allocator);
    Vector<RHIAccelerationStructureBuildDesc> buildDesc(meshes.size(), mContext->allocator);
    Vector<RHIAccelerationStructureSizeInfo> sizeInfo(meshes.size(), mContext->allocator);
    Vector<uint32_t> blasOffsets(meshes.size(), mContext->allocator);
    Vector<uint32_t> scratchOffsets(meshes.size(), mContext->allocator);
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
    auto scratch = mContext->device->CreateBuffer(
    {
        .resource = {
            .heap = RHIDeviceHeapType::Local,
            .shared = false
        },
        .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress,
        .size = scratchOffset,
        .alignment = 256 // Aligned to Vulkan spec. Should be large enough for other APIs as well?
    });
    auto buffer = mContext->device->CreateBuffer(
    {
        .resource = {
            .heap = RHIDeviceHeapType::Local,
            .shared = false
        },
        .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress |
        RHIBufferUsageBits::AccelerationStructureStorage,
        .size = blasOffset
    });
    Vector<RHIDeviceHandle<RHIAccelerationStructure>> newBLASHandles(mContext->allocator);
    Vector<RHIAccelerationStructure*> newBLASPtrs(mContext->allocator);
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
    Vector<uint32_t> compactOffsets(meshes.size(), mContext->allocator);
    auto compactSizes = queryPool->GetResults();
    for (size_t i = 0; i < meshes.size(); i++)
    {
        auto compactedSize = compactSizes[i];
        compactOffsets[i] = compactOffset;
        compactOffset = AlignUp(compactOffset + static_cast<uint32_t>(compactedSize), 256u);
    }
    auto& compactBuffer = mBLASBuffers.emplace_back(mContext->device->CreateBuffer(
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
        outBLASIndices[i] = static_cast<uint32_t>(mBLASes.size());
        auto& blas = mBLASes.emplace_back(device->CreateAccelerationStructure(as));
        cmd->CopyAccelerationStructure(newBLASPtrs[i], blas.Get(), true /* compact */);
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

    auto* device = mContext->device.Get();
    Vector<RHIAccelerationStructureGeometryInfo> geometries(curves.size(), mContext->allocator);
    Vector<RHIAccelerationStructureBuildRangeInfo> buildRanges(curves.size(), mContext->allocator);
    Vector<RHIAccelerationStructureBuildDesc> buildDesc(curves.size(), mContext->allocator);
    Vector<RHIAccelerationStructureSizeInfo> sizeInfo(curves.size(), mContext->allocator);
    Vector<uint32_t> blasOffsets(curves.size(), mContext->allocator);
    Vector<uint32_t> scratchOffsets(curves.size(), mContext->allocator);
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
    Vector<RHIDeviceScopedHandle<RHIAccelerationStructure>> newBLASes(curves.size(), mContext->allocator);
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
        outBLASIndices[i] = static_cast<uint32_t>(mCurveBLASes.size());
        auto& blas = mCurveBLASes.emplace_back(device->CreateAccelerationStructure(as));
        desc.scratchBuffer = scratchBuffer.Get();
        desc.scratchBufferOffset = scratchOffsets[i];
        desc.dstAS = blas.Get();
        cmd->BuildAccelerationStructure({{{desc}}});
    }
    cmd->End(), ctx->Submit(firstSubmitDesc), ctx->WaitIdle();
    LOG(GPUScene, LogDebug, "Curve BLAS Upload Complete: {} BLASes, {} MB used",
        curves.size(), blasOffset / 1e6);
}

uint32_t GPUScene::CountTLASInstances(Span<const GSInstance> instances, Span<const GSLight> lights) const
{
    uint32_t numAreaLights = 0;
    for (const auto& light : lights)
    {
        if (light.type == 3 || light.type == 4)
            numAreaLights++;
    }
    CHECK_MSG(instances.size() <= UINT32_MAX - numAreaLights,
              "TLAS instance count overflow: {} scene instances and {} area lights",
              instances.size(), numAreaLights);
    return static_cast<uint32_t>(instances.size()) + numAreaLights;
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
    auto* device = mContext->device.Get();
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

bool GPUScene::EnsureTLASCapacity(Span<const GSInstance> instances, Span<const GSLight> lights)
{
    return EnsureTLASCapacity(CountTLASInstances(instances, lights), true);
}

GPUScene::TLASBuildResult GPUScene::BuildTLAS(RHICommandList* cmd, Span<const GSInstance> instances,
                                             Span<const uint32_t> blasIndices,
                                             Span<const uint32_t> curveBLASIndices,
                                             Span<const GSLight> lights, bool update)
{
    uint32_t totalInstances = CountTLASInstances(instances, lights);
    if (totalInstances == 0)
    {
        mLastTLASInstancesCount = 0;
        return TLASBuildResult::Empty;
    }
    if (!EnsureTLASCapacity(totalInstances, !update))
        return TLASBuildResult::NeedsRendererRebuild;
    if (totalInstances != mLastTLASInstancesCount)
    {
        update = false;
        mLastTLASInstancesCount = totalInstances;
    }

    auto ConvertInstance = [&](const GSInstance* src) -> RHIAccelerationStructureGeometryInstance
    {
        RHIAccelerationStructureGeometryInstance res{
            .instanceID = static_cast<uint32_t>(src - instances.data()),
        };
        if (src->type == kGSInstanceTypeCurve)
        {
            res.mask = 0x04; // CURVE_MASK
            res.shaderBindingTableRecordOffset = kCurveSBTOffset;
        }
        else
        {
            res.mask = 0x01; // MESH_MASK
        }
        mat3 basis = transpose(mat3(scale(src->scale)) * mat3_cast(src->rotation));
        std::memcpy(res.transformBasisRowMajor[0], &basis[0], sizeof(float) * 3);
        std::memcpy(res.transformBasisRowMajor[1], &basis[1], sizeof(float) * 3);
        std::memcpy(res.transformBasisRowMajor[2], &basis[2], sizeof(float) * 3);
        res.transformTranslation[0] = src->transform.x;
        res.transformTranslation[1] = src->transform.y;
        res.transformTranslation[2] = src->transform.z;
        return res;
    };

    auto ConvertLight = [&](const GSLight* src) -> RHIAccelerationStructureGeometryInstance
    {
        RHIAccelerationStructureGeometryInstance res{
            .instanceID = static_cast<uint32_t>(src - lights.data()) | (1u << 23),
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
    for (const auto & instance : instances)
    {
        auto data = ConvertInstance(&instance);
        if (instance.type == kGSInstanceTypeCurve)
        {
            CHECK_MSG(instance.resourceIndex < curveBLASIndices.size(), "Curve instance references invalid curve BLAS {}",
                      instance.resourceIndex);
            data.blas = mCurveBLASes[curveBLASIndices[instance.resourceIndex]].Get();
        }
        else
        {
            CHECK_MSG(instance.resourceIndex < blasIndices.size(), "Mesh instance references invalid mesh BLAS {}",
                      instance.resourceIndex);
            data.blas = mBLASes[blasIndices[instance.resourceIndex]].Get();
        }
        pInstances += mContext->device->WriteAccelerationStructureInstanceData(data, pInstances);
    }
    for (const auto & light : lights)
    {
        if (light.type == 3 || light.type == 4)
        {
            auto data = ConvertLight(&light);
            pInstances += mContext->device->WriteAccelerationStructureInstanceData(data, pInstances);
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

void GPUScene::UploadEnvMap(ImmediateUpload* ctx, FTexture const& source)
{
    CHECK_MSG(UploadOrUpdateTexture(ctx, source, mEnvMapIndex, "Environment Map"),
              "Environment map staging budget exhausted");
    // Compute CDFs for importance sampling
    uint32_t width = source.GetWidth();
    uint32_t height = source.GetHeight();
    Span<const unsigned char> data = source.GetSubresource(0, 0);
    const float4* pixels = reinterpret_cast<const float4*>(data.data());
    
    Vector<float> f(width * height, mContext->allocator);
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
    
    PiecewiseConstant2D cdf(f, width, height, mContext->allocator);
    
    // Upload Marginal CDF as Texture2D
    FTexture marginalTex(mContext->allocator);
    marginalTex.Initialize(RHIResourceFormat::R32SignedFloat, RHITextureDimension::E2D,
                           cdf.mMarginal->mCDF.size(), 1);
    const size_t marginalSize = cdf.mMarginal->mCDF.size() * sizeof(float);
    marginalTex.bytes.resize(marginalSize);
    std::memcpy(marginalTex.bytes.data(), cdf.mMarginal->mCDF.data(), marginalSize);
    CHECK_MSG(UploadOrUpdateTexture(ctx, marginalTex, mEnvMapMarginalCDFIndex, "Environment Map Marginal CDF"),
              "Environment map marginal CDF staging budget exhausted");
    
    // Upload Conditional CDF as Texture2D
    FTexture conditionalTex(mContext->allocator);
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
    
    CHECK_MSG(UploadOrUpdateTexture(ctx, conditionalTex, mEnvMapConditionalCDFIndex, "Environment Map Conditional CDF"),
              "Environment map conditional CDF staging budget exhausted");

    LOG(GPUScene, LogInfo, "Environment map uploaded: {}x{}", source.GetWidth(), source.GetHeight());
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

void GPUScene::UploadViewLUTs(ImmediateUpload* ctx, FTexture const& sdr, FTexture const& hdr)
{
    CheckViewLUT(sdr, "SDR");
    CheckViewLUT(hdr, "HDR");
    CHECK_MSG(UploadOrUpdateTexture(ctx, sdr, mLUTViewSdrIndex, "View LUT SDR") &&
              UploadOrUpdateTexture(ctx, hdr, mLUTViewHdrIndex, "View LUT HDR"),
              "View LUT staging budget exhausted");
}

static RHITexture* ResolvePoolTexture(BindlessPool& pool, uint32_t index)
{
    if (index == UINT32_MAX)
        return nullptr;
    auto& res = pool.GetResource(index);
    return res.Visit(
        [](RHITexture* tex) -> RHITexture* { return tex; },
        [](RHIDeviceScopedHandle<RHITexture> const& tex) -> RHITexture* { return tex.Get(); }
    );
}

RHITexture* GPUScene::GetEnvMap() const
{
    return ResolvePoolTexture(const_cast<BindlessPool&>(mTexture2DPool), mEnvMapIndex);
}

RHITexture* GPUScene::GetGGXlutE() const
{
    return ResolvePoolTexture(const_cast<BindlessPool&>(mTexture2DPool), mLUTGGXEIndex);
}

RHITexture* GPUScene::GetViewLutSdr() const
{
    return ResolvePoolTexture(const_cast<BindlessPool&>(mTexture3DPool), mLUTViewSdrIndex);
}

RHITexture* GPUScene::GetViewLutHdr() const
{
    return ResolvePoolTexture(const_cast<BindlessPool&>(mTexture3DPool), mLUTViewHdrIndex);
}

RHITexture* GPUScene::GetFoundationDefaultTexture2D() const
{
    return ResolvePoolTexture(const_cast<BindlessPool&>(mTexture2DPool), mFoundationDefaultTexture2DIndex);
}

RHITexture* GPUScene::GetFoundationDefaultTexture2DFloat() const
{
    return ResolvePoolTexture(const_cast<BindlessPool&>(mTexture2DPool), mFoundationDefaultTexture2DFloatIndex);
}

RHITexture* GPUScene::GetEnvMapMarginalCDF() const
{
    return ResolvePoolTexture(const_cast<BindlessPool&>(mTexture2DPool), mEnvMapMarginalCDFIndex);
}

RHITexture* GPUScene::GetEnvMapConditionalCDF() const
{
    return ResolvePoolTexture(const_cast<BindlessPool&>(mTexture2DPool), mEnvMapConditionalCDFIndex);
}

RHIBuffer* GPUScene::GetSobolMatricesBuffer() const
{
    return mSobolMatricesBuffer.Get();
}

void GPUScene::Reset()
{
    mPrimitiveOffset = 0;
    mCurveAABBOffset = 0;
    mMeshletGlobalCounter = 0;
    mLastTLASInstancesCount = 0;
    mBLASes.clear();
    mBLASBuffers.clear();
    mCurveBLASes.clear();
    mCurveBLASBuffers.clear();
    mMaterialBuffer.Reset();
    mInstanceBuffer.Reset();
    mLightBuffer.Reset();
    mLightAliasTableBuffer.Reset();
    // NOTE: texture pools are append-only; old bindings become dead entries.
    //       mTLAS is kept alive and rebuilt in-place by BuildTLAS.
}