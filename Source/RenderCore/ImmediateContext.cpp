namespace Foundation::RenderCore
{
    ImmediateContext::ImmediateContext(RHIDeviceQueueType type, RHIDevice* device) : mDevice(device)
    {
        mQueue = device->GetDeviceQueue(type);
        mCommandPool = device->CreateCommandPool({.queue = mQueue, .type = RHICommandPoolType::Persistent});
        mCommandList = mCommandPool->CreateCommandList();
    }
    void ImmediateContext::Submit(RHIDeviceFence* completionFence)
    {
        mQueue->Submit({{{.cmdLists = {{mCommandList.Get()}}}}}, completionFence);
    }
    void ImmediateContext::WaitIdle() { mDevice->WaitIdle(); }
    void ImmediateUpload::Begin()
    {
        ctx->Reset();
        ctx->Begin();
        ptr = begin;
    }
    char* ImmediateUpload::Upload(RHIBuffer* dst, size_t dataSize, size_t dstOffset)
    {
        if (ptr + dataSize > end)
            return nullptr;
        ctx->CopyBuffer(
            staging.Get(), dst,
            {{{.srcOffset = static_cast<uint32_t>(ptr - begin), .dstOffset = dstOffset, .size = dataSize}}});
        char* res = ptr;
        ptr += dataSize;
        return res;
    }
    char* ImmediateUpload::Upload(RHITexture* dst, size_t dataSize, RHITextureSubresourceLayer dstLayer,
                                  RHIOffset2D dstOffset, RHIExtent2D dstExtent)
    {
        RHIExtent3D maxExtent = dst->mDesc.extent;
        RHIOffset3D offset{dstOffset.x, dstOffset.y, 0};
        RHIExtent3D extent{dstExtent.x ? dstExtent.x : maxExtent.x, dstExtent.y ? dstExtent.y : maxExtent.y, 1};
        return Upload(dst, dataSize, dstLayer, offset, extent);
    }
    char* ImmediateUpload::Upload(RHITexture* dst, size_t dataSize, RHITextureSubresourceLayer dstLayer,
                                  RHIOffset3D dstOffset, RHIExtent3D dstExtent)
    {
        if (ptr + dataSize > end)
            return nullptr;
        RHIExtent3D maxExtent = dst->mDesc.extent;
        RHIExtent3D extent{dstExtent.x ? dstExtent.x : maxExtent.x,
                           dstExtent.y ? dstExtent.y : maxExtent.y,
                           dstExtent.z ? dstExtent.z : maxExtent.z};
        ctx->CopyBufferToImage(staging.Get(), dst, RHITextureLayout::TransferDst,
                               {{{.srcBufferOffset = static_cast<uint32_t>(ptr - begin),
                                  .dstLayer = dstLayer,
                                  .dstOffset = dstOffset,
                                  .extent = extent}}});
        char* res = ptr;
        ptr += dataSize;
        return res;
    }
    bool ImmediateUpload::Align(uint32_t alignment)
    {
        auto* pup = reinterpret_cast<char*>(AlignUp(reinterpret_cast<uintptr_t>(ptr), alignment));
        if (pup >= end)
            return false;
        ptr = pup;
        return true;
    }
    void ImmediateUpload::End(RHIDeviceFence* completionFence)
    {
        ctx->End();
        ctx.Submit(completionFence);
    }
    void ImmediateUpload::WaitIdle() { ctx.WaitIdle(); }
} // namespace Foundation::RenderCore
