GPUScene::GPUScene(FContext* ctx, GPUSceneDesc const& desc) :
    mContext(ctx), mStreaming(ctx->device.Get(), ctx->allocator,
                              {
                                  .streamingPageSize = 1u << 20, // 1MB per, 128 pages max
                              })
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
        .size = desc.instanceBudget});
    mInstanceBegin = mInstanceRing = mInstanceBuffer->Map<GSInstance>();
    mInstanceEnd = mInstanceBegin + desc.instanceBudget;
}
Pair<GSInstance*, uint32_t> GPUScene::InstanceAlloc(uint32_t count)
{
    GSInstance* begin = mInstanceRing;
    if (begin + count >= mInstanceEnd) // Wrap
        begin = mInstanceRing = mInstanceBegin;
    uint32_t offset = mInstanceRing - mInstanceBegin;
    mInstanceRing += count;
    return {begin,offset};
}
SharedFuture<> GPUScene::Upload(FMesh const& src, GSMesh& mesh, uint32_t& outOffset)
{
    Vector<char> data(src.ApproximateSize() + sizeof(GSMesh), mContext->allocator);
    char* ptr = data.data(), *dst = ptr;
    auto Write = [&](const void* pData, size_t bytes)
    {
        std::memcpy(dst, pData, bytes);
        uint32_t off = dst - ptr;
        dst += bytes;
        return off;
    };
    outOffset = mPrimitiveOffset, mPrimitiveOffset += data.size();
    // GSMesh (stub)
    Write(&mesh, sizeof(GSMesh));
    // Vertex data
    mesh.vtxCount = src.vertices.size();
    mesh.vtxOffset = outOffset + Write(src.vertices.data(), sizeof(FVertex) * src.vertices.size());
    // Group data
    mesh.groupCount = src.dag.groups.size();
    mesh.groupOffset = outOffset + Write(src.dag.groups.data(), sizeof(FLODGroup) * src.dag.groups.size());
    // LOD Data
    mesh.lodCount = src.lods.size();
    for (int i = 0; auto& srcLod : src.lods)
    {
        auto& dstLod = mesh.lod[i];
        dstLod.meshletCount = srcLod.meshlets.size();
        dstLod.meshletOffset = outOffset + Write(srcLod.meshlets.data(), sizeof(FMeshlet) * srcLod.meshlets.size());
        dstLod.meshletVtxOffset = outOffset + Write(srcLod.meshletVtx.data(), sizeof(uint32_t) * srcLod.meshletVtx.size());
        dstLod.meshletTriOffset = outOffset + Write(srcLod.meshletTri.data(), sizeof(uint8_t) * srcLod.meshletTri.size());
    }
    // GSMesh (data)
    std::memcpy(ptr, &mesh, sizeof(GSMesh));
    return mStreaming.Write(data, mPrimitiveBuffer.Get(), outOffset);
}
String GPUScene::DbgGetStatistics() const
{
    return fmt::format("Pool: {}", mStreaming.DbgGetStatistics());
}
