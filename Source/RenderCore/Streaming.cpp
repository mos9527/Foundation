#include "Streaming.hpp"
#include "RHICore/Device.hpp"
// TODO: Thread safety
namespace Foundation::RenderCore
{
    StreamingPool::PageRef& StreamingPool::AddRef(PageRef& lhs, PageRef const& rhs)
    {
        for (size_t i = 0; i < kStreamingMaxPages / 4; i += 4)
        {
            lhs[i + 0] += rhs[i + 0];
            lhs[i + 1] += rhs[i + 1];
            lhs[i + 2] += rhs[i + 2];
            lhs[i + 3] += rhs[i + 3];
        }
        return lhs;
    }
    StreamingPool::PageRef& StreamingPool::DecRef(PageRef& lhs, PageRef const& rhs)
    {
        for (size_t i = 0; i < kStreamingMaxPages / 4; i += 4)
        {
            lhs[i + 0] -= rhs[i + 0];
            lhs[i + 1] -= rhs[i + 1];
            lhs[i + 2] -= rhs[i + 2];
            lhs[i + 3] -= rhs[i + 3];
        }
        return lhs;
    }
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
            .resource = {.heap = RHIDeviceHeapType::Upload,
                         .shared = false, /* Transfer only */
                         .staging = true},
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
    void StreamingPool::Write(Span<const char> data, PageRef& outPages, WriteList& outList, size_t alignment)
    {
        RHIBuffer* buf = nullptr;
        while (!data.empty())
        {
            auto* page = &mPages[GetPage()];
            if (page->freeSize() < alignment)
            {
                if (mPageTop >= kStreamingMaxPages)
                    Collect();
                else
                    PageAlloc();
                page = &mPages[GetPage()];
            }
            size_t write = std::min(data.size(), page->freeSize()), offset = 0;
            write -= write % alignment; // Align down
            PageWrite(page->id, data.SubSpan(0, write), offset, buf);
            data = data.SubSpan(write);
            outPages[page->id]++;
            outList.emplace_back(buf, offset, write);
        }
    }
    void StreamingPool::Collect()
    {
        for (size_t i = 0; i < mPageTop && !mPageHeap.empty(); i++)
        {
            if (mPageRefs[i] == 0)
                PageReset(i);
        }
    }
    StreamingPool::StreamingPool(RHIDevice* device, Allocator* allocator) :
        mDevice(device), mAllocator(allocator), mPageHeap(allocator), mBufferCopies(allocator),
        mTextureCopies(allocator), mTransferCmds(allocator), mPendingCompletions(allocator)
    {
        mTransferQueue = mDevice->GetDeviceQueue(RHIDeviceQueueType::Transfer);
        mCommandPool = mDevice->CreateCommandPool({.queue = mTransferQueue, .type = RHICommandPoolType::Transient});
        mTransferSemaphore = mDevice->CreateSemaphore(true /* timeline */);
        mWorker = Thread([this] { WorkerThread(); });
    }
    SharedPromise<> StreamingPool::Write(Span<const char> data, RHIBuffer* dst, size_t offset)
    {
        WriteList writes(mAllocator);
        PageRef touched{};

        std::unique_lock lock(mScheduleMutex);
        Write(data, touched, writes);
        AddRef(mPageRefs, touched);
        SharedPromise<> promise = ConstructShared<std::promise<void>>(mAllocator);
        Vector<BufferCopyCommand> cmds(mAllocator);
        for (auto& [srcBuffer, srcOffset, size] : writes)
        {
            cmds.emplace_back(
                srcBuffer, dst,
                RHICommandList::CopyBufferRegion{.srcOffset = srcOffset, .dstOffset = offset, .size = size});
            offset += size;
        }
        mBufferCopies.emplace_back(cmds, touched, promise);
        return promise;
    }
    SharedPromise<> StreamingPool::Write(Span<const char> data, RHITexture* dst, RHITextureLayout dstLayout,
                                         RHITextureAspectFlag aspect, uint32_t dstMip, uint32_t firstLayer)
    {
        CHECK_MSG(dst->mDesc.dimension == RHITextureDimension::E2D, "2D Textures ONLY");
        WriteList writes(mAllocator);
        PageRef touched{};

        std::unique_lock lock(mScheduleMutex);
        // https://docs.vulkan.org/guide/latest/image_copies.html
        // Texture writes are aligned to row pitch (assuming tightly packed)
        auto extent = dst->mDesc.extent;
        size_t numRows = extent.y;
        size_t rowPitch = extent.x * RHIResourceFormatSize(dst->mDesc.format);
        size_t layerSize = rowPitch * extent.y * extent.z, numLayers = data.size() / layerSize;
        SharedPromise<> promise = ConstructShared<std::promise<void>>(mAllocator);
        Vector<TextureCopyCommand> cmds(mAllocator);
        size_t offset = 0;
        for (size_t layer = 0; layer < numLayers; layer++)
        {
            Span layerData = data.SubSpan(layer * layerSize, layerSize);
            Write(layerData, touched, writes, rowPitch);
            AddRef(mPageRefs, touched);
            // Same mip
            for (auto& [srcBuffer, srcOffset, size] : writes)
            {
                int rows = size / rowPitch, offsetAll = offset / rowPitch, offsetRow = offsetAll % numRows;
                cmds.emplace_back(srcBuffer, dst, dstLayout,
                                  RHICommandList::CopyImageRegion{
                                      .srcBufferOffset = static_cast<uint32_t>(srcOffset),
                                      .dstLayer = RHITextureSubresourceLayer{
                                            .aspect = aspect,
                                            .mipLevel = dstMip,
                                            .baseArrayLayer = static_cast<uint32_t>(firstLayer + layer),
                                            .layerCount = 1,
                                      },
                                      .dstOffset = {0, offsetRow, 0},
                                      .extent = {extent.x, rows, 1}
                                });
                offset += size;
            }
        }
        mTextureCopies.emplace_back(cmds, touched, promise);
        return promise;
    }
    StreamingPool::~StreamingPool() { mShutdown = true; }
    String StreamingPool::DbgGetStatistics() const
    {
        size_t allocated = 0, used = 0, refs = 0;
        for (size_t i = 0; i < mPageTop; i++)
        {
            allocated += kStreamingPageSize;
            used += mPages[i].top - mPages[i].mem;
            refs += mPageRefs[i];
        }
        return fmt::format("Pages: {}, Allocated: {} KB, Used: {} KB, Refs: {}", mPageTop, allocated / 1024,
                           used / 1024, refs);
    }
    void StreamingPool::Submit()
    {
        // Signal completions
        while (!mPendingCompletions.empty())
        {
            auto& [ctr, pages, promises] = mPendingCompletions.front();
            if (!mDevice->WaitForTimelineSemaphores({{{mTransferSemaphore.Get(), ctr}}}, 0 /* timeout */))
            {
                break; // In-progress. Ctr is monotonically increasing so terminate here
            }
            // Complete
            DecRef(mPageRefs, pages);
            for (auto& promise : promises)
                promise->set_value();
            mPendingCompletions.pop_front();
        }
        // Schedule and submit
        if (mBufferCopies.empty() && mTextureCopies.empty())
            return;
        std::unique_lock lock(mScheduleMutex);
        PageRef touched{};
        Vector<SharedPromise<>> promises(mAllocator);
        auto& cmd = mTransferCmds.emplace_back(mCommandPool->CreateCommandList());
        cmd->Begin();
        for (auto& [bccs, pages, promise] : mBufferCopies)
        {

            for (auto const& [src, dst, region] : bccs)
                cmd->CopyBuffer(src, dst, {region});
            AddRef(touched, pages);
            promises.emplace_back(promise);
        }
        for (auto& [tccs, pages, promise] : mTextureCopies)
        {

            for (auto const& [src, dst, dstLayout, region] : tccs)
                cmd->CopyBufferToImage(src, dst, dstLayout, region);
            AddRef(touched, pages);
            promises.emplace_back(promise);
        }
        cmd->End();
        // Submit
        if (mSubmitCtr)
            mTransferQueue->Submit(
                {{RHIDeviceQueue::SubmitDesc{.timelineWaits = {{{mTransferSemaphore.Get(), mSubmitCtr}}},
                                             .timelineSignals = {{{mTransferSemaphore.Get(), mSubmitCtr + 1}}},
                                             .waitsStages = {{{RHIPipelineStageBits::Transfer}}},
                                             .cmdLists = {{cmd.Get()}}}}});
        else
            mTransferQueue->Submit({{RHIDeviceQueue::SubmitDesc{
                .timelineSignals = {{{mTransferSemaphore.Get(), mSubmitCtr + 1}}}, .cmdLists = {{cmd.Get()}}}}});
        mSubmitCtr++;
        mPendingCompletions.emplace_back(mSubmitCtr, touched, std::move(promises));
        mBufferCopies.clear(), mTextureCopies.clear();
    }
    void StreamingPool::WorkerThread()
    {
        while (!mShutdown)
        {
            Submit();
        }
    }
} // namespace Foundation::RenderCore
