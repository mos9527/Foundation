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
        Submit(ImmediateSubmitDesc{.completionFence = completionFence});
    }
    void ImmediateContext::Submit(ImmediateSubmitDesc const& desc)
    {
        RHICommandList* cmd = mCommandList.Get();
        mQueue->Submit({{{.timelineWaits = desc.timelineWaits,
                          .timelineSignals = desc.timelineSignals,
                          .waitsStages = desc.waitStages,
                          .cmdLists = {&cmd, 1}}}},
                       desc.completionFence);
    }
    void ImmediateContext::WaitIdle() { mQueue->WaitIdle(); }
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
        End(ImmediateSubmitDesc{.completionFence = completionFence});
    }
    void ImmediateUpload::End(ImmediateSubmitDesc const& desc)
    {
        ctx->End();
        ctx.Submit(desc);
    }
    void ImmediateUpload::WaitIdle() { ctx.WaitIdle(); }
    void ImmediateReadback::Begin()
    {
        ctx->Reset();
        ctx->Begin();
        ptr = begin;
    }
    char* ImmediateReadback::Readback(RHIBuffer* src, size_t dataSize, size_t srcOffset)
    {
        if (ptr + dataSize > end)
            return nullptr;
        ctx->CopyBuffer(
            src, staging.Get(),
            {{{.srcOffset = srcOffset, .dstOffset = static_cast<uint32_t>(ptr - begin), .size = dataSize}}});
        char* res = ptr;
        ptr += dataSize;
        return res;
    }
    char* ImmediateReadback::Readback(RHITexture* src, size_t dataSize, RHITextureSubresourceLayer srcLayer,
                                      RHIOffset2D srcOffset, RHIExtent2D srcExtent)
    {
        RHIExtent3D maxExtent = src->mDesc.extent;
        RHIOffset3D offset{srcOffset.x, srcOffset.y, 0};
        RHIExtent3D extent{srcExtent.x ? srcExtent.x : maxExtent.x, srcExtent.y ? srcExtent.y : maxExtent.y, 1};
        return Readback(src, dataSize, srcLayer, offset, extent);
    }
    char* ImmediateReadback::Readback(RHITexture* src, size_t dataSize, RHITextureSubresourceLayer srcLayer,
                                      RHIOffset3D srcOffset, RHIExtent3D srcExtent)
    {
        if (ptr + dataSize > end)
            return nullptr;
        RHIExtent3D maxExtent = src->mDesc.extent;
        RHIExtent3D extent{srcExtent.x ? srcExtent.x : maxExtent.x,
                           srcExtent.y ? srcExtent.y : maxExtent.y,
                           srcExtent.z ? srcExtent.z : maxExtent.z};
        ctx->CopyImageToBuffer(src, RHITextureLayout::TransferSrc, staging.Get(),
                               {{{.dstBufferOffset = static_cast<uint32_t>(ptr - begin),
                                  .srcLayer = srcLayer,
                                  .srcOffset = srcOffset,
                                  .extent = extent}}});
        char* res = ptr;
        ptr += dataSize;
        return res;
    }
    bool ImmediateReadback::Align(uint32_t alignment)
    {
        auto* pup = reinterpret_cast<char*>(AlignUp(reinterpret_cast<uintptr_t>(ptr), alignment));
        if (pup >= end)
            return false;
        ptr = pup;
        return true;
    }
    void ImmediateReadback::End(RHIDeviceFence* completionFence)
    {
        End(ImmediateSubmitDesc{.completionFence = completionFence});
    }
    void ImmediateReadback::End(ImmediateSubmitDesc const& desc)
    {
        ctx->End();
        ctx.Submit(desc);
    }
    void ImmediateReadback::WaitIdle() { ctx.WaitIdle(); }
} // namespace Foundation::RenderCore
