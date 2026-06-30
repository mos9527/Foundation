#pragma once
#include <future>
#include <thread>
namespace Foundation::Core
{
    template<typename T = void> using Promise = std::promise<T>;
    template<typename T = void> using Future = std::future<T>;

    using CondVar = std::condition_variable;
    using Mutex = std::mutex;
    /**
     * @brief Alias of std::thread.
     * @note Consumers MUST ensure the thread is joined before destruction.
     */
    using Thread = std::thread;
}
