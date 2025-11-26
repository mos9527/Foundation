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
                         .coherent = true, /* No flush required */
                         .staging = true,},
            .usage = RHIBufferUsageBits::TransferSource,
            .size = page.capacity,
        });
        page.base = page.top = static_cast<char*>(page.buffer->Map());
        // Insert
        mPageHeap.insert({page.freeSize(), page.id});
    }
    size_t StreamingPool::PageWrite(int id, Span<const char> data, size_t& outOffset, RHIBuffer*& outBuffer, size_t alignment)
    {
        auto& page = mPages[id];
        char* aligned = reinterpret_cast<char*>(AlignUp(reinterpret_cast<size_t>(page.top), alignment));
        if (aligned - page.base >= page.capacity)
            return 0; // Not enough space
        const size_t oldSize = page.freeSize();
        // Aligned write
        page.top = aligned;
        outOffset = page.top - page.base;
        outBuffer = page.buffer.Get();
        size_t written = std::min(data.size(), page.freeSize());
        std::memcpy(page.top, data.data(), written);
        page.top += written;
        Maintain(id, oldSize, page.freeSize());
        return written;
    }
    void StreamingPool::PageReset(int id)
    {
        auto& page = mPages[id];
        const size_t oldSize = page.freeSize();
        page.top = page.base;
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
        LOG(Streaming, LogDebug, "StreamingPool collected {} pages", res);
        return res;
    }
    void StreamingPool::WritePages(std::unique_lock<Mutex>& lck, Span<const char> data, WriteList& outList, size_t alignment)
    {
        RHIBuffer* buf = nullptr;
        while (!data.empty())
        {
            auto* page = &mPages[GetPage()];
            size_t written = 0, offset = 0;
            while (true)
            {
                if ((written = PageWrite(page->id, data, offset, buf, alignment)))
                {
                    data = data.subspan(written);
                    break;
                }
                // Largest page does not hold. Alloc new or collect
                if (mPageTop < kStreamingMaxPages)
                    PageAlloc();
                else
                    Collect(lck);
                page = &mPages[GetPage()];
            }
            mPageRefs[page->id]++;
            outList.emplace_back(buf, offset, written, page->id);
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
    StreamingFuture StreamingPool::Write(Span<const char> data, RHIBuffer* dst, size_t offset)
    {
        WriteList writes(mAllocator);

        std::unique_lock lock(mPageMutex);
        WritePages(lock, data, writes, 4);
        Vector<BufferCopyCommand> cmds(mAllocator);
        for (auto& [srcBuffer, srcOffset, size, pid] : writes)
        {
            CHECK_MSG(offset + size <= dst->mDesc.size, "Buffer overflow in StreamingPool Write (at={}, size={}, free={})",
                      offset, size, dst->mDesc.size - offset);
            cmds.emplace_back(
                pid, srcBuffer, dst,
                RHICommandList::CopyBufferRegion{.srcOffset = srcOffset, .dstOffset = offset, .size = size});
            offset += size;
        }
        auto promise = ConstructShared<StreamingPromise>(mAllocator);
        mBufferCopies.emplace(cmds.size(), CmdEntry<BufferCopyCommand>{cmds, promise});
        return promise->get_future();
    }
    StreamingFuture StreamingPool::Write(Span<const char> data, RHITexture* dst, RHITextureAspectFlag aspect,
                                         uint32_t dstMip, uint32_t firstLayer)
    {
        CHECK_MSG(dst->mDesc.dimension == RHITextureDimension::E2D, "2D Textures ONLY");
        WriteList writes(mAllocator);

        std::unique_lock lock(mPageMutex);
        // https://docs.vulkan.org/guide/latest/image_copies.html
        // Texture writes are aligned to row pitch (assuming tightly packed)
        auto extent = dst->mDesc.extent;
        extent.x >>= dstMip, extent.y >>= dstMip;
        size_t numRows = extent.y;
        size_t rowPitch = extent.x * RHIResourceFormatSize(dst->mDesc.format);
        size_t layerSize = rowPitch * extent.y, numLayers = data.size() / layerSize;
        Vector<TextureCopyCommand> cmds(mAllocator);
        size_t offset = 0;
        for (size_t layer = 0; layer < numLayers; layer++)
        {
            Span layerData = data.subspan(layer * layerSize, layerSize);
            WritePages(lock, layerData, writes, rowPitch);
            // Same mip
            for (auto& [srcBuffer, srcOffset, size, pid] : writes)
            {
                int rows = size / rowPitch, offsetAll = offset / rowPitch, offsetRow = offsetAll % numRows;
                cmds.emplace_back(
                    pid, srcBuffer, dst, RHITextureLayout::TransferDst,
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
        auto promise = ConstructShared<StreamingPromise>(mAllocator);
        // Reliably triggers C1001 on MSVC. Deduct it ourselves.
        // (paraphrased) D:\a\_work\1\s\src\vctools\Compiler\CxxFE\sl\p1\c\CTAD.cpp#L372
        // mTextureCopies.emplace(cmds.size(), CmdEntry{cmds, promise});
        mTextureCopies.emplace(cmds.size(), CmdEntry<TextureCopyCommand>{cmds, promise});
        return promise->get_future();
    }
    StreamingPool::~StreamingPool() { mShutdown = true; }
    String StreamingPool::DbgGetStatistics() const
    {
        size_t allocated = 0, used = 0, refs = 0;
        for (size_t i = 0; i < mPageTop; i++)
        {
            allocated += mPages[i].capacity;
            used += mPages[i].top - mPages[i].base;
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
            page.base = page.top = nullptr;
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
        Vector<SharedPtr<StreamingPromise>> promises(mAllocator);
        auto& cmd = mTransferCmds.emplace_back(mCommandPool->CreateCommandList());
        PageRef refs{};
        cmd->Begin();
        int transferBudget = mDesc.maxTransferPerSubmit;
        while (!mBufferCopies.empty() && transferBudget)
        {
            auto [pri, elem] = mBufferCopies.top();
            mBufferCopies.pop();

            auto& [bccs, promise] = elem;
            Span bccsSpan = bccs;
            auto writing = bccsSpan.subspan(0, std::min(transferBudget, static_cast<int>(bccsSpan.size())));
            auto remaining = bccsSpan.subspan(writing.size());
            for (auto const& [pid, src, dst, region] : writing)
            {
                cmd->CopyBuffer(src, dst, {{region}});
                LOG(Streaming, LogDebug, "Push copy pid={} sz={} dst={} src={}", pid, region.size, region.dstOffset, region.srcOffset);
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
            Span tccsSpan = tccs;
            auto writing = tccsSpan.subspan(0, std::min(transferBudget, static_cast<int>(tccsSpan.size())));
            auto remaining = tccsSpan.subspan(writing.size());
            for (auto const& [pid, src, dst, dstLayout, region] : writing)
            {
                cmd->CopyBufferToImage(src, dst, dstLayout, {{region}});
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
        LOG(StreamingPool, LogDebug, "Batch Submit cmds={}", mTransferCmds.size());
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
        }
    }
} // namespace Foundation::RenderCore
