#pragma once
#include "HeapAllocator.hpp"
namespace Foundation::Core {
    /**
     * @brief Alias for HeapAllocator with tracking enabled.
     */
    using DefaultAllocator = HeapAllocator<true>;
}
