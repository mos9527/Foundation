#pragma once
#include "HeapAllocator.hpp"
namespace Foundation::Core {
    /**
     * @brief Alias for HeapAllocatorMultiThreaded
     * Serves as a thin wrapper around mimalloc's default allocation behaviors,
     * with allocations being made through this allocator being tracked.
     */
    using DefaultAllocator = HeapAllocatorMultiThreaded;
}
