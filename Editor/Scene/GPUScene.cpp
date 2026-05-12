#include "GPUScene.hpp"
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
    tex.bytes.assign(bytes, bytes + tex.GetSize());
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
static constexpr uint32_t kGPUSceneDefaultTextureBindings = 2u; // _FoundationDefault Texture2D + Texture2DFloat.
static constexpr uint32_t kGPUSceneEnvMapBindings = 2u; // Env map + conditional CDF texture.
static constexpr uint32_t kGPUSceneTextureBindingSlack = 8u;
static constexpr size_t kGPUSceneByteBudgetSlack = 64u << 10u;

static size_t GetQuantizedVertexCount(FMesh const& src)
{
    if (!src.verticesQuantized.empty())
        return src.verticesQuantized.size();
    return src.vertices.size();
}

static size_t GetLOD0IndexCount(FMesh const& src)
{
    CHECK_MSG(!src.lods.empty(), "Mesh has no LODs");
    auto const& lod = src.lods[0];
    return lod.indices.size();
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

size_t GPUScene::CalculateCurvePrimitiveSize(FCurveSet const& src)
{
    return sizeof(GSCurveSet) +
        sizeof(GSCurvePoint) * src.points.size() +
        sizeof(GSCurveSegment) * GetRenderableCurveSegmentCount(src);
}

size_t GPUScene::CalculateCurvePrimitiveSize(FSerializedCurve const& src)
{
    return sizeof(GSCurveSet) +
        src.points.decodedSize +
        sizeof(GSCurveSegment) * src.numSegments;
}

size_t GPUScene::CalculateCurveAABBSize(FCurveSet const& src)
{
    return sizeof(RHIAccelerationStructureAABB) * GetRenderableCurveSegmentCount(src);
}

size_t GPUScene::CalculateCurveAABBSize(FSerializedCurve const& src)
{
    return sizeof(RHIAccelerationStructureAABB) * src.numSegments;
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
    desc.instanceBudget = RingBudget(std::max(scene.GetInstances().size(), tlasInstanceCount));
    desc.materialBudget = RingBudget(scene.GetMaterials().size());
    desc.lightBudget = RingBudget(scene.GetLights().size());

    size_t textureBindings = kGPUScenePersistentTextureBindings + kGPUSceneSceneViewLUTBindings +
        kGPUSceneDefaultTextureBindings + kGPUSceneTextureBindingSlack;
    for (auto const& texture : scene.GetTextures())
        textureBindings += texture.IsValid() ? 1u : 0u;
    if (scene.GetSceneGlobals().type == FSceneEnvironmentType::EnvMap)
        textureBindings += kGPUSceneEnvMapBindings;
    desc.texturesBudget = CountBudget(textureBindings);
    return desc;
}

static void AddCurveLineSegment(Vector<GSCurveSegment>& segments,
                                Vector<RHIAccelerationStructureAABB>& aabbs,
                                Span<const GSCurvePoint> points,
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
                               Span<const GSCurvePoint> points,
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

void GPUScene::StagedUploadJob::Write() const
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
    case Kind::Blob:
        CHECK(scene != nullptr);
        CHECK(ptr != nullptr || size == 0);
        CHECK(size == static_cast<size_t>(blob.decodedSize));
        CHECK(scene->ReadBlob(blob, ptr, size));
        return;
    case Kind::BlobRange:
        CHECK(scene != nullptr);
        CHECK(ptr != nullptr || size == 0);
        CHECK(scene->ReadBlobRange(blob, blobOffset, ptr, size));
        return;
    case Kind::SerializedCurve:
    {
        static_assert(sizeof(FCurvePoint) == sizeof(GSCurvePoint));
        static_assert(alignof(FCurvePoint) == alignof(GSCurvePoint));

        CHECK(scene != nullptr);
        CHECK(ptr != nullptr);
        CHECK(curveAABBPtr != nullptr || curveAABBSize == 0);
        CHECK_MSG(curve.points.decodedSize != 0 && curve.numSegments > 0, "Curve set has no renderable segments");
        CHECK_MSG(curve.points.stride == sizeof(GSCurvePoint), "Serialized curve point stride mismatch");
        CHECK_MSG(curve.points.decodedSize % sizeof(GSCurvePoint) == 0, "Serialized curve point blob size mismatch");
        CHECK_MSG(curveData.pointOffset >= curvePrimitiveOffset, "Serialized curve point offset underflow");
        CHECK_MSG(curveData.segmentOffset >= curvePrimitiveOffset, "Serialized curve segment offset underflow");

        Vector<uint32_t> curveVertexCounts = scene->ReadBlobArray<uint32_t>(curve.curveVertexCounts);
        const size_t pointCount = static_cast<size_t>(curve.points.decodedSize / sizeof(GSCurvePoint));
        auto* pointsData = reinterpret_cast<GSCurvePoint*>(ptr + curveData.pointOffset - curvePrimitiveOffset);
        CHECK(scene->ReadBlob(curve.points, pointsData, static_cast<size_t>(curve.points.decodedSize)));
        Span<const GSCurvePoint> points(pointsData, pointCount);
        auto* segmentsData = reinterpret_cast<GSCurveSegment*>(ptr + curveData.segmentOffset - curvePrimitiveOffset);

        uint32_t segmentCursor = 0;
        auto WriteAABB = [&](RHIAccelerationStructureAABB const& aabb)
        {
            std::memcpy(curveAABBPtr + sizeof(RHIAccelerationStructureAABB) * segmentCursor,
                        &aabb, sizeof(RHIAccelerationStructureAABB));
        };
        auto WriteLineSegment = [&](uint32_t p0, uint32_t p1, float u0, float u1)
        {
            CHECK(segmentCursor < curveData.segmentCount);
            segmentsData[segmentCursor] = GSCurveSegment{.p0 = p0, .p1 = p1, .u0 = u0, .u1 = u1};

            const auto& a = points[p0];
            const auto& b = points[p1];
            float radius = std::max(a.radius, b.radius);
            float3 mn = min(a.position, b.position) - float3(radius);
            float3 mx = max(a.position, b.position) + float3(radius);
            WriteAABB(RHIAccelerationStructureAABB{mn.x, mn.y, mn.z, mx.x, mx.y, mx.z});
            segmentCursor++;
        };
        auto WriteBezierSpan = [&](uint32_t p0, uint32_t p1, float u0, float u1)
        {
            CHECK(segmentCursor < curveData.segmentCount);
            segmentsData[segmentCursor] = GSCurveSegment{.p0 = p0, .p1 = p1, .u0 = u0, .u1 = u1};

            const auto& a = points[p0];
            const auto& b = points[p0 + 1];
            const auto& c = points[p0 + 2];
            const auto& d = points[p1];
            float radius = std::max(std::max(a.radius, b.radius), std::max(c.radius, d.radius));
            float3 mn = min(min(a.position, b.position), min(c.position, d.position)) - float3(radius);
            float3 mx = max(max(a.position, b.position), max(c.position, d.position)) + float3(radius);
            WriteAABB(RHIAccelerationStructureAABB{mn.x, mn.y, mn.z, mx.x, mx.y, mx.z});
            segmentCursor++;
        };

        uint32_t pointCursor = 0;
        for (uint32_t count : curveVertexCounts)
        {
            CHECK_MSG(pointCursor + count <= points.size(), "Curve set references more points than it stores");
            if (count <= 1)
            {
                pointCursor += count;
                continue;
            }
            switch (curve.basis)
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
                    WriteBezierSpan(p0, p1, float(i) * invSpanCount, float(i + 1) * invSpanCount);
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
                    WriteLineSegment(p0, p1, float(i) * invSegmentCount, float(i + 1) * invSegmentCount);
                }
                break;
            }
            default:
                CHECK_MSG(false, "Unsupported curve basis {}", static_cast<uint32_t>(curve.basis));
                break;
            }
            pointCursor += count;
        }

        CHECK_MSG(segmentCursor == curveData.segmentCount, "Curve segment count mismatch: expected {} got {}",
                  curveData.segmentCount, segmentCursor);
        CHECK_MSG(size == sizeof(GSCurveSet) + curve.points.decodedSize + sizeof(GSCurveSegment) * segmentCursor,
                  "Curve primitive staging size mismatch");
        CHECK_MSG(curveAABBSize == sizeof(RHIAccelerationStructureAABB) * segmentCursor,
                  "Curve AABB staging size mismatch");
        std::memcpy(ptr, &curveData, sizeof(GSCurveSet));
        return;
    }
    default:
        CHECK_MSG(false, "Unsupported staged upload job kind {}", static_cast<uint32_t>(kind));
        return;
    }
}

