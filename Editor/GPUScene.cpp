GPUScene::GPUScene(FContext* ctx, GPUSceneDesc const& desc) :
    mContext(ctx), mInstanceBuffer(ctx->device.Get(), desc.instanceBudget), mMaterialBuffer(ctx->device.Get(), desc.materialBudget),
mTexturePool(ctx->device.Get(), ctx->allocator, {.maxBindings = desc.texturesBudget}), mBLASes(ctx->allocator)
{
    mPrimitiveBuffer = mContext->device->CreateBuffer(
    {
            .resource = {
                .heap = RHIDeviceHeapType::Local,
                .shared = true
            },
         .usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress | RHIBufferUsageBits::AccelerationStructureBuildReadOnly,
         .size = desc.primitiveBudget});
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
    fmt::format_to(std::back_inserter(res), "Primitive Buffer: Used {} / {} MB\n", mPrimitiveOffset / static_cast<float>(1<<20u),
                   mPrimitiveBuffer->mDesc.size / static_cast<float>(1<<20u));
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
    char* ptr = ctx->Upload(mPrimitiveBuffer.Get(), size, outOffset), *dst = ptr;
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
    outData.vtxOffset = outOffset + Write(src.verticesQuantized.data(), sizeof(FQVertex) * src.verticesQuantized.size());
    outData.idxCount = src.lods[0].indices.size();
    outData.idxOffset = outOffset + Write(src.lods[0].indices.data(), sizeof(uint32_t) * src.lods[0].indices.size());
    // LOD Group data
    outData.groupCount = src.dag.groups.size();
    outData.groupOffset = outOffset + Write(src.dag.groups.data(), sizeof(FLODGroup) * src.dag.groups.size());
    // Meshlet data
    outData.meshletCount = src.dag.meshlets.size();
    outData.meshletOffset = outOffset + Write(src.dag.meshlets.data(), sizeof(FMeshlet) * src.dag.meshlets.size());
    outData.meshletVtxOffset = outOffset + Write(src.dag.meshletVtx.data(), sizeof(uint32_t) * src.dag.meshletVtx.size());
    outData.meshletTriOffset = outOffset + Write(src.dag.meshletTri.data(), sizeof(uint8_t) * src.dag.meshletTri.size());
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
            if (!ctx->Align(std::max(source.GetBpp() / 8,source.GetBlockSize())))
                return 0;
            char* ptr = ctx->Upload(texture.Get(), subresource.size_bytes(),
                {
                    .aspect = RHITextureAspectFlagBits::Color,
                    .mipLevel = mip, .baseArrayLayer = layer, .layerCount = 1
                },
                { 0,0 }, { std::max(1u, source.GetWidth() >> mip),
                    std::max(1u, source.GetHeight() >> mip) });
            if (ptr == nullptr) // XXX: Pessimistic, we can't resume from a partial upload this way. Though
                return 0;       // overhead should be minimal for most textures.
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
RHIDeviceScopedHandle<RHIBuffer> GPUScene::CreateScratchBuffer(size_t size)
{
    return mContext->device->CreateBuffer(
    {
            .resource = {
                .heap = RHIDeviceHeapType::Local,
                .shared = false
            },
         .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress,
         .size = size,
         .alignment = 256 // Aligned to spec. See also BuildBLASIncremental
    });
}
RHIDeviceScopedHandle<RHIBuffer> GPUScene::CreateASBuffer(size_t size)
{
    return mContext->device->CreateBuffer(
{
        .resource = {
            .heap = RHIDeviceHeapType::Local,
            .shared = false
        },
     .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::DeviceAddress | RHIBufferUsageBits::AccelerationStructureStorage,
     .size = size
    });
}
// Reference:
// - https://github.com/zeux/niagara/blob/master/src/scenert.cpp
// - https://docs.vulkan.org/tutorial/latest/courses/18_Ray_tracing/02_Acceleration_structures.html
void GPUScene::BuildBLASIncremental(ImmediateContext* ctx, Span<const GSMesh> meshes, Span<uint32_t> outBLASIndices, uint32_t& outPrimitiveCount)
{
    CHECK_MSG(meshes.size() == outBLASIndices.size(), "Mismatched BLAS indices size");
    auto* device = mContext->device.Get();
    Vector<RHIAccelerationStructureGeometryInfo> geometries(meshes.size(), mContext->allocator);
    Vector<RHIAccelerationStructureBuildRangeInfo> buildRanges(meshes.size(), mContext->allocator);
    Vector<RHIAccelerationStructureBuildDesc> buildDesc(meshes.size(), mContext->allocator);
    Vector<RHIAccelerationStructureSizeInfo> sizeInfo(meshes.size(), mContext->allocator);
    Vector<uint32_t> blasOffsets(meshes.size(), mContext->allocator);
    auto* primitiveBuffer = mPrimitiveBuffer.Get();
    uint32_t scratchFootprint = 0;
    outPrimitiveCount = 0;
    for (size_t i = 0; i < meshes.size(); i++){
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
        outPrimitiveCount += range.primitiveCount;
        auto& desc = buildDesc[i];
        desc = RHIAccelerationStructureBuildDesc{
            .type = RHIAccelerationStructureType::BottomLevel,
            .operation = RHIAccelerationStructureBuildOp::Build,
            .geometries = Span<const RHIAccelerationStructureGeometryInfo>{&geo, 1},
            .ranges = Span<const RHIAccelerationStructureBuildRangeInfo>{&range, 1}
        };
        sizeInfo[i] = device->GetAccelerationStructureSizeInfo(desc);
        blasOffsets[i] = mBLASOffset;
        // minAccelerationStructureScratchOffsetAlignment is 256
        mBLASOffset = AlignUp(mBLASOffset + sizeInfo[i].accelerationStructureSize, 256u);
        scratchFootprint = std::max(scratchFootprint, sizeInfo[i].buildScratchSize);
    }
    auto scratch = CreateScratchBuffer(scratchFootprint);
    mBLASBuffer = CreateASBuffer(mBLASOffset);
    auto* cmd = ctx->Get();
    cmd->Begin();
    for (size_t i = 0; i < meshes.size(); i++)
    {
        RHIAccelerationStructureDesc as{
            .type = RHIAccelerationStructureType::BottomLevel,
            .buffer = mBLASBuffer.Get(),
            .offset = blasOffsets[i],
            .size = sizeInfo[i].accelerationStructureSize
        };
        outBLASIndices[i] = mBLASes.size();
        auto& blas = mBLASes.emplace_back(device->CreateAccelerationStructure(as));
        auto& desc = buildDesc[i];
        cmd->BeginTransition();
        cmd->SetBufferTransition(scratch.Get(), {
            .srcStage = RHIPipelineStageBits::AccelerationBuild,
            .dstStage = RHIPipelineStageBits::AccelerationBuild
        });
        cmd->EndTransition();
        desc.scratchBuffer = scratch.Get();
        desc.scratchBufferOffset = 0;
        desc.dstAS = blas.Get();
        cmd->BuildAccelerationStructure({{{desc}}});
    }
    cmd->End();
    ctx->Submit(), ctx->WaitIdle();
}
void GPUScene::BuildTLAS(ImmediateContext* ctx, Span<const GSInstance> instances, Span<const uint32_t> blasIndices, uint32_t primitiveCount)
{
    CHECK_MSG(instances.size() == blasIndices.size(), "Mismatched BLAS indices size");
    auto* device = mContext->device.Get();
    auto ConvertInstance = [&](GSInstance* src) -> vk::AccelerationStructureInstanceKHR
    {
        vk::AccelerationStructureInstanceKHR res{
            .instanceCustomIndex = static_cast<uint32_t>(src - instances.data()),
            .flags = 0xFF,
        };
        mat3 basis = transpose(mat3(scale(src->scale)) * mat3_cast(src->rotation));
        std::memcpy(res.transform.matrix[0], &basis[0], sizeof(float) * 3);
        std::memcpy(res.transform.matrix[1], &basis[1], sizeof(float) * 3);
        std::memcpy(res.transform.matrix[2], &basis[2], sizeof(float) * 3);
        res.transform.matrix[0][3] = src->transform.x;
        res.transform.matrix[1][3] = src->transform.y;
        res.transform.matrix[2][3] = src->transform.z;
        return res;
    };
    auto instanceData = mContext->device->CreateBuffer(
    {
            .resource = {
                .heap = RHIDeviceHeapType::Upload,
                .staging = true
            },
         .usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::TransferSource |
                RHIBufferUsageBits::DeviceAddress | RHIBufferUsageBits::AccelerationStructureBuildReadOnly,
        .size = instances.size() * sizeof(vk::AccelerationStructureInstanceKHR)
    });
    auto* vkInstances = instanceData->Map<vk::AccelerationStructureInstanceKHR>();
    for (size_t i = 0; i < instances.size(); i++)
    {
        vkInstances[i] = ConvertInstance(const_cast<GSInstance*>(&instances[i]));
        auto blas = static_cast<VulkanAccelerationStructure*>(mBLASes[blasIndices[i]].Get());
        vkInstances[i].accelerationStructureReference = blas->GetVkAcceleartionStructureAddress();
    }
    instanceData->Flush();
    RHIAccelerationStructureGeometryInstanceData instance{
        .instanceBuffer = instanceData.Get(),
        .totalPrimitives = primitiveCount
    };
    RHIAccelerationStructureGeometryInfo geometry{
        .type = RHIAccelerationGeometryType::Instances,
        .instanceData = instance
    };
    RHIAccelerationStructureBuildRangeInfo range{
        .primitiveCount = primitiveCount
    };
    RHIAccelerationStructureBuildDesc desc{
        .type = RHIAccelerationStructureType::TopLevel,
        .operation = RHIAccelerationStructureBuildOp::Build,
        .geometries = Span<const RHIAccelerationStructureGeometryInfo>{&geometry, 1},
        .ranges = Span<const RHIAccelerationStructureBuildRangeInfo>{&range, 1}
    };
    auto size = device->GetAccelerationStructureSizeInfo(desc);
    auto scratch = CreateScratchBuffer(size.buildScratchSize);
    mTLASBuffer = CreateASBuffer(size.accelerationStructureSize);
    RHIAccelerationStructureDesc tlasDesc{
        .type = RHIAccelerationStructureType::TopLevel,
        .buffer = mTLASBuffer.Get(),
        .size = size.accelerationStructureSize
    };
    mTLAS = device->CreateAccelerationStructure(tlasDesc);
    desc.srcAS = desc.dstAS = mTLAS.Get();
    desc.scratchBuffer = scratch.Get();
    auto* cmd = ctx->Get();
    cmd->Begin();
    cmd->BuildAccelerationStructure({{{desc}}});
    cmd->End();
    ctx->Submit(), ctx->WaitIdle();
}
void GPUScene::Reset()
{
    mPrimitiveOffset = 0;
    mBLASOffset = 0;
    mTLAS.Reset();
    mBLASes.clear();
    mMaterialBuffer.Reset();
    mInstanceBuffer.Reset();
}
