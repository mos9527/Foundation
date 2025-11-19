GPUScene::GPUScene(FEditorContext* ctx, GPUSceneDesc const& desc) :
    mContext(ctx), mStreaming(ctx->device.Get(), ctx->allocator,
                              {
                                  .streamingPageSize = 1u << 20, // 1MB, 128 pages max
                              }), mTasksMeshUpload(ctx->allocator)
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
    mInstanceData = mInstanceBuffer->MapSpan<char>();
}
SharedFuture<> GPUScene::UploadMeshAsync(uint32_t& outOffset, GSMesh& outData, UploadMeshData const& source)
{

}
PassHandle GPUScene::CreatePass(Renderer* r)
{

}
