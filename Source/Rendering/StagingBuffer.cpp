#include "Bits/Format.hpp"
#include "StagingBuffer.hpp"

namespace Foundation::Rendering
{
    BufferStagingList& CoalesceBufferStaging(BufferStagingList& res)
    {
        if (res.size() <= 1)
            return res;
        auto SortKey = [](BufferStagingItem const& item)
        {
            auto const& [src, dst, region] = item;
            return Tuple{reinterpret_cast<size_t>(src), reinterpret_cast<size_t>(dst), region.dstOffset,
                         region.srcOffset};
        };
        Ranges::sort(res, [SortKey](BufferStagingItem& a, BufferStagingItem& b) { return SortKey(a) < SortKey(b); });
        res.erase(Ranges::unique(res, [SortKey](BufferStagingItem& a, BufferStagingItem& b)
                                 { return SortKey(a) == SortKey(b); })
                      .begin(),
                  res.end());
        Span<BufferStagingItem> staging = {res.begin(), res.end()};
        size_t i = 1, j = 0;
        for (; i < staging.size(); ++i)
        {
            auto const& [src, dst, region] = staging[i];
            auto const& [prev_src, prev_dst, prev_region] = staging[i - 1];
            size_t d_src = region.srcOffset - prev_region.srcOffset;
            size_t d_dst = region.dstOffset - prev_region.dstOffset;
            size_t sz = region.size;
            auto& [top_src, top_dst, top_region] = res[j];
            if (src == prev_src && dst == prev_dst && d_src == sz && d_dst == sz)
                top_region.size += sz;
            else
                j++, res[j] = staging[i];
        }
        res.resize(j + 1);
        return res;
    }
    StagingBuffer::StagingBuffer(RHIDevice* device, const size_t size, Allocator* allocator) :
        mAllocator(allocator), mDevice(device),
        mBuffer(device->CreateBuffer(
                {.resource = {
                .heap = RHIDeviceHeapType::Upload,
                .hostAccess = RHIResourceHostAccess::WriteOnly,
                .staging = true
            },
             .usage = RHIBufferUsageBits::TransferSource,
             .size = size})),
        mSize(size)
    {
        mMapped = mBuffer->Map();
    }
    size_t StagingBuffer::Write(const Span<const char> data, const size_t alignment)
    {
        size_t alignedOffset = (mOffset + alignment - 1) & ~(alignment - 1);
        CHECK_MSG(alignedOffset + data.size() <= mSize, "Staging buffer overflow");
        std::memcpy(static_cast<char*>(mMapped) + alignedOffset, data.data(), data.size());
        mOffset = alignedOffset + data.size();
        return alignedOffset;
    }
    void StagingBuffer::Seek(size_t offset, const size_t alignment)
    {
        size_t alignedOffset = (mOffset + alignment - 1) & ~(alignment - 1);
        mOffset = alignedOffset + offset;
        CHECK_MSG(mOffset < mSize, "Staging buffer overflow");
    }
    void StagingBuffer::Reset() { mOffset = 0; }

