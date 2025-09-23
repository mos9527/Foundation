#pragma once
#include <Core/Core.hpp>
#include <future>
/**
 * @brief Asynchronous programming primitives.
 */
namespace Foundation::Async
{
    template<typename T = void> using Promise = std::promise<T>;
    template<typename T = void> using Future = std::future<T>;

    template<typename T = void> using SharedPromise = Core::SharedPtr<std::promise<T>>;

    using Condition = std::condition_variable;
    using Mutex = std::mutex;
    /**
     * @brief Alias for `std::counting_semaphore`.
     */
    using Semaphore = std::counting_semaphore<>;
}