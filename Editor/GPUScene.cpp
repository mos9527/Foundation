#include "Tables.hpp"
template <typename T>
RHIDeviceScopedHandle<RHITexture> UploadLUT(const float* data, RHIResourceFormat format, uint32_t width, uint32_t height,
                                            FContext* ctx)
{
    auto tex = ctx->device->CreateTexture(RHITextureDesc{
        .resource = {
            .heap = RHIDeviceHeapType::Local,
            .shared = false
        },
        .dimension = RHITextureDimension::E2D,
        .usage = RHITextureUsageBits::SampledImage | RHITextureUsageBits::TransferDestination,
        .extent = {width, height, 1},
        .format = format,
    });
    const uint32_t size = width * height * sizeof(T);
    ImmediateUpload upload(ctx->device.Get(), size);
    upload.Begin();
    upload.ctx->BeginTransition();
    upload.ctx->SetImageTransition(tex.Get(), {
        .dstAccess = RHIResourceAccessBits::TransferWrite,
        .dstStage = RHIPipelineStageBits::Transfer,
        .dstImgLayout =  RHITextureLayout::TransferDst,
        .srcImgRange = RHITextureSubresourceRange::Create()
    });
    upload.ctx->EndTransition();
    char* ptr = upload.Upload(tex.Get(), size);
    CHECK(ptr);
    std::memcpy(ptr, data, size);
    upload.ctx->BeginTransition();
    upload.ctx->SetImageTransition(tex.Get(), {
        .dstAccess = RHIResourceAccessBits::ShaderRead,
        .dstStage = RHIPipelineStageBits::AllGraphics,
        .dstImgLayout =  RHITextureLayout::ShaderReadOnly,
        .srcImgRange = RHITextureSubresourceRange::Create()
    });
    upload.ctx->EndTransition();
    upload.End();
    upload.WaitIdle();
    return tex;
}

GPUScene::GPUScene(FContext* ctx, GPUSceneDesc const& desc) :
    mContext(ctx), mInstanceBuffer(ctx->device.Get(), desc.instanceBudget),
    mMaterialBuffer(ctx->device.Get(), desc.materialBudget),
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
    mGGXlutE = UploadLUT<float2>(kGGXlutE, RHIResourceFormat::R32G32SignedFloat, 32, 32, mContext);
    mGGXlutEavg = UploadLUT<float>(kGGXlutEavg, RHIResourceFormat::R32SignedFloat, 32, 1, mContext);
}

Pair<GSInstance*, uint32_t> GPUScene::AllocateInstance(uint32_t count)
{
    return mInstanceBuffer.Allocate(count);
}

Pair<GSMaterial*, uint32_t> GPUScene::AllocateMaterial(uint32_t count)
{
    return mMaterialBuffer.Allocate(count);
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
        sizeInfo[i] = device->GetAccelerationStructureSizeInfo(desc);
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

void GPUScene::BuildTLAS(RHICommandList* cmd, Span<const GSInstance> instances, Span<const uint32_t> blasIndices,
                         bool update)
{
    auto* device = mContext->device.Get();
    auto ConvertInstance = [&](const GSInstance* src) -> RHIAccelerationStructureGeometryInstance
    {
        RHIAccelerationStructureGeometryInstance res{
            .instanceID = static_cast<uint32_t>(src - instances.data()),
            .mask = 0xFF,
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
    auto [pInstances, instancesOffset] = mTLASInstances.Allocate(mTLASInstanceStride * instances.size());
    for (const auto & instance : instances)
    {
        auto data = ConvertInstance(&instance);
        data.blas = mBLASes[blasIndices[instance.meshIndex]].Get();
        pInstances += mContext->device->WriteAccelerationStructureInstanceData(data, pInstances);
    }
    RHIAccelerationStructureGeometryInstanceData instance{
        .instanceBuffer = mTLASInstances.mBuffer.Get(),
        .instanceOffset = instancesOffset,
        .totalPrimitives = static_cast<uint32_t>(instances.size())
    };
    RHIAccelerationStructureGeometryInfo geometry{
        .type = RHIAccelerationGeometryType::Instances,
        .instanceData = instance
    };
    RHIAccelerationStructureBuildRangeInfo range{.primitiveCount = static_cast<uint32_t>(instances.size())
    };
    RHIAccelerationStructureBuildDesc desc{
        .type = RHIAccelerationStructureType::TopLevel,
        .flags = RHIAccelerationStructureBuildFlagsBits::PreferFastTrace |
        RHIAccelerationStructureBuildFlagsBits::AllowUpdate | RHIAccelerationStructureBuildFlagsBits::AllowCompaction,
        .operation = update ? RHIAccelerationStructureBuildOp::Update : RHIAccelerationStructureBuildOp::Build,
        .geometries = Span<const RHIAccelerationStructureGeometryInfo>{&geometry, 1},
        .ranges = Span<const RHIAccelerationStructureBuildRangeInfo>{&range, 1}
    };
    auto size = device->GetAccelerationStructureSizeInfo(desc);
    CHECK_MSG(size.accelerationStructureSize <= mTLASBuffer->mDesc.size, "TLAS buffer overflow");
    desc.scratchBuffer = mScratchBufferTLAS.Get();
    desc.scratchBufferOffset = 0;
    desc.srcAS = desc.dstAS = mTLAS.Get();
    cmd->BuildAccelerationStructure({{{desc}}});
}

void GPUScene::Reset()
{
    mPrimitiveOffset = 0;
    mTLAS.Reset();
    mBLASes.clear();
    mBLASBuffers.clear();
    mMaterialBuffer.Reset();
    mInstanceBuffer.Reset();
}
