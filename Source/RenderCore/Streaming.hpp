#pragma once
#include <RHICore/Resource.hpp>
#include <RHICore/Command.hpp>
#include <Core/ThreadPool.hpp>
#include <Core/Container.hpp>
namespace Foundation::RenderCore
{
    using namespace RHI;
    constexpr static size_t kStreamingMaxPages = 128; // Max 128 * 1 MiB

    struct BufferCopyCommand
    {
        int pid;
        RHIBuffer* src;
        RHIBuffer* dst;
        RHICommandList::CopyBufferRegion region;
    };
    struct TextureCopyCommand
    {
        int pid;
        RHIBuffer* src;
        RHITexture* dst;
        RHITextureLayout dstLayout;
        RHICommandList::CopyImageRegion region;
    };
    /**
     * @brief Simple dynamic pool + linear allocator for streaming data to the GPU.
     *        Thread-safety is guaranteed for public methods.
     * @note  GPU memory once allocated will NOT be freed until destruction of the pool,
     *        or @ref Reset is called (which requires no active references).
     */
    class StreamingPool
    {
        using PageRef = Array<int, kStreamingMaxPages>;
        using WriteEntry = Tuple<RHIBuffer*, size_t, size_t, int>; // buffer, offset, size, page ID
        using WriteList = Vector<WriteEntry>;

        RHIDevice* const mDevice;
        Allocator* const mAllocator;
        struct StagingPage
        {
            int id = 0;
            RHIDeviceScopedObjectHandle<RHIBuffer> buffer;
            size_t capacity;
            char *mem = nullptr, *top = nullptr; // Mapped memory for linear allocation
            [[nodiscard]] constexpr size_t freeSize() const { return capacity - (top - mem); }
        };
        Array<StagingPage, kStreamingMaxPages> mPages{};
        size_t mPageTop = 0;
        // [free size, index] Max heap
        Set<Pair<int, int>> mPageHeap;
        // Maintain max heap with updated page at pageIndex
        void Maintain(int id, size_t oldSize, size_t newSize);
        // Allocate a new page
        void PageAlloc();
        // Write to page
        void PageWrite(int id, Span<const char> data, size_t& outOffset, RHIBuffer*& outBuffer);
        // Reset page allocations
        void PageReset(int id);
        // Retrieve the most available page
        int GetPage();
        /**
         * @brief Commit buffer to pages, possibly touching multiple ones.
         * @param alignment Size alignment for each write in @ref WriteList
         */
        void WritePages(std::unique_lock<Mutex>& lck, Span<const char> data, WriteList& outList, size_t alignment);

        PageRef mPageRefs{};
        // Perform page GC sweep to reclaim space
        int Collect(std::unique_lock<Mutex>& lck);

        template<typename T> using CmdEntry = Tuple<Vector<T>, SharedPromise<>>;

        // Schedule copies that's more likely to complete early (less command/pages touched) first
        // with a min-heap
        constexpr static auto kCopyPred = [](auto const& lhs, auto const& rhs){ return lhs.first < rhs.first; };
        PriorityQueue<Pair<size_t, CmdEntry<BufferCopyCommand>>, decltype(kCopyPred)> mBufferCopies;
        PriorityQueue<Pair<size_t, CmdEntry<TextureCopyCommand>>, decltype(kCopyPred)> mTextureCopies;

        mutable Mutex mPageMutex;
        RHIDeviceQueue* mTransferQueue;
        RHIDeviceScopedObjectHandle<RHICommandPool> mCommandPool;

        RHIDeviceScopedObjectHandle<RHIDeviceSemaphore> mTransferSemaphore;
        Vector<RHICommandPoolScopedHandle<RHICommandList>> mTransferCmds;

        size_t mSubmitCtr = 0;
        using CompletionEntry = Tuple<size_t, PageRef, Vector<SharedPromise<>>>;
        Deque<CompletionEntry> mPendingCompletions;
        /**
         * @brief Submits GPU side transfers.
         */
        void Submit();

        bool mShutdown{false};
        void WorkerThread();
        Thread mWorker;

        CondVar mResolvedCV;
    public:
        struct StreamingPoolDesc
        {
            // Size per page.
            // A max total of 128 pages are allowed.
            // Max page count * page size = max resident staging memory usage,
            // your assets should be streamed in chunks smaller than that.
            size_t streamingPageSize = 1u << 20;
            // Max transfer commands to be executed per submit
            // Effectively limits max concurrent transfers - use a lower
            // value for lower latency, higher value for higher throughput
            int maxTransferPerSubmit = 16;
        };
        const StreamingPoolDesc mDesc;
        StreamingPool(RHIDevice* device, Allocator * allocator, StreamingPoolDesc const& desc);
        ~StreamingPool();
        /**
         * @brief Schedules buffer upload.
         * @note Destination buffer MUST have been created with @ref RHIBufferUsage::TransferDst, and
         *       is *shared* across at least the transfer queue.
         */
        SharedPromise<> Write(Span<const char> data, RHIBuffer* dst, size_t offset);
        /**
         * @brief Schedules texture upload.
         * @note Destination texture MUST have been created with @ref RHITextureUsage::TransferDst, and
         *       is *shared* across at least the transfer queue.
         */
        SharedPromise<> Write(Span<const char> data, RHITexture* dst, RHITextureLayout dstLayout,
                              RHITextureAspectFlag aspect, uint32_t dstMip, uint32_t firstLayer);
        /**
         * @brief Get total active reference counts. A non-zero value indicates there are active GPU transfers.
         */
        size_t GetRefCounts() const;
        /**
         * @brief Frees all allocated pages. Can only be called when there are no active references.
         */
        void Reset();
        ///
        String DbgGetStatistics() const;
    };
}