GPUScene::GPUScene(FContext* ctx, GPUSceneDesc const& desc) :
    mContext(ctx), mInstanceBuffer(ctx->device.Get(), desc.instanceBudget),
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
    auto textureStats = mTexturePool.GetStats();
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
    size_t envCDFBytes = AddBufferSize(mEnvMapMarginalCDF);

    outStats.push_back({"Primitive Buffer (Buffer)", primitiveBytes});
    outStats.push_back({"Curve AABB Buffer (Buffer)", curveAABBBytes});
    outStats.push_back({"Texture Pool (Texture)", textureStats.ownedTextureBytes});
    outStats.push_back({"Instance Buffer (Buffer)", instanceBytes});
    outStats.push_back({"Dynamic Upload Buffers (Buffer)",
                        materialBytes + lightBytes + lightAliasBytes + tlasInstanceBytes});
    outStats.push_back({"Mesh BLAS (Buffer)", blasBytes});
    outStats.push_back({"Curve BLAS (Buffer)", curveBLASBytes});
    outStats.push_back({"TLAS (Buffer)", tlasBytes});
    outStats.push_back({"TLAS Scratch (Buffer)", tlasScratchBytes});
    outStats.push_back({"Light AS (Buffer)", lightBLASBytes});
    outStats.push_back({"Other GPUScene Buffers (Buffer)", lightGeometryBytes + sobolBytes + defaultBufferBytes + envCDFBytes});
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
        fmt::format_to(std::back_inserter(res), "{}: {:.1f} MB\n", stat.name,
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

size_t GPUScene::Upload(ImmediateUpload* ctx, FScene const& scene, FSerializedMesh const& src,
                         GSMesh& outData, uint32_t& outOffset)
{
    CHECK_MSG(!src.lods.empty(), "Serialized mesh has no LODs");
    auto const& lod0 = src.lods[0];
    const size_t size = CalculateMeshPrimitiveSize(src);
    constexpr size_t kAlign = 4;
    outOffset = AlignUp(mPrimitiveOffset, kAlign);
    CHECK_MSG(outOffset + size <= mPrimitiveBuffer->mDesc.size,
              "GPUScene primitive buffer overflow for serialized mesh. Need {} bytes more, have {} left",
              size, mPrimitiveBuffer->mDesc.size - outOffset);

    char *ptr = ctx->Upload(mPrimitiveBuffer.Get(), size, outOffset), *dst = ptr;
    if (ptr == nullptr)
        return 0;
    auto Write = [&](const void* pData, size_t bytes)
    {
        std::memcpy(dst, pData, bytes);
        uint32_t off = static_cast<uint32_t>(dst - ptr);
        dst += bytes;
        return off;
    };
    auto Read = [&](FBlobRef const& blob)
    {
        uint32_t off = static_cast<uint32_t>(dst - ptr);
        CHECK(scene.ReadBlob(blob, dst, static_cast<size_t>(blob.decodedSize)));
        dst += static_cast<size_t>(blob.decodedSize);
        return off;
    };

    Write(&outData, sizeof(GSMesh));
    outData.vtxCount = src.vertexCount;
    outData.vtxOffset = outOffset + Read(src.vertices);
    outData.idxCount = lod0.indexCount;
    outData.idxOffset = outOffset + Read(lod0.indices);
    outData.groupCount = src.dagGroups.count;
    outData.groupOffset = outOffset + Read(src.dagGroups);
    outData.meshletCount = src.dagMeshlets.count;
    outData.meshletOffset = outOffset + Read(src.dagMeshlets);
    outData.meshletVtxOffset = outOffset + Read(src.dagMeshletVtx);
    outData.meshletTriOffset = outOffset + Read(src.dagMeshletTri);
    outData.meshletGlobalIndex = mMeshletGlobalCounter;
    std::memcpy(ptr, &outData, sizeof(GSMesh));
    size_t written = dst - ptr;
    CHECK_MSG(written == size, "Write mismatch: expected {} got {}", size, written);
    mPrimitiveOffset = outOffset + size;
    mMeshletGlobalCounter += outData.meshletCount;
    return written;
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
    if (ctx->ptr + size > ctx->end)
        return 0;

    char* ptr = ctx->Upload(mPrimitiveBuffer.Get(), size, outOffset);
    CHECK(ptr != nullptr);
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

size_t GPUScene::Upload(ImmediateUpload* ctx, FScene const& scene, FSerializedCurve const& src,
                         GSCurveSet& outData, uint32_t& outOffset)
{
    static_assert(sizeof(FCurvePoint) == sizeof(GSCurvePoint));
    static_assert(alignof(FCurvePoint) == alignof(GSCurvePoint));

    Vector<uint32_t> curveVertexCounts = scene.ReadBlobArray<uint32_t>(src.curveVertexCounts);
    CHECK_MSG(src.points.decodedSize != 0 && src.numSegments > 0, "Curve set has no renderable segments");
    CHECK_MSG(src.points.stride == sizeof(GSCurvePoint), "Serialized curve point stride mismatch");
    CHECK_MSG(src.points.decodedSize % sizeof(GSCurvePoint) == 0, "Serialized curve point blob size mismatch");

    const size_t pointCount = static_cast<size_t>(src.points.decodedSize / sizeof(GSCurvePoint));
    const size_t size = CalculateCurvePrimitiveSize(src);
    constexpr size_t kAlign = 4;
    outOffset = AlignUp(mPrimitiveOffset, kAlign);
    CHECK_MSG(outOffset + size <= mPrimitiveBuffer->mDesc.size,
              "GPUScene primitive buffer overflow for serialized curve. Need {} bytes more, have {} left",
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
    auto Skip = [&](size_t bytes)
    {
        uint32_t off = static_cast<uint32_t>(dst - ptr);
        dst += bytes;
        return off;
    };

    Write(&outData, sizeof(GSCurveSet));
    outData.pointCount = static_cast<uint32_t>(pointCount);
    outData.pointOffset = outOffset + Skip(src.points.decodedSize);
    GSCurvePoint* pointsData = reinterpret_cast<GSCurvePoint*>(ptr + outData.pointOffset - outOffset);
    CHECK(scene.ReadBlob(src.points, pointsData, static_cast<size_t>(src.points.decodedSize)));
    Span<const GSCurvePoint> points(pointsData, pointCount);

    Vector<GSCurveSegment> segments(GLOBAL_ALLOC);
    segments.reserve(src.numSegments);
    Vector<RHIAccelerationStructureAABB> aabbs(GLOBAL_ALLOC);
    aabbs.reserve(src.numSegments);

    uint32_t pointCursor = 0;
    for (uint32_t count : curveVertexCounts)
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

size_t GPUScene::BeginUpload(ImmediateUpload* ctx, FScene const& scene, FSerializedCurve const& src,
                             GSCurveSet& outData, uint32_t& outOffset, Vector<StagedUploadJob>& outJobs)
{
    static_assert(sizeof(FCurvePoint) == sizeof(GSCurvePoint));
    static_assert(alignof(FCurvePoint) == alignof(GSCurvePoint));

    CHECK_MSG(src.points.decodedSize != 0 && src.numSegments > 0, "Curve set has no renderable segments");
    CHECK_MSG(src.points.stride == sizeof(GSCurvePoint), "Serialized curve point stride mismatch");
    CHECK_MSG(src.points.decodedSize % sizeof(GSCurvePoint) == 0, "Serialized curve point blob size mismatch");

    const size_t pointCount = static_cast<size_t>(src.points.decodedSize / sizeof(GSCurvePoint));
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
    if (ctx->ptr + size + aabbSize > ctx->end)
        return 0;

    char* ptr = ctx->Upload(mPrimitiveBuffer.Get(), size, outOffset);
    CHECK(ptr != nullptr);
    char* aabbPtr = ctx->Upload(mCurveAABBBuffer.Get(), aabbSize, aabbOffset);
    CHECK(aabbPtr != nullptr);

    char* dst = ptr;
    auto Skip = [&](size_t bytes)
    {
        uint32_t off = static_cast<uint32_t>(dst - ptr);
        dst += bytes;
        return off;
    };

    Skip(sizeof(GSCurveSet));
    outData.pointCount = static_cast<uint32_t>(pointCount);
    outData.pointOffset = outOffset + Skip(static_cast<size_t>(src.points.decodedSize));
    outData.segmentCount = src.numSegments;
    outData.segmentOffset = outOffset + Skip(sizeof(GSCurveSegment) * src.numSegments);
    outData.materialIndex = src.materialIndex;
    outData.aabbOffset = aabbOffset;

    StagedUploadJob job{};
    job.scene = &scene;
    job.kind = StagedUploadJob::Kind::SerializedCurve;
    job.ptr = ptr;
    job.size = size;
    job.curve = src;
    job.curveData = outData;
    job.curvePrimitiveOffset = outOffset;
    job.curveAABBPtr = aabbPtr;
    job.curveAABBSize = aabbSize;
    outJobs.push_back(job);

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

static uint32_t CalculateTextureImageSize(uint32_t width, uint32_t height, uint32_t depth, uint32_t mipLevels,
                                          uint32_t blockSize, uint32_t blockDim)
{
    uint32_t res = 0;
    while (mipLevels--)
    {
        res += ((width + blockDim - 1) / blockDim) * ((height + blockDim - 1) / blockDim) * depth * blockSize;
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
        depth = std::max(1u, depth / 2);
    }
    return res;
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
        uint32_t layerOffset =
            layer * CalculateTextureImageSize(metadata.GetWidth(), metadata.GetHeight(), metadata.GetDepth(),
                                              metadata.GetNumMips(), blockSize, blockDim);
        for (uint32_t mip = 0; mip < metadata.GetNumMips(); ++mip)
        {
            uint32_t mipOffset = CalculateTextureImageSize(metadata.GetWidth(), metadata.GetHeight(), metadata.GetDepth(),
                                                           mip, blockSize, blockDim);
            RHIExtent3D mipExtent = metadata.GetMipExtent(mip);
            uint32_t mipSize = CalculateTextureImageSize(mipExtent.x, mipExtent.y, mipExtent.z, 1u, blockSize, blockDim);
            uint32_t subresourceOffset = layerOffset + mipOffset;
            uint32_t subresourceEnd = subresourceOffset + mipSize;
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
    outIndex = mTexturePool.Allocate(std::move(texture), view.Release().Get());
    return written;
}

size_t GPUScene::Upload(ImmediateUpload* ctx, FScene const& scene, FSerializedTexture const& source,
                        uint32_t& outIndex, const char* debugName)
{
    CHECK_MSG(source.IsValid(), "Serialized texture is invalid");
    CHECK_MSG(source.data.codec == FBlobCodec::None, "Serialized texture data must not use LZ4 blob compression");
    CHECK_MSG(source.data.decodedSize == source.GetSize(), "Serialized texture size mismatch: descriptor {} header {}",
              source.data.decodedSize, source.GetSize());

    FTextureHeader const& metadata = static_cast<FTextureHeader const&>(source);
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
            uint64_t(layer) * CalculateTextureImageSize(metadata.GetWidth(), metadata.GetHeight(), metadata.GetDepth(),
                                                        metadata.GetNumMips(), blockSize, blockDim);
        for (uint32_t mip = 0; mip < metadata.GetNumMips(); ++mip)
        {
            uint64_t mipOffset = CalculateTextureImageSize(metadata.GetWidth(), metadata.GetHeight(), metadata.GetDepth(),
                                                           mip, blockSize, blockDim);
            RHIExtent3D mipExtent = metadata.GetMipExtent(mip);
            uint32_t mipSize = CalculateTextureImageSize(mipExtent.x, mipExtent.y, mipExtent.z, 1u, blockSize, blockDim);
            uint64_t subresourceOffset = layerOffset + mipOffset;
            uint64_t subresourceEnd = subresourceOffset + mipSize;
            CHECK_MSG(subresourceEnd <= source.data.decodedSize,
                      "Texture subresource out of range: layer {}, mip {} (size {}), data size {}",
                      layer, mip, mipSize, source.data.decodedSize);
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
            CHECK(scene.ReadBlobRange(source.data, subresourceOffset, ptr, mipSize));
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
    outIndex = mTexturePool.Allocate(std::move(texture), view.Release().Get());
    return written;
}

size_t GPUScene::BeginUpload(ImmediateUpload* ctx, FScene const& scene, FSerializedTexture const& source,
                             uint32_t& outIndex, Vector<StagedUploadJob>& outJobs, const char* debugName)
{
    CHECK_MSG(source.IsValid(), "Serialized texture is invalid");
    CHECK_MSG(source.data.codec == FBlobCodec::None, "Serialized texture data must not use LZ4 blob compression");
    CHECK_MSG(source.data.decodedSize == source.GetSize(), "Serialized texture size mismatch: descriptor {} header {}",
              source.data.decodedSize, source.GetSize());

    FTextureHeader const& metadata = static_cast<FTextureHeader const&>(source);
    uint32_t blockSize = metadata.GetBlockSize(), blockDim = 4;
    if (!blockSize)
        blockSize = metadata.GetBpp() / 8, blockDim = 1;
    CHECK_MSG(blockSize && blockDim, "Unsupported texture format {}", metadata.GetFormat());

    uint32_t alignment = std::max(metadata.GetBpp() / 8, metadata.GetBlockSize());
    char* preflight = ctx->ptr;
    for (uint32_t layer = 0; layer < metadata.GetNumLayers(); ++layer)
    {
        uint64_t layerOffset =
            uint64_t(layer) * CalculateTextureImageSize(metadata.GetWidth(), metadata.GetHeight(), metadata.GetDepth(),
                                                        metadata.GetNumMips(), blockSize, blockDim);
        for (uint32_t mip = 0; mip < metadata.GetNumMips(); ++mip)
        {
            uint64_t mipOffset = CalculateTextureImageSize(metadata.GetWidth(), metadata.GetHeight(), metadata.GetDepth(),
                                                           mip, blockSize, blockDim);
            RHIExtent3D mipExtent = metadata.GetMipExtent(mip);
            uint32_t mipSize = CalculateTextureImageSize(mipExtent.x, mipExtent.y, mipExtent.z, 1u, blockSize, blockDim);
            uint64_t subresourceOffset = layerOffset + mipOffset;
            uint64_t subresourceEnd = subresourceOffset + mipSize;
            CHECK_MSG(subresourceEnd <= source.data.decodedSize,
                      "Texture subresource out of range: layer {}, mip {} (size {}), data size {}",
                      layer, mip, mipSize, source.data.decodedSize);
            preflight = reinterpret_cast<char*>(AlignUp(reinterpret_cast<uintptr_t>(preflight), alignment));
            if (preflight >= ctx->end || static_cast<size_t>(ctx->end - preflight) < mipSize)
                return 0;
            preflight += mipSize;
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
        uint64_t layerOffset =
            uint64_t(layer) * CalculateTextureImageSize(metadata.GetWidth(), metadata.GetHeight(), metadata.GetDepth(),
                                                        metadata.GetNumMips(), blockSize, blockDim);
        for (uint32_t mip = 0; mip < metadata.GetNumMips(); ++mip)
        {
            uint64_t mipOffset = CalculateTextureImageSize(metadata.GetWidth(), metadata.GetHeight(), metadata.GetDepth(),
                                                           mip, blockSize, blockDim);
            RHIExtent3D mipExtent = metadata.GetMipExtent(mip);
            uint32_t mipSize = CalculateTextureImageSize(mipExtent.x, mipExtent.y, mipExtent.z, 1u, blockSize, blockDim);
            uint64_t subresourceOffset = layerOffset + mipOffset;
            CHECK(ctx->Align(alignment));
            char* ptr = ctx->Upload(texture.Get(), mipSize,
                                    {
                                        .aspect = RHITextureAspectFlagBits::Color,
                                        .mipLevel = mip, .baseArrayLayer = layer, .layerCount = 1
                                    },
                                    {0, 0, 0}, mipExtent);
            CHECK(ptr != nullptr);

            StagedUploadJob job{};
            job.scene = &scene;
            job.kind = StagedUploadJob::Kind::BlobRange;
            job.blob = source.data;
            job.blobOffset = subresourceOffset;
            job.ptr = ptr;
            job.size = mipSize;
            outJobs.push_back(job);
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
    ctx->Submit(), ctx->WaitIdle();
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
                         Span<const uint32_t> curveBLASIndices, Span<const GSLight> lights, bool update)
{
    uint32_t numAreaLights = 0;
    for (const auto& light : lights)
    {
        if (light.type == 3 || light.type == 4)
            numAreaLights++;
    }

    uint32_t totalInstances = static_cast<uint32_t>(instances.size()) + numAreaLights;
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

    // NOTE: Byte buffers
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
    conditionalTex.bytes.resize(conditionalSize);
    
    for (uint32_t y = 0; y < height; ++y)
    {
        std::memcpy(conditionalTex.bytes.data() + y * cdf.mConditional[0]->mCDF.size() * sizeof(float),
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

RHITexture* GPUScene::GetFoundationDefaultTexture2D() const
{
    return ResolvePoolTexture(const_cast<BindlessPool&>(mTexturePool), mFoundationDefaultTexture2DIndex);
}

RHITexture* GPUScene::GetFoundationDefaultTexture2DFloat() const
{
    return ResolvePoolTexture(const_cast<BindlessPool&>(mTexturePool), mFoundationDefaultTexture2DFloatIndex);
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
    mLightBuffer.Reset();
    mLightAliasTableBuffer.Reset();
    // NOTE: mTexturePool is append-only; old bindings become dead entries.
    //       mTLAS is kept alive and rebuilt in-place by BuildTLAS.
}