#pragma once
#include <Core/Allocator/Allocator.hpp>
#include <Core/Container/FreeList.hpp>
#include <RHICore/Resource.hpp>
#include <Rendering/Renderer.hpp>

namespace Foundation::Rendering
{
    using namespace Foundation::RHI;
    using namespace Foundation::Core;
    // [Source Buffer (staging), Dest Buffer, CopyRegion]
    using BufferStagingItem = Tuple<RHIBuffer*, RHIBuffer*, RHICommandList::CopyBufferRegion>;
    using BufferStagingList = Vector<BufferStagingItem>;
    using BufferCopyList = Vector<RHICommandList::CopyBufferRegion>;
    /**
     * @brief Bump-only allocation buffer used for staging data to be transferred to GPU.
     */
    class StagingBuffer
    {
        Allocator* m_allocator;
        RHIDevice* m_device;
        RHIDeviceScopedObjectHandle<RHIBuffer> m_buffer;
        void* m_mapped;
        size_t m_size;
        size_t m_offset = 0;

    public:
        StagingBuffer(Allocator* allocator, RHIDevice* device, size_t size);
        /**
         * @brief Writes data to the staging buffer, returning the offset of the data in the buffer.
         * @return Offset of the data in the buffer. Throws std::bad_alloc if there's not enough space.
         */
        size_t Write(Span<const char> data, size_t alignment);
        /**
         * @brief Seeks the current offset to the given offset, aligned to the given alignment.
         */
        void Seek(size_t offset, size_t alignment);
        /**
         * @brief Resets the staging buffer, allowing it to be reused.
         * No GPU-side data transfer is performed.
         */
        size_t Tell() const { return m_offset; }
        size_t Size() const { return m_size; }
        size_t FreeSize() const { return m_size - m_offset; }
        void Reset();

        [[nodiscard]] RHIBuffer* GetBuffer() const { return m_buffer.Get(); }
    };
    /**
     * @brief Two-pointers in-place coalescing of copy regions
     *
     * O(N log N) due to sorting. Though N should be quite small in practice
     * as we'd also coalesce neighboring transfers on-the-fly in @ref StagedBuffer::Transfer
     *
     * Drivers _may_ do this too - but we can't rely on it, plus pushing copy commands
     * in itself has quite some overhead.
     */
    extern BufferStagingList& CoalesceBufferStaging(BufferStagingList& res);
    /**
     * @brief Helper class for GPU contention-free staged buffer updates.
     *
     * @todo Not thread-safe on the CPU yet.
     *
     * This creates a GPU-only local buffer, and multiple staging buffers per swap.
     *
     * For one-shot, static content - you don't need this. Get a staging buffer,
     * write to it, and do the transfer right away. Wait until the queue is done.
     *
     * This is here for _dynamic_ content updates that can be performed asynchronously
     * without stalling the CPU or GPU for each update.
     *
     * For trivial, small updates, you might want to consider using Push Constants instead.
     * See also @ref createStagedBufferUpdatePass
     */
    class StagedBuffer : public RHIObject /* pinned */
    {
    public:
        enum class State
        {
            Idle,
            Transfer,
        };

    private:
        RHIDevice* m_device{nullptr};
        Allocator* m_allocator{nullptr};

        Vector<StagingBuffer> m_stagingBuffers;
        RHIDeviceScopedObjectHandle<RHIBuffer> m_buffer;

        State m_state{State::Idle};
        uint32_t m_currentSync{0};
        BufferStagingList m_bufferStagings;

        std::mutex m_transferMutex;
        RHIDeviceIdleGuard m_idleGuard;

