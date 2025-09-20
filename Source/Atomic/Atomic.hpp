#pragma once
#include <atomic>
namespace Foundation::Atomic
{
    /**
     * @brief Alias of `std::atomic<T>`.
     */
    template<typename T> using Atomic = std::atomic<T>;
}