    StagedBuffer::StagedBuffer(RHIDevice* device, Allocator* allocator, RHIBufferDesc const& desc,
                               size_t stagingBudget, Optional<uint32_t> clearValue) :
        mDevice(device), mAllocator(allocator), mStagingBuffer(device, stagingBudget, allocator),
        mBufferStagings(allocator),
        mIdleGuard(mDevice), mClearValue(clearValue)
    {
        // Create one staging buffer for each frame in flight.
        // This in turn utilizes the frame fences for synchronization.
        // A lower memory footprint option is to do this with a single staging buffer,
        // and wait for the resource fence before reusing it, which
        // incurs CPU stalls.
        if (stagingBudget == kFullSize)
            stagingBudget = desc.size;
        mStagingBuffer.GetBuffer()->DebugSetObjectName("Staging Buffer");
        mBuffer = device->CreateBuffer(desc);
    }
    void StagedBuffer::BeginTransfer()
    {
        CHECK_MSG(mState == State::Idle, "Staging is already in {} state", mState);
        mStagingBuffer.Reset();
        mBufferStagings.clear();
        mState = State::Transfer;
    }
    StagingBuffer* StagedBuffer::GetStagingBuffer()
    {
        CHECK_MSG(mState == State::Transfer, "Staging is not in Transfer state");
        return &mStagingBuffer;
    }
    void StagedBuffer::Transfer(size_t dst_offset, Span<const char> data, size_t alignment)
    {
        CHECK_MSG(mState == State::Transfer, "Staging is not in Transfer state");
        size_t src_offset = mStagingBuffer.Write(data, alignment);
        if (!mBufferStagings.empty())
        {
            // Check if we can coalesce the buffer transfer right away
            auto& [last_src, last_dst, last_region] = mBufferStagings.back();
            if (last_src == mStagingBuffer.GetBuffer() && last_dst == mBuffer.Get() &&
                dst_offset - last_region.dstOffset == last_region.size &&
                src_offset - last_region.srcOffset == last_region.size)
            {
                last_region.size += data.size_bytes();
                return;
            }
        }
        mBufferStagings.emplace_back(mStagingBuffer.GetBuffer(), mBuffer.Get(),
                                      RHICommandList::CopyBufferRegion{src_offset, dst_offset, data.size_bytes()});
    }
    void StagedBuffer::EndTransfer()
    {
        CHECK_MSG(mState == State::Transfer, "Staging is not in Transfer state");
        CoalesceBufferStaging(mBufferStagings);
        mState = State::Idle;
    }
    void StagedBuffer::Update(RHICommandList* cmd)
    {
        CHECK_MSG(mState == State::Idle, "Staging is not in Idle state");
        if (!HasUpdates())
            return;
        auto* stagingBuf = mStagingBuffer.GetBuffer();
        cmd->DebugBegin("Staging Buffer Updates");
        mStagingBuffer.GetBuffer()->Flush(0, mStagingBuffer.Tell());
        cmd->BeginTransition();
        cmd->SetBufferTransition(
            stagingBuf,
            {
                .srcAccess = RHIResourceAccessBits::HostWrite,
                .dstAccess = RHIResourceAccessBits::TransferRead,
                .srcStage = RHIPipelineStageBits::Host,
                .dstStage = RHIPipelineStageBits::Transfer
            }
        );
        cmd->EndTransition();
        if (mClearValue.has_value())
        {
            cmd->BeginTransition();
            cmd->SetBufferTransition(
                mBuffer.Get(),
                {
                    .dstAccess = RHIResourceAccessBits::TransferWrite,
                    .dstStage = RHIPipelineStageBits::Transfer
                }
            );
            cmd->EndTransition();
            cmd->FillBuffer(mBuffer.Get(), mClearValue.value());
            mClearValue.reset();
        }
        cmd->BeginTransition();
        cmd->SetBufferTransition(
            mBuffer.Get(),
            {
                .srcAccess = RHIResourceAccessBits::TransferWrite,
                .dstAccess = RHIResourceAccessBits::TransferWrite,
                .srcStage = RHIPipelineStageBits::Transfer,
                .dstStage = RHIPipelineStageBits::Transfer
            }
        );
        cmd->EndTransition();
        for (auto const& [src, dst, range] : mBufferStagings)
            cmd->CopyBuffer(src, dst, {range});
        cmd->BeginTransition();
        cmd->SetBufferTransition(
            stagingBuf,
            {
                .srcAccess = RHIResourceAccessBits::TransferRead,
                .dstAccess = RHIResourceAccessBits::HostWrite,
                .srcStage = RHIPipelineStageBits::Transfer,
                .dstStage = RHIPipelineStageBits::Host,
            }
        );
        cmd->EndTransition();
        cmd->DebugEnd();
        mBufferStagings.clear();
    }
} // namespace Foundation::RenderCore
