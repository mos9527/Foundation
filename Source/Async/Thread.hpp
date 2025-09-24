#pragma once
#include <thread>
#include <Core/Core.hpp>
namespace Foundation::Async
{
    /**
     * @brief Alias of std::jthread.
     * @note @ref Thread is joinable by default, and will be joined in the destructor.
     */
    using Thread = std::jthread;
    /**
     * @brief Sets the name of the current thread.
     * @param name Name to set. The name will be truncated if it exceeds the platform-specific limit.
     * @note This is a no-op on platforms that do not support setting thread names.
     */
    void setThreadName(Thread& thread, Core::StringView name);
}