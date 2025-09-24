#pragma once
#include <atomic>
/**
 * @brief Bounded, lock-free atomic primitives and implementations of data structures.
 *
 * References:
 * - Dmitry Vyukov's blog on lock-free algorithms and data structures
 *   - https://www.1024cores.net/home/lock-free-algorithms/introduction
 * - CppCon 2014: Herb Sutter "Lock-Free Programming (or, Juggling Razor Blades), Part I"
 *   - https://www.youtube.com/watch?v=c1gO9aB9nbs
 * - Single Producer Single Consumer Lock-free FIFO From the Ground Up - Charles Frasch - CppCon 2023
 *   - https://www.youtube.com/watch?v=K3P_Lmq6pw0
 * - Djordje Nedic's lockfree
 *   - https://github.com/DNedic/lockfree
 * - std::memory_order on cppreference
 *   - https://en.cppreference.com/w/cpp/atomic/memory_order.html
 * - Tracy Profiler's various lockfree implementations
 *   - https://github.com/wolfpld/tracy/blob/master/public/client/tracy_concurrentqueue.h
 */
namespace Foundation::Atomics
{
    /**
     * @brief Alias of `std::atomic<T>`.
     */
    template<typename T> using Atomic = std::atomic<T>;
}
