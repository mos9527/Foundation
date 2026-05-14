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

    ImmediateUpload::UploadLane::UploadLane(RHIDevice* device, size_t capacity, RHIDeviceQueueType type) :
        ctx(type, device),
        staging(device->CreateBuffer({.resource =
                                          {
                                              .heap = RHIDeviceHeapType::Upload,
                                              .shared = false, /* Transfer only */
                                              .coherent = true, /* No flush required */
                                              .staging = true,
                                          },
                                      .usage = RHIBufferUsageBits::TransferSource,
                                      .size = capacity}))
    {
        begin = staging->Map<char>();
        end = begin + capacity;
    }

    ImmediateUpload::ImmediateUpload(RHIDevice* device, size_t capacity, RHIDeviceQueueType type, size_t buffers) :
        ctx(type, device),
        staging(device->CreateBuffer({.resource =
                                          {
                                              .heap = RHIDeviceHeapType::Upload,
                                              .shared = false, /* Transfer only */
                                              .coherent = true, /* No flush required */
                                              .staging = true,
                                          },
                                      .usage = RHIBufferUsageBits::TransferSource,
                                      .size = capacity})),
        mDevice(device),
        mLaneCount(std::max<size_t>(buffers, 1u)),
        mLanes(GLOBAL_ALLOC),
        mSubmitSignals(GLOBAL_ALLOC)
    {
        mLane0Begin = staging->Map<char>();
        mLane0End = mLane0Begin + capacity;
        begin = ptr = mLane0Begin;
        end = mLane0End;
        mCompletionTimeline = device->CreateSemaphore(true);
        if (mLaneCount > 1)
        {
            mLanes.reserve(mLaneCount - 1u);
            for (size_t i = 1; i < mLaneCount; ++i)
                mLanes.emplace_back(Core::ConstructUnique<UploadLane>(GLOBAL_ALLOC, device, capacity, type));
        }
        mSubmitSignals.reserve(mLaneCount + 1u);
    }

    ImmediateContext& ImmediateUpload::CurrentContext()
    {
        return mCurrentLane == 0 ? ctx : mLanes[mCurrentLane - 1u]->ctx;
    }

    ImmediateContext const& ImmediateUpload::CurrentContext() const
    {
        return mCurrentLane == 0 ? ctx : mLanes[mCurrentLane - 1u]->ctx;
    }

    RHIBuffer* ImmediateUpload::CurrentStaging() const
    {
        return mCurrentLane == 0 ? staging.Get() : mLanes[mCurrentLane - 1u]->staging.Get();
    }

    char* ImmediateUpload::CurrentBegin() const
    {
        return mCurrentLane == 0 ? mLane0Begin : mLanes[mCurrentLane - 1u]->begin;
    }

    char* ImmediateUpload::CurrentEnd() const
    {
        return mCurrentLane == 0 ? mLane0End : mLanes[mCurrentLane - 1u]->end;
    }

    size_t& ImmediateUpload::CurrentSignalValue()
    {
        return mCurrentLane == 0 ? mLane0SignalValue : mLanes[mCurrentLane - 1u]->signalValue;
    }

    RHICommandList* ImmediateUpload::Get() const
    {
        return CurrentContext().Get();
    }

    void ImmediateUpload::WaitCurrentLaneReusable()
    {
        if (!mCompletionTimeline)
            return;
        size_t const signalValue = CurrentSignalValue();
        if (signalValue == 0)
            return;
        RHIDeviceQueue::TimelinePair wait{mCompletionTimeline.Get(), signalValue};
        mDevice->WaitForTimelineSemaphores(Span<const RHIDeviceQueue::TimelinePair>(&wait, 1), -1);
        CurrentSignalValue() = 0;
    }

    void ImmediateUpload::SelectCurrentLane()
    {
        begin = CurrentBegin();
        ptr = begin;
        end = CurrentEnd();
    }

    void ImmediateUpload::Begin()
    {
        WaitCurrentLaneReusable();
        CurrentContext()->Reset();
        CurrentContext()->Begin();
        SelectCurrentLane();
    }
    char* ImmediateUpload::Upload(RHIBuffer* dst, size_t dataSize, size_t dstOffset)
    {
        if (ptr + dataSize > end)
            return nullptr;
        CurrentContext()->CopyBuffer(
            CurrentStaging(), dst,
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
        CurrentContext()->CopyBufferToImage(CurrentStaging(), dst, RHITextureLayout::TransferDst,
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
        CurrentContext()->End();
        if (mCompletionTimeline)
        {
            size_t& signalValue = CurrentSignalValue();
            signalValue = mNextSignalValue++;
            mSubmitSignals.clear();
            mSubmitSignals.insert(mSubmitSignals.end(), desc.timelineSignals.begin(), desc.timelineSignals.end());
            mSubmitSignals.push_back({mCompletionTimeline.Get(), signalValue});
            ImmediateSubmitDesc submitDesc = desc;
            submitDesc.timelineSignals = mSubmitSignals;
            CurrentContext().Submit(submitDesc);
            mCurrentLane = (mCurrentLane + 1u) % mLaneCount;
            SelectCurrentLane();
        }
        else
        {
            CurrentContext().Submit(desc);
        }
    }
    void ImmediateUpload::WaitIdle()
    {
        if (!mCompletionTimeline)
        {
            CurrentContext().WaitIdle();
            return;
        }
        if (mNextSignalValue > 1u)
        {
            RHIDeviceQueue::TimelinePair wait{mCompletionTimeline.Get(), mNextSignalValue - 1u};
            mDevice->WaitForTimelineSemaphores(Span<const RHIDeviceQueue::TimelinePair>(&wait, 1), -1);
        }
        mLane0SignalValue = 0;
        for (Core::UniquePtr<UploadLane> const& lane : mLanes)
            lane->signalValue = 0;
    }
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