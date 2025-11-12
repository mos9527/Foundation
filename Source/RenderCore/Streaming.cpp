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
        page.id = mPageTop++, page.capacity = mDesc.streamingPageSize;
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
    int StreamingPool::Collect(std::unique_lock<Mutex>& lck)
    {
        int res = 0;
        auto collect = [&]
        {
            for (size_t i = 0; i < mPageTop && !mPageHeap.empty(); i++)
            {
                if (mPageRefs[i] == 0)
                    PageReset(i), res++;
            }
        };
        collect();
        while (!res)
        {
            // Block until some pages are freed
            mResolvedCV.wait(lck);
            collect();
        }
        return res;
    }
    void StreamingPool::WritePages(std::unique_lock<Mutex>& lck, Span<const char> data, WriteList& outList, size_t alignment)
    {
        RHIBuffer* buf = nullptr;
        while (!data.empty())
        {
            auto* page = &mPages[GetPage()];
            if (page->freeSize() < alignment)
            {
                if (mPageTop < kStreamingMaxPages)
                    PageAlloc();
                else
                    Collect(lck);
                page = &mPages[GetPage()];
            }
            size_t write = std::min(data.size(), page->freeSize()), offset = 0;
            write -= write % alignment; // Align down
            PageWrite(page->id, data.SubSpan(0, write), offset, buf);
            data = data.SubSpan(write);
            mPageRefs[page->id]++;
            outList.emplace_back(buf, offset, write, page->id);
        }
    }
    StreamingPool::StreamingPool(RHIDevice* device, Allocator* allocator, StreamingPoolDesc const& desc) :
        mDevice(device), mAllocator(allocator), mPageHeap(allocator), mBufferCopies(allocator),
        mTextureCopies(allocator), mTransferCmds(allocator), mPendingCompletions(allocator), mDesc(desc)
    {
        mTransferQueue = mDevice->GetDeviceQueue(RHIDeviceQueueType::Transfer);
        mCommandPool = mDevice->CreateCommandPool({.queue = mTransferQueue, .type = RHICommandPoolType::Transient});
        mTransferSemaphore = mDevice->CreateSemaphore(true /* timeline */);
        mWorker = Thread([this] { WorkerThread(); });
    }
    SharedPromise<> StreamingPool::Write(Span<const char> data, RHIBuffer* dst, size_t offset)
    {
        WriteList writes(mAllocator);

        std::unique_lock lock(mPageMutex);
        WritePages(lock, data, writes, 1);
        SharedPromise<> promise = ConstructShared<std::promise<void>>(mAllocator);
        Vector<BufferCopyCommand> cmds(mAllocator);
        for (auto& [srcBuffer, srcOffset, size, pid] : writes)
        {
            cmds.emplace_back(
                pid, srcBuffer, dst,
                RHICommandList::CopyBufferRegion{.srcOffset = srcOffset, .dstOffset = offset, .size = size});
            offset += size;
        }
        mBufferCopies.push({cmds.size(), {cmds, promise}});
        return promise;
    }
    SharedPromise<> StreamingPool::Write(Span<const char> data, RHITexture* dst, RHITextureLayout dstLayout,
                                         RHITextureAspectFlag aspect, uint32_t dstMip, uint32_t firstLayer)
    {
        CHECK_MSG(dst->mDesc.dimension == RHITextureDimension::E2D, "2D Textures ONLY");
        WriteList writes(mAllocator);

        std::unique_lock lock(mPageMutex);
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
            WritePages(lock, layerData, writes, rowPitch);
            // Same mip
            for (auto& [srcBuffer, srcOffset, size, pid] : writes)
            {
                int rows = size / rowPitch, offsetAll = offset / rowPitch, offsetRow = offsetAll % numRows;
                cmds.emplace_back(
                    pid, srcBuffer, dst, dstLayout,
                    RHICommandList::CopyImageRegion{.srcBufferOffset = static_cast<uint32_t>(srcOffset),
                                                    .dstLayer =
                                                        RHITextureSubresourceLayer{
                                                            .aspect = aspect,
                                                            .mipLevel = dstMip,
                                                            .baseArrayLayer = static_cast<uint32_t>(firstLayer + layer),
                                                            .layerCount = 1,
                                                        },
                                                    .dstOffset = {0, offsetRow, 0},
                                                    .extent = {extent.x, rows, 1}});
                offset += size;
            }
        }
        mTextureCopies.push({cmds.size(), {cmds, promise}});
        return promise;
    }
    StreamingPool::~StreamingPool() { mShutdown = true; }
    String StreamingPool::DbgGetStatistics() const
    {
        size_t allocated = 0, used = 0, refs = 0;
        for (size_t i = 0; i < mPageTop; i++)
        {
            allocated += mPages[i].capacity;
            used += mPages[i].top - mPages[i].mem;
            refs += mPageRefs[i];
        }
        return fmt::format("Pages: {}, Capacity: {} KB, Resident: {} KB, Refs: {}", mPageTop, allocated / 1024,
                           used / 1024, refs);
    }
    size_t StreamingPool::GetRefCounts() const
    {
        return std::reduce(mPageRefs.begin(), mPageRefs.end(), 0uLL);
    }
    void StreamingPool::Reset()
    {
        std::unique_lock lock(mPageMutex);
        CHECK_MSG(GetRefCounts() == 0, "Cannot reset StreamingPool with active references (count={})", GetRefCounts());
        mTransferCmds.clear();
        mPendingCompletions.clear();
        mPageTop = 0;
        mPageHeap.clear();
        mPageRefs.fill(0);
        for (auto& page : mPages)
        {
            page.buffer.Reset();
            page.mem = page.top = nullptr;
        }
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
            {
                std::unique_lock lock(mPageMutex);
                for (size_t pid = 0; pid < kStreamingMaxPages; pid++)
                    mPageRefs[pid] -= pages[pid];
                mResolvedCV.notify_one();
            }
            for (auto& promise : promises)
                promise->set_value();
            mPendingCompletions.pop_front();
        }
        // Schedule and submit
        if (mBufferCopies.empty() && mTextureCopies.empty())
            return;
        std::unique_lock lock(mPageMutex);
        Vector<SharedPromise<>> promises(mAllocator);
        auto& cmd = mTransferCmds.emplace_back(mCommandPool->CreateCommandList());
        PageRef refs{};
        cmd->Begin();
        int transferBudget = mDesc.maxTransferPerSubmit;
        while (!mBufferCopies.empty() && transferBudget)
        {
            auto [pri, elem] = mBufferCopies.top();
            mBufferCopies.pop();

            auto& [bccs, promise] = elem;
            Span<BufferCopyCommand> bccsSpan = bccs;
            auto writing = bccsSpan.SubSpan(0, std::min(transferBudget, static_cast<int>(bccsSpan.size())));
            auto remaining = bccsSpan.SubSpan(writing.size());
            for (auto const& [pid, src, dst, region] : writing)
            {
                cmd->CopyBuffer(src, dst, {region});
                refs[pid]++;
            }
            // Finished this entry?
            if (remaining.empty())
                promises.emplace_back(promise);
            else
            {
                // Enqueue again
                Vector<BufferCopyCommand> newBccs(remaining.size(), mAllocator);
                Ranges::copy(remaining, newBccs.begin());
                mBufferCopies.push({newBccs.size(), {newBccs, promise}});
            }
            transferBudget -= writing.size();
        }
        while (!mTextureCopies.empty() && transferBudget)
        {
            auto [pri, elem] = mTextureCopies.top();
            mTextureCopies.pop();

            auto& [tccs, promise] = elem;
            Span<TextureCopyCommand> tccsSpan = tccs;
            auto writing = tccsSpan.SubSpan(0, std::min(transferBudget, static_cast<int>(tccsSpan.size())));
            auto remaining = tccsSpan.SubSpan(writing.size());
            for (auto const& [pid, src, dst, dstLayout, region] : writing)
            {
                cmd->CopyBufferToImage(src, dst, dstLayout, region);
                refs[pid]++;
            }
            // Finished this entry?
            if (remaining.empty())
                promises.emplace_back(promise);
            else
            {
                // Enqueue again
                Vector<TextureCopyCommand> newTccs(remaining.size(), mAllocator);
                Ranges::copy(remaining, newTccs.begin());
                mTextureCopies.push({newTccs.size(), {newTccs, promise}});
            }
            transferBudget -= writing.size();
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
        mPendingCompletions.emplace_back(mSubmitCtr, refs, std::move(promises));
    }
    void StreamingPool::WorkerThread()
    {
        while (!mShutdown)
        {
            Submit();
            // std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    }
} // namespace Foundation::RenderCore
