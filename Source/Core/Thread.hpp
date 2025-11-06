#pragma once
#include <future>
#include <thread>
namespace Foundation::Core
{
    template<typename T = void> using Promise = std::promise<T>;
    template<typename T = void> using Future = std::future<T>;

    template<typename T = void> using SharedPromise = SharedPtr<std::promise<T>>;

    using CondVar = std::condition_variable;
    using Mutex = std::mutex;
    /**
     * @brief Alias of std::jthread.
     * @note @ref Thread is joinable by default, and will be joined in the destructor.
     */
    using Thread = std::jthread;
}