    public:
        /**
         * @brief Create the staging arena buffers.
         * @param numSwaps Max number of frames in flight, can be greater than the actual number of swaps.
         * @param desc
         */
        StagedBuffer(RHIDevice* device, Allocator* allocator, uint32_t numSwaps, RHIBufferDesc const& desc, size_t stagingBudget = kFullSize);
        /**
         * @brief Gets the GPU-only backing buffer.
         */
        RHIBuffer* GetBuffer() { return m_buffer.Get(); }
        /**
         * @brief Resets the staging buffer and aborts all pending data updates.
         *
         * @note This MUST be called after the @ref Renderer's @ref BeginExecute,
         * and before @ref ExecuteFrame as the staging buffer is tied to the frame fences.
         *
         * @param rendererSync The @ref Renderer::GetSync() value acquired after @ref Renderer::BeginExecute()
         */
        void BeginTransfer(uint32_t rendererSync);
        /**
         * @brief Gets the current staging buffer for data uploads.
         *
         * This MUST be called between @ref BeginTransfer and @ref EndTransfer.
         */
        RHIBuffer* GetStagingBuffer();
        /**
         * @brief Schedules a data upload to the given buffer at the given offset.
         *
         * This MUST be called between @ref BeginTransfer and @ref EndTransfer.
         *
         * @note No transfer is performed until @ref EndTransfer is called, and its command list is executed.
         * @note Overlapping transfers is undefined behavior.
         */
        void Transfer(size_t dst_offset, Span<const char> data, size_t alignment = 4);
        /**
         * @brief Ends the transfer state, and pushes optimized copy commands to the given command list.
         */
        void EndTransfer();
        /**
         * @brief Check if there are any pending updates to be performed.
         */
        bool HasUpdates() const { return !m_bufferStagings.empty(); }
        /**
         * @brief Push the scheduled uploads onto the command list.
         *
         * This MUST be called after @ref EndTransfer, and before the command list is submitted.
         *
         * Once executed, the previously scheduled uploads are considered done, and will be flushed.
         *
         * @note No resource transitions are performed here. It's up to the caller to ensure correct access patterns.
         *       Thus, it's recommended to use @ref createStagedBufferUpdatePass to create a pass that performs
         *       everything above with optimal synchronization.
         */
        void Update(RHICommandList* cmd);
    };
    /**
     * @brief Convenient functional wrapper to create a staged buffer update pass.
     *
     * This creates a pass that performs the following:
     * - Binds the destination buffer as a copy destination.
     * - Calls StagedBuffer::Update to perform and flush all pending updates.
     * - Skipped when there are no more pending updates.
     *
     * Updates should be scheduled between @ref Renderer's @ref BeginExecute and @ref ExecuteFrame,
     * as per @StagedBuffer documentation.
     *
     * The created pass will have the name "Staged Buffer [name]".
     *
     * @param renderer Renderer to create the pass in.
     * @param staged StagedBuffer to perform updates from.
     * @param name Name of the pass.
     * @param outBufferHandle Output parameter to receive the created buffer handle.
     * @param queue Queue to prefer running this pass in - this is a hint, and might be ignored if async compute is disabled.
     * @return Handle to the created pass.
     */
    inline auto* createStagedBufferUpdatePass(Renderer* renderer, StagedBuffer* staged, StringView name,
                                       ResourceHandle& outBufferHandle,
                                       RHIDeviceQueueType queue = RHIDeviceQueueType::Graphics)
    {
        outBufferHandle = renderer->CreateResource(fmt::format("StagedBuffer {}", name), staged->GetBuffer());
        return createPass(renderer, name, queue,
            [=](PassHandle self, Renderer* r)
            {
                r->BindBufferCopyDst(self, outBufferHandle);
            },
            [&](RHICommandList* cmd)
            {
                staged->Update(cmd);
            },
            [=](PassHandle self, Renderer* r)
            {
                return !staged->HasUpdates();
            });
    }

    ENUM_NAME_CONV_BEGIN(StagedBuffer::State)
    ENUM_NAME(Idle)
    ENUM_NAME(Transfer)
    ENUM_NAME_CONV_END()
} // namespace Foundation::Rendering
