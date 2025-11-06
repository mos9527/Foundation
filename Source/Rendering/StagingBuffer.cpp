#include "Bits/Format.hpp"
#include "StagingBuffer.hpp"

namespace Foundation::Rendering
{
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
} // namespace Foundation::RenderCore
