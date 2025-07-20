#pragma once
#include "HeapAllocator.hpp"
namespace Foundation::Core {
    /// <summary>
    /// Alias for HeapAllocatorMultiThreaded
    /// Serves as a thin wrapper around mimalloc's default allocation behaviors,
    /// with allocations being made through this allocator being tracked.
    /// </summary>
    using DefaultAllocator = HeapAllocatorMultiThreaded;
}
