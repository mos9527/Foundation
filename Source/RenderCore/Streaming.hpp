#pragma once
#include <RHICore/Resource.hpp>
#include <RHICore/Command.hpp>
#include <Core/ThreadPool.hpp>
#include <Core/Container.hpp>
namespace Foundation::RenderCore
{
    using namespace RHI;
    constexpr static size_t kStreamingPageSize = 4 * 1024 * 1024; // 4 MiB
    constexpr static size_t kStreamingMaxPages = 128; // Max 128 * 4 MiB
    struct BufferCopyCommand
    {
        RHIBuffer* src;
        RHIBuffer* dst;
        RHICommandList::CopyBufferRegion region;
    };
    struct TextureCopyCommand
    {
        RHIBuffer* src;
        RHITexture* dst;
        RHITextureLayout dstLayout;
        RHICommandList::CopyImageRegion region;
    };
    /**
     * @brief Simple dynamic pool + linear allocator for streaming data to the GPU.
     */
    class StreamingPool
    {
        using PageRef = Array<int, kStreamingMaxPages>;
        static PageRef& AddRef(PageRef& lhs, PageRef const& rhs);
        static PageRef& DecRef(PageRef& lhs, PageRef const& rhs);
        using WriteEntry = Tuple<RHIBuffer*, size_t, size_t>; // buffer, offset, size
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
        // Commit buffer to pages, possibly touching multiple ones.
        void Write(Span<const char> data, PageRef& outPages, WriteList& outList);

        PageRef mPageRefs{};
        // Perform page GC sweep to reclaim space
        void Collect();

        template<typename T> using CmdEntry = Tuple<Vector<T>, PageRef, SharedPromise<>>;
        Vector<CmdEntry<BufferCopyCommand>> mBufferCopies;
        Vector<CmdEntry<TextureCopyCommand>> mTextureCopies;

        mutable Mutex mScheduleMutex;
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
    public:
        StreamingPool(RHIDevice* device, Allocator * allocator);
        /**
         * @brief Schedules buffer upload.
         * @note Destination buffer MUST have been created with @ref RHIBufferUsage::TransferDst, and
         *       is *shared* across at least the transfer queue.
         */
        SharedPromise<> Write(Span<const char> data, RHIBuffer* dst, size_t offset);

        ~StreamingPool();

        String DbgGetStatistics() const;
    };

    class StreamingTexture
    {
        RHIDeviceScopedObjectHandle<RHITexture> mTexture;
    };
}