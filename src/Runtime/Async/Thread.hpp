#include <thread>
namespace Foundation::Async
{
    /**
     * @brief Alias of std::jthread.
     * @note @ref Thread is joinable by default, and will be joined in the destructor.
     */
    using Thread = std::jthread;
}