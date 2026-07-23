#include <cassert>
#include <cstring>
namespace Foundation::RenderCore
{
    RHIDeviceScopedHandle<RHIBuffer> ImmediateCreateBuffer(RHIDevice* device, RHIBufferDesc const& desc,
                                                           void const* data, size_t bytes)
    {
        RHIDeviceScopedHandle<RHIBuffer> res = device->CreateBuffer(desc);
        ImmediateUpload upload(device, bytes);
        ImmediateUpload::UploadBatch batch = upload.BeginBatch();
        char* dst = batch.Upload(res.Get(), bytes, 0);
        std::memcpy(dst, data, bytes);
        batch.End();
        upload.WaitIdle();
        return res;
    }

    RHIDeviceScopedHandle<RHITexture> ImmediateCreateTexture(RHIDevice* device, RHITextureDesc const& desc,
                                                             void const* data, size_t bytes,
                                                             RHITextureLayout finalLayout)
    {
        RHIDeviceScopedHandle<RHITexture> res = device->CreateTexture(desc);
        ImmediateUpload upload(device, bytes);
        ImmediateUpload::UploadBatch batch = upload.BeginBatch();
        RHICommandList* cmd = batch.Get();
        cmd->BeginTransition();
        cmd->SetImageTransition(res.Get(), {
                                               .dstAccess = RHIResourceAccessBits::TransferWrite,
                                               .dstStage = RHIPipelineStageBits::Transfer,
                                               .srcImgLayout = RHITextureLayout::Undefined,
                                               .dstImgLayout = RHITextureLayout::TransferDst,
                                               .srcImgRange = RHITextureSubresourceRange::Create(),
                                           });
        cmd->EndTransition();
        char* dst =
            batch.Upload(res.Get(), bytes, {.aspect = RHITextureAspectFlagBits::Color}, RHIOffset2D{}, RHIExtent2D{});
        std::memcpy(dst, data, bytes);
        if (finalLayout != RHITextureLayout::TransferDst)
        {
            cmd->BeginTransition();
            cmd->SetImageTransition(res.Get(), {
                                                   .srcAccess = RHIResourceAccessBits::TransferRead,
                                                   .dstAccess = RHIResourceAccessBits::ShaderRead,
                                                   .srcStage = RHIPipelineStageBits::Transfer,
                                                   .dstStage = RHIPipelineStageBits::FragmentShader,
                                                   .srcImgLayout = RHITextureLayout::TransferDst,
                                                   .dstImgLayout = finalLayout,
                                                   .srcImgRange = RHITextureSubresourceRange::Create(),
                                               });
            cmd->EndTransition();
        }
        batch.End();
        upload.WaitIdle();
        return res;
    }

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
                                              .shared = false,
                                              .coherent = true,
                                              .staging = true,
                                          },
                                      .usage = RHIBufferUsageBits::TransferSource,
                                      .size = capacity}))
    {
        begin = staging->Map<char>();
        end = begin + capacity;
    }

    ImmediateUpload::ImmediateUpload(RHIDevice* device, size_t capacity, RHIDeviceQueueType type, size_t buffers) :
        mDevice(device), mCapacity(capacity), mLanes(GLOBAL_ALLOC), mSubmitSignals(GLOBAL_ALLOC)
    {
        size_t const laneCount = std::max<size_t>(buffers, 1u);
        mCompletionTimeline = device->CreateSemaphore(true);
        mLanes.reserve(laneCount);
        for (size_t i = 0; i < laneCount; ++i)
            mLanes.emplace_back(Core::ConstructUnique<UploadLane>(GLOBAL_ALLOC, device, capacity, type));
        mSubmitSignals.reserve(laneCount + 1u);
    }

    bool ImmediateUpload::WaitTimeline(size_t value, size_t timeout) const
    {
        if (!mCompletionTimeline || value == 0)
            return true;
        RHIDeviceQueue::TimelinePair wait{mCompletionTimeline.Get(), value};
        return mDevice->WaitForTimelineSemaphores(Span<const RHIDeviceQueue::TimelinePair>(&wait, 1), timeout);
    }

    bool ImmediateUpload::IsLaneReusable(size_t lane, size_t timeout)
    {
        UploadLane& state = *mLanes[lane];
        if (state.recording)
            return false;
        if (state.signalValue == 0)
            return true;
        if (!WaitTimeline(state.signalValue, timeout))
            return false;
        state.signalValue = 0;
        return true;
    }

    void ImmediateUpload::WaitLaneReusable(size_t lane)
    {
        CHECK_MSG(IsLaneReusable(lane, static_cast<size_t>(-1)), "ImmediateUpload lane failed to become reusable");
    }

    ImmediateUpload::UploadBatch ImmediateUpload::AcquireLane(size_t lane)
    {
        WaitLaneReusable(lane);
        UploadLane& state = *mLanes[lane];
        state.recording = true;
        state.ctx->Reset();
        state.ctx->Begin();
        return UploadBatch(this, lane);
    }

    bool ImmediateUpload::TryAcquireLane(size_t lane, UploadBatch& out)
    {
        if (!IsLaneReusable(lane, 0))
            return false;
        UploadLane& state = *mLanes[lane];
        state.recording = true;
        state.ctx->Reset();
        state.ctx->Begin();
        out = UploadBatch(this, lane);
        return true;
    }

    ImmediateUpload::UploadBatch ImmediateUpload::BeginBatch()
    {
        size_t const lane = mNextLane;
        mNextLane = (mNextLane + 1u) % mLanes.size();
        return AcquireLane(lane);
    }

    bool ImmediateUpload::TryBeginBatch(UploadBatch& out)
    {
        size_t const start = mNextLane;
        for (size_t i = 0; i < mLanes.size(); ++i)
        {
            size_t const lane = (start + i) % mLanes.size();
            if (!TryAcquireLane(lane, out))
                continue;
            mNextLane = (lane + 1u) % mLanes.size();
            return true;
        }
        return false;
    }

    void ImmediateUpload::ReleaseRecording(size_t lane) noexcept
    {
        if (lane < mLanes.size())
            mLanes[lane]->recording = false;
    }

    void ImmediateUpload::WaitIdle()
    {
        if (mNextSignalValue > 1u)
            CHECK(WaitTimeline(mNextSignalValue - 1u, static_cast<size_t>(-1)));
        for (Core::UniquePtr<UploadLane> const& lane : mLanes)
        {
            CHECK(!lane->recording);
            lane->signalValue = 0;
        }
    }

    ImmediateUpload::UploadBatch::UploadBatch(ImmediateUpload* owner, size_t lane) noexcept :
        mOwner(owner), mLane(lane)
    {
        UploadLane& state = *mOwner->mLanes[mLane];
        begin = ptr = state.begin;
        end = state.end;
    }

    ImmediateUpload::UploadBatch::UploadBatch(UploadBatch&& other) noexcept :
        mOwner(std::exchange(other.mOwner, nullptr)), mLane(std::exchange(other.mLane, SIZE_MAX)),
        mCompletionValue(std::exchange(other.mCompletionValue, 0)), begin(other.begin), ptr(other.ptr), end(other.end)
    {
        other.begin = other.ptr = other.end = nullptr;
    }

    ImmediateUpload::UploadBatch& ImmediateUpload::UploadBatch::operator=(UploadBatch&& other) noexcept
    {
        if (this == &other)
            return *this;
        assert(!IsValid());
        mOwner = std::exchange(other.mOwner, nullptr);
        mLane = std::exchange(other.mLane, SIZE_MAX);
        mCompletionValue = std::exchange(other.mCompletionValue, 0);
        begin = other.begin;
        ptr = other.ptr;
        end = other.end;
        other.begin = other.ptr = other.end = nullptr;
        return *this;
    }

    ImmediateUpload::UploadBatch::~UploadBatch() noexcept
    {
        assert(!IsValid());
    }

    RHICommandList* ImmediateUpload::UploadBatch::Get() const
    {
        CHECK(IsValid());
        return mOwner->mLanes[mLane]->ctx.Get();
    }

    RHIDeviceSemaphore* ImmediateUpload::UploadBatch::CompletionTimeline() const
    {
        return mOwner ? mOwner->CompletionTimeline() : nullptr;
    }

    char* ImmediateUpload::UploadBatch::Upload(RHIBuffer* dst, size_t dataSize, size_t dstOffset)
    {
        CHECK(IsValid());
        if (ptr + dataSize > end)
            return nullptr;
        UploadLane& state = *mOwner->mLanes[mLane];
        state.ctx->CopyBuffer(state.staging.Get(), dst,
                              {{{.srcOffset = static_cast<uint32_t>(ptr - begin),
                                 .dstOffset = dstOffset,
                                 .size = dataSize}}});
        char* res = ptr;
        ptr += dataSize;
        return res;
    }

    char* ImmediateUpload::UploadBatch::Upload(RHITexture* dst, size_t dataSize, RHITextureSubresourceLayer dstLayer,
                                               RHIOffset2D dstOffset, RHIExtent2D dstExtent)
    {
        RHIExtent3D maxExtent = dst->mDesc.extent;
        RHIOffset3D offset{dstOffset.x, dstOffset.y, 0};
        RHIExtent3D extent{dstExtent.x ? dstExtent.x : maxExtent.x, dstExtent.y ? dstExtent.y : maxExtent.y, 1};
        return Upload(dst, dataSize, dstLayer, offset, extent);
    }

    char* ImmediateUpload::UploadBatch::Upload(RHITexture* dst, size_t dataSize, RHITextureSubresourceLayer dstLayer,
                                               RHIOffset3D dstOffset, RHIExtent3D dstExtent)
    {
        CHECK(IsValid());
        if (ptr + dataSize > end)
            return nullptr;
        UploadLane& state = *mOwner->mLanes[mLane];
        RHIExtent3D maxExtent = dst->mDesc.extent;
        RHIExtent3D extent{dstExtent.x ? dstExtent.x : maxExtent.x, dstExtent.y ? dstExtent.y : maxExtent.y,
                           dstExtent.z ? dstExtent.z : maxExtent.z};
        state.ctx->CopyBufferToImage(state.staging.Get(), dst, RHITextureLayout::TransferDst,
                                     {{{.srcBufferOffset = static_cast<uint32_t>(ptr - begin),
                                        .dstLayer = dstLayer,
                                        .dstOffset = dstOffset,
                                        .extent = extent}}});
        char* res = ptr;
        ptr += dataSize;
        return res;
    }

    bool ImmediateUpload::UploadBatch::Align(uint32_t alignment)
    {
        CHECK(IsValid());
        auto* pup = reinterpret_cast<char*>(AlignUp(reinterpret_cast<uintptr_t>(ptr), alignment));
        if (pup >= end)
            return false;
        ptr = pup;
        return true;
    }

    void ImmediateUpload::UploadBatch::End(RHIDeviceFence* completionFence)
    {
        End(ImmediateSubmitDesc{.completionFence = completionFence});
    }

    void ImmediateUpload::UploadBatch::End(ImmediateSubmitDesc const& desc)
    {
        CHECK(IsValid());
        UploadLane& state = *mOwner->mLanes[mLane];
        state.ctx->End();
        mCompletionValue = mOwner->mNextSignalValue++;
        state.signalValue = mCompletionValue;
        mOwner->mSubmitSignals.clear();
        mOwner->mSubmitSignals.insert(mOwner->mSubmitSignals.end(), desc.timelineSignals.begin(),
                                      desc.timelineSignals.end());
        mOwner->mSubmitSignals.push_back({mOwner->mCompletionTimeline.Get(), mCompletionValue});
        ImmediateSubmitDesc submitDesc = desc;
        submitDesc.timelineSignals = mOwner->mSubmitSignals;
        state.ctx.Submit(submitDesc);
        mOwner->ReleaseRecording(mLane);
        mOwner = nullptr;
        mLane = SIZE_MAX;
        begin = ptr = end = nullptr;
    }

    void ImmediateUpload::UploadBatch::Abort()
    {
        CHECK(IsValid());
        UploadLane& state = *mOwner->mLanes[mLane];
        state.ctx->End();
        mOwner->ReleaseRecording(mLane);
        mOwner = nullptr;
        mLane = SIZE_MAX;
        begin = ptr = end = nullptr;
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
