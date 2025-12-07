GPUScene::GPUScene(FContext* ctx, GPUSceneDesc const& desc) :
    mContext(ctx), mInstanceBuffer(ctx->device.Get(), desc.instanceBudget),
mMaterialBuffer(ctx->device.Get(), desc.materialBudget), mTexturePool(ctx->device.Get(), ctx->allocator,
    { .maxBindings = desc.texturesBudget })
{
    mPrimitiveBuffer = mContext->device->CreateBuffer(
    {
            .resource = {
                .heap = RHIDeviceHeapType::Local,
                .shared = true
            },
         .usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::StorageBuffer,
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
    const size_t size = sizeof(GSMesh) + src.CalculateQuantizedBound(false, true);
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
void GPUScene::Reset()
{
    mPrimitiveOffset = 0;
    mInstanceBuffer.Reset();
}
