#pragma once
#include <Runtime/Core/Core.hpp>
#include <future>
/**
 * @brief Asynchronous programming primitives.
 */
namespace Foundation::Async
{
    template<typename T = void> using Future = std::future<T>;
    using Condition = std::condition_variable;
    using Mutex = std::mutex;
}