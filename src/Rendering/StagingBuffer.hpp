#pragma once
#include <RHICore/Resource.hpp>
#include <Core/Allocator/Allocator.hpp>
#include <Core/Container/FreeList.hpp>
#include <Rendering/Renderer.hpp>

namespace Foundation::Rendering {
    using namespace Foundation::RHI;
    using namespace Foundation::Core;
    using DataHandle = uint32_t;

    class StagingBuffer;
    /**
     * @brief Helper class that manages a GPU buffer with dynamic allocations, and staging data transfers.
     */
    class DataBuffer {
        using AllocationList = FreeList<DataHandle, RHIBuffer::Arena::Allocation>;
        using StagingPair = Pair<StagingBuffer*, RHICommandList::CopyBufferRegion>;
        using StagingList = Vector<StagingPair>;
        using CopyList = Vector<RHICommandList::CopyBufferRegion>;
        // Greedily coalesce m_staging to reduce the number of copy commands.
        StagingList& CoalesceStaging();

        String m_name;

        Allocator* m_allocator;
        RHIDeviceScopedObjectHandle<RHIBuffer> m_buffer;
        AllocationList m_allocations;

        StagingList m_staging;
        StagingList m_coalescedStaging;

        Optional<uint32_t> m_initClear;
    public:
        /**
         * @brief Initializes the DataBuffer with the given budget and usage.
         * @param budget Max size of the buffer in bytes.
         * @param initClear The value to clear the buffer to on the first Update() call. If not set, the buffer is
         * uninitialized.
         */
        DataBuffer(StringView name, Allocator* allocator, RHIDevice* device, size_t budget, RHIBufferUsageBits usage, Optional<uint32_t> initClear = {});
        /**
         * @brief Pushes data to the buffer, returning a handle to the allocation.
         * @return Handle to the allocation.
         * @note You must ensure there's NO contention for staging buffer usage between Record() calls.
         *        See @ref Scene for an example.
         * The staging buffer should be the same one used in the subsequent @ref Record() call.
         * No GPU-side data transfer is performed, until @ref Record() is called.
         */
        DataHandle PushData(StagingBuffer* staging, Span<const char> data, size_t alignment = 16);
        /**
         * @brief Updates the data associated with the given handle.
         * @note You must ensure there's NO contention for staging buffer usage between Record() calls.
         *        See @ref Scene for an example.
         * The staging buffer should be the same one used in the subsequent @ref Record() call.
         * No GPU-side data transfer is performed, until @ref Record() is called.
         */
        void UpdateData(StagingBuffer* staging, DataHandle handle, Span<const char> data, size_t alignment = 16);
        /**
         * @brief Frees the allocation associated with the given handle.
         *
         * No GPU-side data transfer is performed, until @ref Record() is called.
         */
        void FreeData(DataHandle handle);
        /**
         * @brief Queries the size and offset of the allocation associated with the given handle.
         * @return [size, offset] in bytes of the allocation.
         */
        [[nodiscard]] Pair<size_t, size_t> Query(DataHandle handle) const;
        [[nodiscard]] RHIDeviceObjectHandle<RHIBuffer> GetBuffer() const { return m_buffer; }
        /**
         * @brief Finalize all previous PushData/UpdateData/FreeData calls, and record the copy commands to the given
         * command list.
         * @note *No* resource barriers are recorded. You must ensure the buffer is in the correct state before/after
         * the copy commands. e.g. through using the @ref Renderer and import the buffer with @ref CreateResource(), and
         * have Record() as a pass's Record() callback. See @ref Scene for an example.
         * @param cmd The command list to record the copy commands to.
         */
        void Update(RHICommandList* cmd);
        /**
         * @brief Abort all previous PushData/UpdateData/FreeData calls.
         * No GPU-side data transfer is performed, and subsequent Record() calls will be no-ops.
         */
        void Abort();
        /**
         * @brief Reset the DataBuffer, freeing all allocations.
         * @note This is a no-op on the GPU side.
         */
        void Reset();
        /**
         * @return Whether there are pending updates to be recorded.
         */
        bool HasUpdates() const { return !m_staging.empty(); }
    };
    /**
     * @brief Bump-only allocation buffer used for staging data to be transferred to GPU.
     */
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

        [[nodiscard]] RHIDeviceObjectHandle<RHIBuffer> GetBuffer() const { return m_buffer; }
    };
}
