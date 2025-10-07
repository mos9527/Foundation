#pragma once
#include <Async/Future.hpp>
#include <Core/Core.hpp>
#include <Core/Pool.hpp>
#include <vk_mem_alloc.h>
namespace Foundation::Rendering
{
    using namespace Core;
    using VirtualAllocation = uint32_t;
    // [Raw Offset, Size]
    constexpr VirtualAllocation kInvalidVirtualAllocation = ~0u;
    /**
     * @brief Thread-safe wrapper around VulkanMemoryAllocator's Virtual Allocator interface
     *
     * @note Despite the name, this does not limit you to only Vulkan memory allocations. VMA's virtual
     *       allocations are just offsets into a large virtual memory arena that you can use for any purpose.
     *       The backing memory doesn't have to be real in that sense.
     *
     * You can use this to allocate offsets into a large GPU buffer, or even as a general purpose CPU memory
     * allocator - the usage of the latter is discouraged in favor of @ref HeapAllocator and @ref StackAllocator
     *
     * See also
     *  - https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/virtual_allocator.html
     */
    class VirtualAllocator
    {
        const size_t mSize;
        VmaVirtualBlock mBlock{};
        Pool<VirtualAllocation, Tuple<size_t, size_t, VmaVirtualAllocation>> mAllocs;
        // VMA virtual allocs are not thread-safe. Need guards.
        Async::Mutex mMutex;

    public:
        /**
         * @brief Construct a @ref VirtualAllocator instance
         * @param size Size of the virtual memory arena
         * @param alloc @ref Allocator for virtual allocator state management
         */
        VirtualAllocator(size_t size, Allocator* alloc);
        /**
         * @brief Allocate memory of size and alignment
         * @param size Size of the allocation
         * @param alignment Alignment of the allocation
         * @return Opaque @ref VirtualAllocation handle that you can use with @ref Query, @ref Free
         */
        VirtualAllocation Allocate(size_t size, size_t alignment);
        /**
         * @brief Free a previous allocation
         * @param handle Previously acquired allocation from the same allocator through @ref Allocate
         */
        void Free(VirtualAllocation handle);
        /**
         * @brief Query the offset and size of a previous allocation
         * @param handle Previously acquired allocation from the same allocator through @ref Allocate
         * @return Pair of [Raw Offset, Raw Size]
         */
        Pair<size_t, size_t> Query(VirtualAllocation handle);
        size_t QuerySize(size_t handle)
        {
            auto [off, sz] = Query(handle);
            return sz;
        };
        size_t QueryOffset(size_t handle)
        {
            auto [off, sz] = Query(handle);
            return off;
        }
        ~VirtualAllocator();
    };
} // namespace Foundation::Rendering
