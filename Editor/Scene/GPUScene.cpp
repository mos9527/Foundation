#include "../Render/Precompute.hpp"
#include "../Render/Tables.hpp"
#include "../Render/ViewLUTs.hpp"
#include <Core/AllocatorStack.hpp>
#include <Core/Paths.hpp>
#include <Math/Quantize.hpp>
#include <algorithm>
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
    tex.data.assign(bytes, bytes + tex.GetSize());
    return tex;
}

static uint32_t GetRenderableCurveSegmentCount(FCurveSet const& src)
{
    uint32_t segmentCount = 0;
    for (uint32_t count : src.curveVertexCounts)
    {
        switch (src.basis)
        {
        case FCurveBasis::Bezier:
            CHECK_MSG(count >= 4 && (count - 1) % 3 == 0,
                      "Bezier curve strands must contain 3n + 1 controls, got {}", count);
            segmentCount += (count - 1) / 3;
            break;
        case FCurveBasis::Linear:
            segmentCount += count > 1 ? count - 1 : 0;
            break;
        default:
            CHECK_MSG(false, "Unsupported curve basis {}", static_cast<uint32_t>(src.basis));
            break;
        }
    }
    return segmentCount;
}

static constexpr uint32_t kGPUSceneRingFrameSlack = 3u;
static constexpr uint32_t kGPUScenePersistentTextureBindings = 3u; // GGX + default SDR/HDR view LUTs.
static constexpr uint32_t kGPUSceneSceneViewLUTBindings = 2u;
static constexpr uint32_t kGPUSceneEnvMapBindings = 2u; // Env map + conditional CDF texture.
static constexpr uint32_t kGPUSceneTextureBindingSlack = 8u;
static constexpr size_t kGPUSceneByteBudgetSlack = 64u << 10u;

static size_t GetQuantizedVertexCount(FMesh const& src)
{
    if (!src.verticesQuantized.empty())
        return src.verticesQuantized.size();
    if (src.verticesCompressedCount != 0)
        return src.verticesCompressedCount;
    return src.vertices.size();
}

static size_t GetLOD0IndexCount(FMesh const& src)
{
    CHECK_MSG(!src.lods.empty(), "Mesh has no LODs");
    auto const& lod = src.lods[0];
    if (!lod.indices.empty())
        return lod.indices.size();
    return lod.indicesCompressedCount;
}

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

size_t GPUScene::CalculateMeshPrimitiveSize(FMesh const& src)
{
    return sizeof(GSMesh) +
        sizeof(FQVertex) * GetQuantizedVertexCount(src) +
        sizeof(uint32_t) * GetLOD0IndexCount(src) +
        sizeof(FLODGroup) * src.dag.groups.size() +
        sizeof(FMeshlet) * src.dag.meshlets.size() +
        sizeof(uint32_t) * src.dag.meshletVtx.size() +
        sizeof(uint8_t) * src.dag.meshletTri.size();
}

size_t GPUScene::CalculateCurvePrimitiveSize(FCurveSet const& src)
{
    return sizeof(GSCurveSet) +
        sizeof(GSCurvePoint) * src.points.size() +
        sizeof(GSCurveSegment) * GetRenderableCurveSegmentCount(src);
}

size_t GPUScene::CalculateCurveAABBSize(FCurveSet const& src)
{
    return sizeof(RHIAccelerationStructureAABB) * GetRenderableCurveSegmentCount(src);
}

GPUScene::GPUSceneDesc GPUScene::CalculateSceneBudget(FScene const& scene, RHIDeviceCapabilities const& caps)
{
    GPUSceneDesc desc{};
    size_t primitiveBytes = 0;
    for (auto const& mesh : scene.mMeshes)
    {
        primitiveBytes = AlignUp(primitiveBytes, size_t(4));
        primitiveBytes += CalculateMeshPrimitiveSize(mesh);
    }
    size_t curveAABBBytes = 0;
    for (auto const& curve : scene.mCurves)
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
    for (auto const& light : scene.mLights)
    {
        if (light.type == FLightType::Disk || light.type == FLightType::Rect)
            areaLightCount++;
    }
    const size_t tlasInstanceCount = scene.mInstances.size() + scene.mCurveInstances.size() + areaLightCount;
    desc.instanceBudget = RingBudget(std::max({scene.mInstances.size(), scene.mCurveInstances.size(), tlasInstanceCount}));
    desc.materialBudget = RingBudget(scene.mMaterials.size());
    desc.lightBudget = RingBudget(scene.mLights.size());

    size_t textureBindings = kGPUScenePersistentTextureBindings + kGPUSceneSceneViewLUTBindings + kGPUSceneTextureBindingSlack;
    for (auto const& texture : scene.mTextures)
        textureBindings += texture.IsValid() ? 1u : 0u;
    if (scene.mEnvironment.type == FSceneEnvironmentType::EnvMap && scene.mEnvironmentMap.IsValid())
        textureBindings += kGPUSceneEnvMapBindings;
    desc.texturesBudget = CountBudget(textureBindings);
    return desc;
}

