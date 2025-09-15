#pragma once
#include <RHICore/Resource.hpp>
#include <Core/Allocator/Allocator.hpp>
#include <Core/Container/FreeList.hpp>
#include <Rendering/Renderer.hpp>

namespace Foundation::Rendering {
    using namespace Foundation::RHI;
    using namespace Foundation::Core;
    using DataHandle = uint32_t;
    using StagingHandle = uint32_t;
    // [Source Buffer (staging), Dest Buffer, CopyRegion]
    using BufferStagingItem = Tuple<RHIBuffer*, RHIBuffer*, RHICommandList::CopyBufferRegion>;
    using BufferStagingList = Vector<BufferStagingItem>;
    using BufferCopyList = Vector<RHICommandList::CopyBufferRegion>;
    /**
    * @brief Helper class for no contention staging buffer management.
    */
    class Staging;
    /**
    * @brief Bump-only allocation buffer used for staging data to be transferred to GPU.
    */
    class StagingBuffer;
    class StagingBuffer {
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
        void Reset();

        [[nodiscard]] RHIBuffer* GetBuffer() const { return m_buffer.Get(); }
    };
    /**
     * @brief Two-pointers in-place coalescing of copy regions
     *
     * Drivers _may_ do this too - but we can't rely on it. And pushing copy commands
     * in itself has quite some overhead.
     */
    extern BufferStagingList& CoalesceBufferStaging(BufferStagingList& res);
    class Staging : public RHIObject /* pinned */
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

        State m_state{State::Idle};
        uint32_t m_currentSync{0};
        BufferStagingList m_bufferStagings;

        RHIDeviceIdleGuard m_idleGuard;
    public:
        /**
         * @brief Create the staging arena buffers.
         * @param device
         * @param allocator
         * @param numSwaps Max number of frames in flight, can be greater than the actual number of swaps.
         * @param stagingBudget Size of each staging buffer in bytes.
         */
        Staging(RHIDevice* device, Allocator* allocator, uint32_t numSwaps, size_t stagingBudget);
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
        void TransferBuffer(RHIBuffer* dst_buffer, size_t offset, Span<const char> data, size_t alignment = 4);
        /**
         * @brief Ends the transfer state.
         * @note Transfer is _not_ performed here. A subsequent pass must be recorded to perform the actual copy.
         */
        void EndTransfer();
    };
    ENUM_NAME_CONV_BEGIN(Staging::State)
    ENUM_NAME(Idle)
    ENUM_NAME(Transfer)
    ENUM_NAME_CONV_END()
}
