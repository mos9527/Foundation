#include "../Render/Precompute.hpp"
#include "../Render/Tables.hpp"
#include <Core/AllocatorStack.hpp>
#include <Math/Quantize.hpp>
static FTexture2D MakeLUT(const float* data, RHIResourceFormat format, uint32_t width, uint32_t height)
{
    FTexture2D tex(GLOBAL_ALLOC);
    ddsCreateHeader(tex.header, width, height, 1);
    ddsSetFormat(tex.header, tex.header10, 1, format);
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    tex.data.assign(bytes, bytes + tex.GetSize());
    return tex;
}

GPUScene::GPUScene(FContext* ctx, GPUSceneDesc const& desc) :
    mContext(ctx), mInstanceBuffer(ctx->device.Get(), desc.instanceBudget),
    mMaterialBuffer(ctx->device.Get(), desc.materialBudget),
    mLightBuffer(ctx->device.Get(), desc.lightBudget),
    mLightAliasTableBuffer(ctx->device.Get(), desc.lightBudget),
    mTexturePool(ctx->device.Get(), ctx->allocator, {.maxBindings = desc.texturesBudget}), mBLASes(ctx->allocator),
    mBLASBuffers(ctx->allocator),
    mTLASInstanceStride(mContext->device->WriteAccelerationStructureInstanceData({}, nullptr)),
    mTLASInstances(ctx->device.Get(), desc.instanceBudget * mTLASInstanceStride)
{
    mPrimitiveBuffer = mContext->device->CreateBuffer(
    {.resource = {.heap = RHIDeviceHeapType::Local, .shared = true},
     .usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::StorageBuffer |
     RHIBufferUsageBits::DeviceAddress | RHIBufferUsageBits::AccelerationStructureBuildReadOnly,
     .size = desc.primitiveBudget});
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

    // Initialize Light BLAS
    {
        struct LightGeo {
            uint16_t vertices[4 + 33][4];
            uint32_t indices[6 + 32 * 3];
        } geo;
        
        auto SetVertex = [&](int idx, float x, float y, float z) {
            geo.vertices[idx][0] = Math::quantizeFP16(x);
            geo.vertices[idx][1] = Math::quantizeFP16(y);
            geo.vertices[idx][2] = Math::quantizeFP16(z);
            geo.vertices[idx][3] = Math::quantizeFP16(1.0f);
        };

        // Rect
        SetVertex(0, -1, -1, 0);
        SetVertex(1,  1, -1, 0);
        SetVertex(2,  1,  1, 0);
        SetVertex(3, -1,  1, 0);
        geo.indices[0] = 0; geo.indices[1] = 1; geo.indices[2] = 2;
        geo.indices[3] = 0; geo.indices[4] = 2; geo.indices[5] = 3;
        
        // Disk
        SetVertex(4, 0, 0, 0);
        for (int i = 0; i < 32; ++i) {
            float theta = i * 2.0f * pi<float>() / 32.0f;
            SetVertex(5 + i, std::cos(theta), std::sin(theta), 0);
            geo.indices[6 + i * 3 + 0] = 4;
            geo.indices[6 + i * 3 + 1] = 5 + i;
            geo.indices[6 + i * 3 + 2] = 5 + (i + 1) % 32;
        }

        mLightGeometryBuffer = mContext->device->CreateBuffer({
            .resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
            .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::DeviceAddress | RHIBufferUsageBits::AccelerationStructureBuildReadOnly,
            .size = sizeof(LightGeo)
        });

        ImmediateUpload upload(mContext->device.Get(), sizeof(LightGeo));
        upload.Begin();
        char* ptr = upload.Upload(mLightGeometryBuffer.Get(), sizeof(LightGeo), 0);
        std::memcpy(ptr, &geo, sizeof(LightGeo));
        upload.End();
        upload.WaitIdle();

        // Build BLAS
        RHIAccelerationStructureGeometryInfo rectGeoInfo{
            .type = RHIAccelerationGeometryType::Triangles,
            .triangleData = {
                .vertexFormat = RHIResourceFormat::R16G16B16A16SignedFloat,
                .vertexBuffer = mLightGeometryBuffer.Get(),
                .vertexOffset = offsetof(LightGeo, vertices),
                .vertexCount = 4,
                .vertexStride = sizeof(uint16_t) * 4,
                .indexFormat = RHIResourceFormat::R32Uint,
                .indexBuffer = mLightGeometryBuffer.Get(),
                .indexOffset = offsetof(LightGeo, indices),
                .indexCount = 6
            }
        };
        RHIAccelerationStructureBuildRangeInfo rectRange{.primitiveCount = 2};

        RHIAccelerationStructureGeometryInfo diskGeoInfo{
            .type = RHIAccelerationGeometryType::Triangles,
            .triangleData = {
                .vertexFormat = RHIResourceFormat::R16G16B16A16SignedFloat,
                .vertexBuffer = mLightGeometryBuffer.Get(),
                .vertexOffset = offsetof(LightGeo, vertices) + 4 * sizeof(uint16_t) * 4,
                .vertexCount = 33,
                .vertexStride = sizeof(uint16_t) * 4,
                .indexFormat = RHIResourceFormat::R32Uint,
                .indexBuffer = mLightGeometryBuffer.Get(),
                .indexOffset = offsetof(LightGeo, indices) + 6 * sizeof(uint32_t),
                .indexCount = 32 * 3
            }
        };
        RHIAccelerationStructureBuildRangeInfo diskRange{.primitiveCount = 32};

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

    {
        auto lutE = MakeLUT(kGGXlutE, RHIResourceFormat::R32G32SignedFloat, 32, 32);
        auto lutEavg = MakeLUT(kGGXlutEavg, RHIResourceFormat::R32SignedFloat, 32, 1);
        const size_t budget = lutE.GetSize() + lutEavg.GetSize() + sizeof(kSobolMatrices32);
        ImmediateUpload upload(mContext->device.Get(), budget);
        upload.Begin();
        Upload(&upload, lutE, mGGXlutEIndex);
        Upload(&upload, lutEavg, mGGXlutEavgIndex);
        char* ptr = upload.Upload(mSobolMatricesBuffer.Get(), sizeof(kSobolMatrices32), 0);
        std::memcpy(ptr, kSobolMatrices32, sizeof(kSobolMatrices32));
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

String GPUScene::DbgGetBufferStatistics() const
{
    String res;
    fmt::format_to(std::back_inserter(res), "Primitive Buffer: Used {} / {} MB\n",
                   mPrimitiveOffset / static_cast<float>(1 << 20u),
                   mPrimitiveBuffer->mDesc.size / static_cast<float>(1 << 20u));
    fmt::format_to(std::back_inserter(res), "Instance Buffer: Used {} / {} instances",
                   mInstanceBuffer.Used(), mInstanceBuffer.Capacity());
    return res;
}

size_t GPUScene::Upload(ImmediateUpload* ctx, FMesh const& src, GSMesh& outData, uint32_t& outOffset)
{
    // Only upload DAG data
    const size_t size = sizeof(GSMesh) + src.CalculateQuantizedBound(true, true);
    // We need to ensure the *worst* alignment case fits per DXC docs
    // https://github.com/microsoft/DirectXShaderCompiler/wiki/ByteAddressBuffer-Load-Store-Additions
    // We can consider the GSMesh, FVertex, etc. as one struct - aligning to its largest member
    // uint32_t, in this case - would be sufficient.
    constexpr size_t kAlign = 4;
    outOffset = AlignUp(mPrimitiveOffset, kAlign);
    CHECK_MSG(outOffset + size <= mPrimitiveBuffer->mDesc.size, "GPUScene primitive buffer overflow");
    mPrimitiveOffset = outOffset + size;
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
    mMeshletGlobalCounter += outData.meshletCount;
    // GSMesh (data)
    std::memcpy(ptr, &outData, sizeof(GSMesh));
    size_t written = dst - ptr;
    CHECK_MSG(written == size, "Write mismatch: expected {} got {}", size, written);
    return dst - ptr;
}

size_t GPUScene::Upload(ImmediateUpload* ctx, FTexture2D const& source, uint32_t& outIndex)
{
    auto texture = mContext->device->CreateTexture(source.GetDesc());
    size_t written = 0;
    auto range = RHITextureSubresourceRange::Create(
        RHITextureAspectFlagBits::Color,
        0, source.GetNumMips(),
        0, source.GetNumLayers());
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
                                    {0, 0}, {std::max(1u, source.GetWidth() >> mip),
                                             std::max(1u, source.GetHeight() >> mip)});
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
    outIndex = mTexturePool.Allocate(std::move(texture), texture->CreateTextureView({
                                         .format = source.GetFormat(),
                                         .dimension = RHITextureDimension::E2D,
                                         .range = RHITextureSubresourceRange::Create(
                                             RHITextureAspectFlagBits::Color,
                                             0, source.GetNumMips(),
                                             0, source.GetNumLayers())
                                     }).Release().Get());
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

void GPUScene::BuildTLAS(RHICommandList* cmd, Span<const GSInstance> instances, Span<const uint32_t> blasIndices, Span<const GSLight> lights,
                         bool update)
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
        RHIAccelerationStructureBuildFlagsBits::AllowUpdate;
    if (!update)
        buildFlags |= RHIAccelerationStructureBuildFlagsBits::AllowCompaction;
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

void GPUScene::UploadEnvMap(ImmediateUpload* ctx, FTexture2D const& source)
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
            float luminance = 0.2126f * pixel.x + 0.7152f * pixel.y + 0.0722f * pixel.z;
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
    FTexture2D conditionalTex(mContext->allocator);
    ddsCreateHeader(conditionalTex.header, cdf.mConditional[0]->mCDF.size(), height, 1);
    ddsSetFormat(conditionalTex.header, conditionalTex.header10, 1, RHIResourceFormat::R32SignedFloat);
    
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
    return ResolvePoolTexture(const_cast<BindlessPool&>(mTexturePool), mGGXlutEIndex);
}

RHITexture* GPUScene::GetGGXlutEavg() const
{
    return ResolvePoolTexture(const_cast<BindlessPool&>(mTexturePool), mGGXlutEavgIndex);
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
    mMeshletGlobalCounter = 0;
    mLastTLASInstancesCount = 0;
    mBLASes.clear();
    mBLASBuffers.clear();
    mMaterialBuffer.Reset();
    mInstanceBuffer.Reset();
    mLightBuffer.Reset();
    mLightAliasTableBuffer.Reset();
    // NOTE: mTexturePool is append-only; old bindings become dead entries.
    //       mTLAS is kept alive and rebuilt in-place by BuildTLAS.
}