static void AddCurveLineSegment(Vector<GSCurveSegment>& segments,
                                Vector<RHIAccelerationStructureAABB>& aabbs,
                                Vector<GSCurvePoint> const& points,
                                uint32_t p0,
                                uint32_t p1,
                                float u0,
                                float u1)
{
    segments.push_back(GSCurveSegment{
        .p0 = p0,
        .p1 = p1,
        .u0 = u0,
        .u1 = u1,
    });

    const auto& a = points[p0];
    const auto& b = points[p1];
    float radius = std::max(a.radius, b.radius);
    float3 mn = min(a.position, b.position) - float3(radius);
    float3 mx = max(a.position, b.position) + float3(radius);
    aabbs.push_back(RHIAccelerationStructureAABB{mn.x, mn.y, mn.z, mx.x, mx.y, mx.z});
}

static void AddBezierCurveSpan(Vector<GSCurveSegment>& segments,
                               Vector<RHIAccelerationStructureAABB>& aabbs,
                               Vector<GSCurvePoint> const& points,
                               uint32_t p0,
                               uint32_t p1,
                               float u0,
                               float u1)
{
    segments.push_back(GSCurveSegment{
        .p0 = p0,
        .p1 = p1,
        .u0 = u0,
        .u1 = u1,
    });

    const auto& a = points[p0];
    const auto& b = points[p0 + 1];
    const auto& c = points[p0 + 2];
    const auto& d = points[p1];
    float radius = std::max(std::max(a.radius, b.radius), std::max(c.radius, d.radius));
    float3 mn = min(min(a.position, b.position), min(c.position, d.position)) - float3(radius);
    float3 mx = max(max(a.position, b.position), max(c.position, d.position)) + float3(radius);
    aabbs.push_back(RHIAccelerationStructureAABB{mn.x, mn.y, mn.z, mx.x, mx.y, mx.z});
}

