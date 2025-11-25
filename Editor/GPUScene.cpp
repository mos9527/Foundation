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
        .size = desc.instanceBudget * sizeof(GSInstance)});
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
    // LOD Group data
    mesh.groupCount = src.dag.groups.size();
    mesh.groupOffset = outOffset + Write(src.dag.groups.data(), sizeof(FLODGroup) * src.dag.groups.size());
    // Meshlet data
    mesh.meshletCount = src.dag.meshlets.size();
    mesh.meshletOffset = outOffset + Write(src.dag.meshlets.data(), sizeof(FMeshlet) * src.dag.meshlets.size());
    mesh.meshletVtxOffset = outOffset + Write(src.dag.meshletVtx.data(), sizeof(uint32_t) * src.dag.meshletVtx.size());
    mesh.meshletTriOffset = outOffset + Write(src.dag.meshletTri.data(), sizeof(uint8_t) * src.dag.meshletTri.size());
    // GSMesh (data)
    std::memcpy(ptr, &mesh, sizeof(GSMesh));
    return mStreaming.Write(data, mPrimitiveBuffer.Get(), outOffset);
}
String GPUScene::DbgGetStatistics() const
{
    return fmt::format("Pool: {}", mStreaming.DbgGetStatistics());
}
void GPUScene::Reset()
{
    mPrimitiveOffset = 0;
    mInstanceRing = mInstanceBegin;
}
