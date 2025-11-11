#include "Streaming.hpp"
#include "RHICore/Device.hpp"
// TODO: Thread safety
namespace Foundation::RenderCore
{
    void StreamingPool::Maintain(int id, size_t oldSize, size_t newSize)
    {
        auto it = mPageHeap.find({oldSize, id});
        CHECK_MSG(it != mPageHeap.end(), "StreamingPool page not found in heap");
        mPageHeap.erase(it);
        mPageHeap.insert({newSize, id});
    }

    void StreamingPool::PageAlloc()
    {
        CHECK_MSG(mPageTop < kStreamingMaxPages, "StreamingPool out of pages");
        auto& page = mPages[mPageTop];
        page.id = mPageTop++, page.capacity = kStreamingPageSize;
        page.buffer = mDevice->CreateBuffer({
            .resource = {
                .heap = RHIDeviceHeapType::Upload,
                .shared = false, /* Transfer only */
                .staging = true
            },
            .usage = RHIBufferUsageBits::TransferSource,
            .size = page.capacity,
        });
        page.mem = page.top = static_cast<char*>(page.buffer->Map());
        // Insert
        mPageHeap.insert({page.freeSize(), page.id});
    }
    void StreamingPool::PageWrite(int id, Span<const char> data, size_t& outOffset, RHIBuffer*& outBuffer)
    {
        auto& page = mPages[id];
        CHECK_MSG(data.size() <= page.freeSize(), "StreamingPool page overflow");
        outOffset = page.top - page.mem;
        outBuffer = page.buffer.Get();
        const size_t oldSize = page.freeSize();
        std::memcpy(page.top, data.data(), data.size());
        page.top += data.size();
        Maintain(id, oldSize, page.freeSize());
    }
    void StreamingPool::PageReset(int id)
    {
        auto& page = mPages[id];
        const size_t oldSize = page.freeSize();
        page.top = page.mem;
        Maintain(id, oldSize, page.freeSize());
    }
    int StreamingPool::GetPage()
    {
        if (mPageHeap.empty())
            PageAlloc();
        return mPageHeap.rbegin()->second;
    }
    void StreamingPool::Write(Span<const char> data, PageSet& outPages, WriteList& outList)
    {
        RHIBuffer* buf = nullptr;
        while (!data.empty())
        {
            auto* page = &mPages[GetPage()];
            if (!page->freeSize())
            {
                PageAlloc();
                page = &mPages[GetPage()];
            }
            size_t write = std::min(data.size(), page->freeSize()), offset = 0;
            PageWrite(page->id, data.SubSpan(0, write), offset, buf);
            data = data.SubSpan(write);
            outPages.set(page->id);
            outList.emplace_back(buf, offset, write);
        }
    }
    void StreamingPool::IncRef(PageSet const& pages)
    {
        for (int i = 0; i < kStreamingMaxPages; i++)
        {
            if (pages.test(i))
                mPageRefs[i]++;
        }
    }
    void StreamingPool::DecRef(PageSet const& pages)
    {
        for (int i = 0; i < kStreamingMaxPages; i++)
        {
            if (pages.test(i))
                mPageRefs[i]--;
        }
    }
    void StreamingPool::Collect()
    {
        for (int i = 0; i < mPageTop && !mPageHeap.empty(); i++)
        {
            if (mPageRefs[i] == 0 && mPages[i].id == i /* actually allocated */)
                PageReset(i);
        }
    }
    StreamingPool::StreamingPool(RHIDevice* device, Allocator* allocator) :
        mDevice(device), mAllocator(allocator), mPageHeap(allocator), mCommands(allocator)
    {
        mTransferQueue = mDevice->GetDeviceQueue(RHIDeviceQueueType::Transfer);
        mCommandPool = mDevice->CreateCommandPool({
            .queue = RHIDeviceQueueType::Graphics,
            .type = RHICommandPoolType::Transient
        });
    }
    SharedPromise<> StreamingPool::Write(Span<const char> data, RHIBuffer* dst, size_t offset)
    {
        WriteList writes(mAllocator);
        PageSet touched;
        Write(data, touched, writes);
        IncRef(touched);
        SharedPromise<> promise;
        auto command = mCommandPool->CreateCommandList();
        command->Begin();
        for (auto& [srcBuffer, srcOffset, size] : writes)
        {
            RHICommandList::CopyBufferRegion region{
                .srcOffset = srcOffset,
                .dstOffset = offset,
                .size = size
            };
            command->CopyBuffer(srcBuffer, dst, {region});
            offset += size;
        }
        command->End();
        mCommands.emplace_back(std::move(command), touched, promise);
        return promise;
    }
    void StreamingPool::Execute()
    {

    }
} // namespace Foundation::RenderCore
