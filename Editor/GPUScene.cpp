GPUScene::GPUScene(FContext* ctx, GPUSceneDesc const& desc) :
    mContext(ctx)
{
    mPrimitiveBuffer = mContext->device->CreateBuffer(
    {
            .resource = {
                .heap = RHIDeviceHeapType::Local,
                .shared = true
            },
         .usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::StorageBuffer,
         .size = desc.primitiveBudget});
    mInstanceBuffer = mContext->device->CreateBuffer(
        {
            .resource = {
                .heap = RHIDeviceHeapType::Upload,
                .hostAccess = RHIResourceHostAccess::WriteOnly,
                .coherent = true
            },
        .usage = RHIBufferUsageBits::StorageBuffer,
        .size = desc.instanceBudget * sizeof(GSInstance)});
    mInstanceBegin = mInstanceRing = mInstanceRingPrev = mInstanceBuffer->Map<GSInstance>();
    mInstanceEnd = mInstanceBegin + desc.instanceBudget;
}
Pair<GSInstance*, uint32_t> GPUScene::InstanceAlloc(uint32_t count)
{
    GSInstance* begin = mInstanceRing;
    if (begin + count >= mInstanceEnd) // Wrap
        begin = mInstanceRing = mInstanceBegin;
    uint32_t offset = mInstanceRing - mInstanceBegin;
    mInstanceRingPrev = mInstanceRing, mInstanceRing += count;
    return {begin,offset};
}
String GPUScene::DbgGetBufferStatistics() const
{
    String res;
    fmt::format_to(std::back_inserter(res), "Primitive Buffer: Used {} / {} MB\n", mPrimitiveOffset / 1e6f,
                   mPrimitiveBuffer->mDesc.size / 1e6f);
    fmt::format_to(std::back_inserter(res), "Instance Buffer: Used {} / {} instances",
                   mInstanceRing - mInstanceRingPrev, mInstanceEnd - mInstanceBegin);
    return res;
}
size_t GPUScene::Upload(ImmediateUpload* ctx, FMesh const& src, GSMesh& outData, uint32_t& outOffset)
{
    const size_t size = src.ApproximateSizeQuantized();
    // We need to ensure the *worst* alignment case fits per DXC docs
    // https://github.com/microsoft/DirectXShaderCompiler/wiki/ByteAddressBuffer-Load-Store-Additions
    // We can consider the GSMesh, FVertex, etc. as one struct - aligning to its largest member
    // uint32_t, in this case - would be sufficient.
    constexpr size_t kAlign = 4;
    outOffset = AlignUp(mPrimitiveOffset, kAlign), mPrimitiveOffset += size;
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
    CHECK_MSG(mPrimitiveOffset < mPrimitiveBuffer->mDesc.size, "GPUScene primitive buffer overflow");
    // GSMesh (stub)
    Write(&outData, sizeof(GSMesh));
    // Vertex data
    outData.vtxCount = src.quantizedVertices.size();
    outData.vtxOffset = outOffset + Write(src.quantizedVertices.data(), sizeof(FVertex) * src.quantizedVertices.size());
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
    return dst - ptr;
}
void GPUScene::Reset()
{
    mPrimitiveOffset = 0;
    mInstanceRing = mInstanceBegin;
}
