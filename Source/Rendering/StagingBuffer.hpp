#pragma once
#include <Core/Allocator.hpp>
#include <RenderCore/Renderer.hpp>

namespace Foundation::Rendering
{
    using namespace RHI;
    using namespace Core;
    using namespace RenderCore;
    // [Source Buffer (staging), Dest Buffer, CopyRegion]
    using BufferStagingItem = Tuple<RHIBuffer*, RHIBuffer*, RHICommandList::CopyBufferRegion>;
    using BufferStagingList = Vector<BufferStagingItem>;
    using BufferCopyList = Vector<RHICommandList::CopyBufferRegion>;
    /**
     * @brief Bump-only allocation buffer used for staging data to be transferred to GPU.
     */
    class StagingBuffer
    {
        Allocator* mAllocator;
        RHIDevice* mDevice;
        RHIDeviceScopedObjectHandle<RHIBuffer> mBuffer;
        void* mMapped;
        size_t mSize;
        size_t mOffset = 0;

    public:
        StagingBuffer(RHIDevice* device, size_t size, Allocator* allocator);
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
        [[nodiscard]] size_t Tell() const { return mOffset; }
        [[nodiscard]] size_t Size() const { return mSize; }
        [[nodiscard]] size_t FreeSize() const { return mSize - mOffset; }
        void Reset();

        [[nodiscard]] RHIBuffer* GetBuffer() const { return mBuffer.Get(); }
    };
} // namespace Foundation::Rendering