GPUScene::GPUScene(FContext* ctx, GPUSceneDesc const& desc) :
    mContext(ctx), mInstanceBuffer(ctx->device.Get(), desc.instanceBudget),
    mCurveInstanceBuffer(ctx->device.Get(), desc.instanceBudget),
    mMaterialBuffer(ctx->device.Get(), desc.materialBudget),
    mLightBuffer(ctx->device.Get(), desc.lightBudget),
    mLightAliasTableBuffer(ctx->device.Get(), desc.lightBudget),
    mTexturePool(ctx->device.Get(), ctx->allocator, {.maxBindings = desc.texturesBudget}), mBLASes(ctx->allocator),
    mBLASBuffers(ctx->allocator),
    mCurveBLASes(ctx->allocator), mCurveBLASBuffers(ctx->allocator),
    mTLASInstanceStride(mContext->device->WriteAccelerationStructureInstanceData({}, nullptr)),
    mTLASInstances(ctx->device.Get(), desc.instanceBudget * mTLASInstanceStride)
{
    mPrimitiveBuffer = mContext->device->CreateBuffer(
    {.resource = {.heap = RHIDeviceHeapType::Local, .shared = true},
     .usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::StorageBuffer |
     RHIBufferUsageBits::DeviceAddress | RHIBufferUsageBits::AccelerationStructureBuildReadOnly,
     .size = desc.primitiveBudget});
    mCurveAABBBuffer = mContext->device->CreateBuffer(
    {.resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
     .usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::StorageBuffer |
     RHIBufferUsageBits::DeviceAddress | RHIBufferUsageBits::AccelerationStructureBuildReadOnly,
     .size = desc.curveAABBBudget});
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
    RHIAccelerationStructureDesc tlasDesc{
        .type = RHIAccelerationStructureType::TopLevel,
        .buffer = mTLASBuffer.Get(),
        .size = desc.tlasBudget
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
        auto defaultViewLutSdr =
            LoadLUT(mContext->allocator, kViewLUTsSdr[kDefaultViewLUTSdr].path);
        auto defaultViewLutHdr =
            LoadLUT(mContext->allocator, kViewLUTsHdr[kDefaultViewLUTHdr].path);
        const size_t budget =
            lutE.GetSize() + sizeof(kSobolMatrices32) + defaultViewLutSdr.GetSize() + defaultViewLutHdr.GetSize();
        ImmediateUpload upload(mContext->device.Get(), budget);
        upload.Begin();
        Upload(&upload, lutE, mLUTGGXEIndex);
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

Pair<GSCurveInstance*, uint32_t> GPUScene::AllocateCurveInstance(uint32_t count)
{
    return mCurveInstanceBuffer.Allocate(count);
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

GPUScene::UpdateResult GPUScene::UpdateGPUScene(Span<const GSInstance> instances, Span<const GSCurveInstance> curveInstances, Span<const GSMaterial> materials, Span<const GSLight> lights)
{
    UpdateResult res{};
    if (!instances.empty()) {
        auto [ptr, off] = AllocateInstance(static_cast<uint32_t>(instances.size()));
        std::memcpy(ptr, instances.data(), instances.size() * sizeof(GSInstance));
        res.firstInstance = off;
        res.numInstances = static_cast<uint32_t>(instances.size());
    }
    if (!curveInstances.empty()) {
        auto [ptr, off] = AllocateCurveInstance(static_cast<uint32_t>(curveInstances.size()));
        std::memcpy(ptr, curveInstances.data(), curveInstances.size() * sizeof(GSCurveInstance));
        res.firstCurveInstance = off;
        res.numCurveInstances = static_cast<uint32_t>(curveInstances.size());
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
    auto textureStats = mTexturePool.GetStats();
    size_t instanceBytes = AddRingBufferSize(mInstanceBuffer);
    size_t curveInstanceBytes = AddRingBufferSize(mCurveInstanceBuffer);
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
    size_t envCDFBytes = AddBufferSize(mEnvMapMarginalCDF);

    outStats.push_back({"Primitive Buffer", primitiveBytes});
    outStats.push_back({"Curve AABB Buffer", curveAABBBytes});
    outStats.push_back({"Texture Pool (Owned)", textureStats.ownedTextureBytes});
    outStats.push_back({"Instance Buffer", instanceBytes});
    outStats.push_back({"Dynamic Upload Buffers",
                        curveInstanceBytes + materialBytes + lightBytes + lightAliasBytes + tlasInstanceBytes});
    outStats.push_back({"Mesh BLAS", blasBytes});
    outStats.push_back({"Curve BLAS", curveBLASBytes});
    outStats.push_back({"TLAS", tlasBytes});
    outStats.push_back({"TLAS Scratch", tlasScratchBytes});
    outStats.push_back({"Light AS", lightBLASBytes});
    outStats.push_back({"Other GPUScene Buffers", lightGeometryBytes + sobolBytes + envCDFBytes});
}

String GPUScene::DbgGetBufferStatistics() const
{
    String res;
    Vector<MemoryStat> stats(GLOBAL_ALLOC);
    DbgGetMemoryStatistics(stats);
    size_t totalBytes = 0;
    for (auto const& stat : stats)
        totalBytes += stat.bytes;

    auto textureStats = mTexturePool.GetStats();
    fmt::format_to(std::back_inserter(res), "Primitive Buffer: {:.1f} MB allocated, used {:.1f} / {:.1f} MB\n",
                   mPrimitiveBuffer->GetAllocationSize() / static_cast<float>(1 << 20u),
                   mPrimitiveOffset / static_cast<float>(1 << 20u),
                   mPrimitiveBuffer->mDesc.size / static_cast<float>(1 << 20u));
    fmt::format_to(std::back_inserter(res), "Curve AABB Buffer: {:.1f} MB allocated, used {:.1f} / {:.1f} MB\n",
                   mCurveAABBBuffer->GetAllocationSize() / static_cast<float>(1 << 20u),
                   mCurveAABBOffset / static_cast<float>(1 << 20u),
                   mCurveAABBBuffer->mDesc.size / static_cast<float>(1 << 20u));
    fmt::format_to(std::back_inserter(res), "Texture Pool: {:.1f} MB owned, {:.1f} MB referenced, used {} / {} bindings, owned {} textures\n",
                   textureStats.ownedTextureBytes / static_cast<float>(1 << 20u),
                   textureStats.referencedTextureBytes / static_cast<float>(1 << 20u),
                   textureStats.activeBindings,
                   textureStats.capacity,
                   textureStats.ownedTextureBindings);
    fmt::format_to(std::back_inserter(res), "Instance Buffer: {:.1f} MB allocated, used {} / {} instances\n",
                   mInstanceBuffer.mBuffer->GetAllocationSize() / static_cast<float>(1 << 20u),
                   mInstanceBuffer.Used(), mInstanceBuffer.Capacity());
    for (auto const& stat : stats)
        fmt::format_to(std::back_inserter(res), "{}: {:.1f} MB\n", stat.type,
                       stat.bytes / static_cast<float>(1 << 20u));
    fmt::format_to(std::back_inserter(res), "GPUScene Tracked Total: {:.1f} MB",
                   totalBytes / static_cast<float>(1 << 20u));
    return res;
}

size_t GPUScene::Upload(ImmediateUpload* ctx, FMesh const& src, GSMesh& outData, uint32_t& outOffset)
{
    // Only upload DAG data
    const size_t size = CalculateMeshPrimitiveSize(src);
    // We need to ensure the *worst* alignment case fits per DXC docs
    // https://github.com/microsoft/DirectXShaderCompiler/wiki/ByteAddressBuffer-Load-Store-Additions
    // We can consider the GSMesh, FVertex, etc. as one struct - aligning to its largest member
    // uint32_t, in this case - would be sufficient.
    constexpr size_t kAlign = 4;
    outOffset = AlignUp(mPrimitiveOffset, kAlign);
    CHECK_MSG(outOffset + size <= mPrimitiveBuffer->mDesc.size, "GPUScene primitive buffer overflow for FMesh. Need {} bytes more, have {} left", size, mPrimitiveBuffer->mDesc.size - outOffset);
    // Allocate staging memory to upload into
    char *ptr = ctx->Upload(mPrimitiveBuffer.Get(), size, outOffset), *dst = ptr;
    if (ptr == nullptr)
        return 0;
    auto Write = [&](const void* pData, size_t bytes)
    {
        std::memcpy(dst, pData, bytes);
        uint32_t off = dst - ptr;
        dst += bytes;
        return off;
    };
    // GSMesh (stub)
    Write(&outData, sizeof(GSMesh));
    // Vertex data
    outData.vtxCount = src.verticesQuantized.size();
    outData.vtxOffset = outOffset + Write(src.verticesQuantized.data(),
                                          sizeof(FQVertex) * src.verticesQuantized.size());
    outData.idxCount = src.lods[0].indices.size();
    outData.idxOffset = outOffset + Write(src.lods[0].indices.data(), sizeof(uint32_t) * src.lods[0].indices.size());
    // LOD Group data
    outData.groupCount = src.dag.groups.size();
    outData.groupOffset = outOffset + Write(src.dag.groups.data(), sizeof(FLODGroup) * src.dag.groups.size());
    // Meshlet data
    outData.meshletCount = src.dag.meshlets.size();
    outData.meshletOffset = outOffset + Write(src.dag.meshlets.data(), sizeof(FMeshlet) * src.dag.meshlets.size());
    outData.meshletVtxOffset = outOffset + Write(src.dag.meshletVtx.data(),
                                                 sizeof(uint32_t) * src.dag.meshletVtx.size());
    outData.meshletTriOffset = outOffset +
        Write(src.dag.meshletTri.data(), sizeof(uint8_t) * src.dag.meshletTri.size());
    outData.meshletGlobalIndex = mMeshletGlobalCounter;
    // GSMesh (data)
    std::memcpy(ptr, &outData, sizeof(GSMesh));
    size_t written = dst - ptr;
    CHECK_MSG(written == size, "Write mismatch: expected {} got {}", size, written);
    mPrimitiveOffset = outOffset + size;
    mMeshletGlobalCounter += outData.meshletCount;
    return dst - ptr;
}

size_t GPUScene::Upload(ImmediateUpload* ctx, FCurveSet const& src, GSCurveSet& outData, uint32_t& outOffset)
{
    uint32_t segmentCount = GetRenderableCurveSegmentCount(src);
    CHECK_MSG(!src.points.empty() && segmentCount > 0, "Curve set has no renderable segments");

    Vector<GSCurvePoint> points(src.points.size(), GLOBAL_ALLOC);
    for (size_t i = 0; i < src.points.size(); ++i)
        points[i] = GSCurvePoint{.position = src.points[i].position, .radius = src.points[i].radius};

    Vector<GSCurveSegment> segments(GLOBAL_ALLOC);
    segments.reserve(segmentCount);
    Vector<RHIAccelerationStructureAABB> aabbs(GLOBAL_ALLOC);
    aabbs.reserve(segmentCount);

    uint32_t pointCursor = 0;
    for (uint32_t count : src.curveVertexCounts)
    {
        CHECK_MSG(pointCursor + count <= points.size(), "Curve set references more points than it stores");
        if (count <= 1)
        {
            pointCursor += count;
            continue;
        }
        switch (src.basis)
        {
        case FCurveBasis::Bezier:
        {
            CHECK_MSG(count >= 4 && (count - 1) % 3 == 0,
                      "Bezier curve strands must contain 3n + 1 controls, got {}", count);
            uint32_t spanCount = (count - 1) / 3;
            float invSpanCount = 1.0f / float(spanCount);
            for (uint32_t i = 0; i < spanCount; ++i)
            {
                uint32_t p0 = pointCursor + i * 3;
                uint32_t p1 = pointCursor + (i + 1) * 3;
                AddBezierCurveSpan(segments, aabbs, points, p0, p1,
                                   float(i) * invSpanCount, float(i + 1) * invSpanCount);
            }
            break;
        }
        case FCurveBasis::Linear:
        {
            float invSegmentCount = 1.0f / float(count - 1);
            for (uint32_t i = 0; i + 1 < count; ++i)
            {
                uint32_t p0 = pointCursor + i;
                uint32_t p1 = pointCursor + i + 1;
                AddCurveLineSegment(segments, aabbs, points, p0, p1,
                                    float(i) * invSegmentCount, float(i + 1) * invSegmentCount);
            }
            break;
        }
        default:
            CHECK_MSG(false, "Unsupported curve basis {}", static_cast<uint32_t>(src.basis));
            break;
        }
        pointCursor += count;
    }

    const size_t size = CalculateCurvePrimitiveSize(src);
    constexpr size_t kAlign = 4;
    outOffset = AlignUp(mPrimitiveOffset, kAlign);
    CHECK_MSG(outOffset + size <= mPrimitiveBuffer->mDesc.size,
              "GPUScene primitive buffer overflow for FCurveSet. Need {} bytes more, have {} left",
              size, mPrimitiveBuffer->mDesc.size - outOffset);

    char* ptr = ctx->Upload(mPrimitiveBuffer.Get(), size, outOffset);
    if (ptr == nullptr)
        return 0;
    char* dst = ptr;
    auto Write = [&](const void* pData, size_t bytes)
    {
        std::memcpy(dst, pData, bytes);
        uint32_t off = static_cast<uint32_t>(dst - ptr);
        dst += bytes;
        return off;
    };

    Write(&outData, sizeof(GSCurveSet));
    outData.pointCount = static_cast<uint32_t>(points.size());
    outData.pointOffset = outOffset + Write(points.data(), sizeof(GSCurvePoint) * points.size());
    outData.segmentCount = static_cast<uint32_t>(segments.size());
    outData.segmentOffset = outOffset + Write(segments.data(), sizeof(GSCurveSegment) * segments.size());
    outData.materialIndex = src.materialIndex;

    const size_t aabbSize = CalculateCurveAABBSize(src);
    outData.aabbOffset = static_cast<uint32_t>(AlignUp(mCurveAABBOffset, alignof(RHIAccelerationStructureAABB)));
    CHECK_MSG(outData.aabbOffset + aabbSize <= mCurveAABBBuffer->mDesc.size,
              "GPUScene curve AABB buffer overflow. Need {} bytes more, have {} left",
              aabbSize, mCurveAABBBuffer->mDesc.size - outData.aabbOffset);
    char* aabbPtr = ctx->Upload(mCurveAABBBuffer.Get(), aabbSize, outData.aabbOffset);
    if (aabbPtr == nullptr)
        return 0;
    std::memcpy(aabbPtr, aabbs.data(), aabbSize);

    std::memcpy(ptr, &outData, sizeof(GSCurveSet));
    size_t written = dst - ptr;
    CHECK_MSG(written == size, "Write mismatch: expected {} got {}", size, written);
    mPrimitiveOffset = outOffset + size;
    mCurveAABBOffset = outData.aabbOffset + aabbSize;
    return written + aabbSize;
}

size_t GPUScene::Upload(ImmediateUpload* ctx, FTexture const& source, uint32_t& outIndex, const char* debugName)
{
    auto texture = mContext->device->CreateTexture(source.GetDesc());
    if (debugName)
        texture->DebugSetObjectName(debugName);
    size_t written = 0;
    auto range = RHITextureSubresourceRange::Create(
        RHITextureAspectFlagBits::Color,
        0, source.GetNumMips(),
        0, texture->mDesc.arrayLayers);
    auto* cmd = ctx->ctx.Get();
    cmd->BeginTransition();
    cmd->SetImageTransition(texture.Get(), {
                                .dstImgLayout = RHITextureLayout::TransferDst,
                                .srcImgRange = range
                            });
    cmd->EndTransition();
    for (uint32_t layer = 0; layer < source.GetNumLayers(); ++layer)
    {
        for (uint32_t mip = 0; mip < source.GetNumMips(); ++mip)
        {
            Span<const unsigned char> subresource = source.GetSubresource(mip, layer);
            if (!ctx->Align(std::max(source.GetBpp() / 8, source.GetBlockSize())))
                return 0;
            char* ptr = ctx->Upload(texture.Get(), subresource.size_bytes(),
                                    {
                                        .aspect = RHITextureAspectFlagBits::Color,
                                        .mipLevel = mip, .baseArrayLayer = layer, .layerCount = 1
                                    },
                                    {0, 0, 0}, source.GetMipExtent(mip));
            if (ptr == nullptr) // XXX: Pessimistic, we can't resume from a partial upload this way. Though
                return 0; // overhead should be minimal for most textures.
            std::memcpy(ptr, subresource.data(), subresource.size_bytes());
            written += subresource.size_bytes();
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
        .format = source.GetFormat(),
        .dimension = source.GetViewDimension(),
        .range = RHITextureSubresourceRange::Create(
            RHITextureAspectFlagBits::Color,
            0, source.GetNumMips(),
            0, texture->mDesc.arrayLayers)
    });
    outIndex = mTexturePool.Allocate(std::move(texture), view.Release().Get());
    return written;
}

// Reference:
// - https://github.com/zeux/niagara/blob/master/src/scenert.cpp
// - https://docs.vulkan.org/tutorial/latest/courses/18_Ray_tracing/02_Acceleration_structures.html
// - https://docs.vulkan.org/tutorial/latest/courses/18_Ray_tracing/04_TLAS_animation.html
void GPUScene::BuildBLAS(ImmediateContext* ctx, Span<const GSMesh> meshes, Span<uint32_t> outBLASIndices)
{
    CHECK_MSG(meshes.size() == outBLASIndices.size(), "Mismatched BLAS indices size");
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
    Vector<RHIDeviceScopedHandle<RHIAccelerationStructure>> newBlases(mContext->allocator);
    Vector<RHIAccelerationStructure*> newBlasPtrs(mContext->allocator);
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
        auto& blas = newBlases.emplace_back(device->CreateAccelerationStructure(as));
        auto& desc = buildDesc[i];
        newBlasPtrs.push_back(blas.Get());
        cmd->BeginTransition();
        cmd->SetBufferTransition(scratch.Get(), {
                                     .srcStage = RHIPipelineStageBits::AccelerationBuild,
                                     .dstStage = RHIPipelineStageBits::AccelerationBuild
                                 });
        cmd->EndTransition();
        desc.scratchBuffer = scratch.Get();
        desc.scratchBufferOffset = scratchOffsets[i];
        desc.dstAS = blas.Get();
        cmd->BuildAccelerationStructure({{{desc}}});
    }
    cmd->End();
    ctx->Submit(), ctx->WaitIdle();
    // Compact
    auto queryPool = device->CreateQueryPool({
        .type = RHIDeviceQueryPool::QueryPoolDesc::AccelerationStructureCompactedSize,
        .count = static_cast<uint32_t>(meshes.size())
    });
    queryPool->Reset();
    cmd->Begin();
    cmd->WriteAccelerationStructureCompactedSize(newBlasPtrs, queryPool.Get(), 0);
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
        cmd->CopyAccelerationStructure(
            newBlases[i].Get(), blas.Get(), true /* compact */
            );
    }
    cmd->End(), ctx->Submit(), ctx->WaitIdle();
    LOG(GPUScene, LogDebug, "BLAS Upload Complete: {} BLASes, {} MB used (compacted from {} MB)",
        meshes.size(),
        compactOffset / 1e6,
        blasOffset / 1e6);
}

void GPUScene::BuildCurveBLAS(ImmediateContext* ctx, Span<const GSCurveSet> curves, Span<uint32_t> outBLASIndices)
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
    cmd->End(), ctx->Submit(), ctx->WaitIdle();
    LOG(GPUScene, LogDebug, "Curve BLAS Upload Complete: {} BLASes, {} MB used",
        curves.size(), blasOffset / 1e6);
}

void GPUScene::BuildTLAS(RHICommandList* cmd, Span<const GSInstance> instances, Span<const uint32_t> blasIndices,
                         Span<const GSCurveInstance> curveInstances, Span<const uint32_t> curveBLASIndices,
                         Span<const GSLight> lights, bool update)
{
    CHECK_MSG(curveInstances.size() == curveBLASIndices.size(), "Mismatched curve TLAS BLAS indices size");
    uint32_t numAreaLights = 0;
    for (const auto& light : lights)
    {
        if (light.type == 3 || light.type == 4)
            numAreaLights++;
    }

    uint32_t totalInstances = static_cast<uint32_t>(instances.size() + curveInstances.size()) + numAreaLights;
    if (totalInstances == 0)
        return;
    if (totalInstances != mLastTLASInstancesCount)
    {
        update = false;
        mLastTLASInstancesCount = totalInstances;
    }

    auto* device = mContext->device.Get();
    auto ConvertInstance = [&](const GSInstance* src) -> RHIAccelerationStructureGeometryInstance
    {
        RHIAccelerationStructureGeometryInstance res{
            .instanceID = static_cast<uint32_t>(src - instances.data()),
            .mask = 0x01, // MESH_MASK
        };
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
        float3 n = normalize(cross(u, v));
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

    auto ConvertCurve = [&](const GSCurveInstance* src) -> RHIAccelerationStructureGeometryInstance
    {
        RHIAccelerationStructureGeometryInstance res{
            .instanceID = static_cast<uint32_t>(src - curveInstances.data()) | (1u << 22),
            .mask = 0x04, // CURVE_MASK
            .shaderBindingTableRecordOffset = kCurveSBTOffset,
        };
        mat3 basis = transpose(mat3(scale(src->scale)) * mat3_cast(src->rotation));
        std::memcpy(res.transformBasisRowMajor[0], &basis[0], sizeof(float) * 3);
        std::memcpy(res.transformBasisRowMajor[1], &basis[1], sizeof(float) * 3);
        std::memcpy(res.transformBasisRowMajor[2], &basis[2], sizeof(float) * 3);
        res.transformTranslation[0] = src->transform.x;
        res.transformTranslation[1] = src->transform.y;
        res.transformTranslation[2] = src->transform.z;
        return res;
    };

    // NOTE: Byte buffers
    auto [pInstances, instancesOffset] = mTLASInstances.Allocate(mTLASInstanceStride * totalInstances);
    for (const auto & instance : instances)
    {
        auto data = ConvertInstance(&instance);
        data.blas = mBLASes[blasIndices[instance.meshIndex]].Get();
        pInstances += mContext->device->WriteAccelerationStructureInstanceData(data, pInstances);
    }
    for (const auto & curveInstance : curveInstances)
    {
        auto data = ConvertCurve(&curveInstance);
        data.blas = mCurveBLASes[curveBLASIndices[curveInstance.curveIndex]].Get();
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
    RHIAccelerationStructureBuildRangeInfo range{.primitiveCount = totalInstances
    };
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
    if (!update)
    {
        StackArena<4096> sizeInfoArena;
        AllocatorStack sizeInfoScratch(sizeInfoArena);
        auto size = device->GetAccelerationStructureSizeInfo(desc, sizeInfoScratch.Ptr());
        CHECK_MSG(size.accelerationStructureSize <= mTLASBuffer->mDesc.size, "TLAS buffer overflow");
        CHECK_MSG(size.buildScratchSize <= mScratchBufferTLAS->mDesc.size, "TLAS build scratch buffer overflow");
        CHECK_MSG(size.updateScratchSize <= mScratchBufferTLAS->mDesc.size, "TLAS update scratch buffer overflow");
    }
    desc.scratchBuffer = mScratchBufferTLAS.Get();
    desc.scratchBufferOffset = 0;
    desc.dstAS = mTLAS.Get();
    if (update)
        desc.srcAS = mTLAS.Get();
    cmd->BuildAccelerationStructure({{{desc}}});
}

void GPUScene::UploadEnvMap(ImmediateUpload* ctx, FTexture const& source)
{
    Upload(ctx, source, mEnvMapIndex);

    
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
    
    // Upload Marginal CDF
    size_t marginalSize = cdf.mMarginal->mCDF.size() * sizeof(float);
    mEnvMapMarginalCDF = mContext->device->CreateBuffer({
        .resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
        .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination,
        .size = marginalSize
    });
    char* marginalPtr = ctx->Upload(mEnvMapMarginalCDF.Get(), marginalSize, 0);
    std::memcpy(marginalPtr, cdf.mMarginal->mCDF.data(), marginalSize);
    
    // Upload Conditional CDF as Texture2D
    FTexture conditionalTex(mContext->allocator);
    conditionalTex.Initialize(RHIResourceFormat::R32SignedFloat, RHITextureDimension::E2D,
                              cdf.mConditional[0]->mCDF.size(), height);
    
    size_t conditionalSize = height * cdf.mConditional[0]->mCDF.size() * sizeof(float);
    conditionalTex.data.resize(conditionalSize);
    
    for (uint32_t y = 0; y < height; ++y)
    {
        std::memcpy(conditionalTex.data.data() + y * cdf.mConditional[0]->mCDF.size() * sizeof(float),
                    cdf.mConditional[y]->mCDF.data(),
                    cdf.mConditional[y]->mCDF.size() * sizeof(float));
    }
    
    Upload(ctx, conditionalTex, mEnvMapConditionalCDFIndex);

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
    CHECK_MSG(Upload(ctx, sdr, mLUTViewSdrIndex, "View LUT SDR") &&
              Upload(ctx, hdr, mLUTViewHdrIndex, "View LUT HDR"),
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
    return ResolvePoolTexture(const_cast<BindlessPool&>(mTexturePool), mEnvMapIndex);
}

RHITexture* GPUScene::GetGGXlutE() const
{
    return ResolvePoolTexture(const_cast<BindlessPool&>(mTexturePool), mLUTGGXEIndex);
}

RHITexture* GPUScene::GetViewLutSdr() const
{
    return ResolvePoolTexture(const_cast<BindlessPool&>(mTexturePool), mLUTViewSdrIndex);
}

RHITexture* GPUScene::GetViewLutHdr() const
{
    return ResolvePoolTexture(const_cast<BindlessPool&>(mTexturePool), mLUTViewHdrIndex);
}

RHITexture* GPUScene::GetEnvMapConditionalCDF() const
{
    return ResolvePoolTexture(const_cast<BindlessPool&>(mTexturePool), mEnvMapConditionalCDFIndex);
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
    mCurveInstanceBuffer.Reset();
    mLightBuffer.Reset();
    mLightAliasTableBuffer.Reset();
    // NOTE: mTexturePool is append-only; old bindings become dead entries.
    //       mTLAS is kept alive and rebuilt in-place by BuildTLAS.
}