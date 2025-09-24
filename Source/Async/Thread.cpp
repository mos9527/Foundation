#include "Thread.hpp"
namespace Foundation::Async
{
    void setThreadName(Thread& thread, Core::StringView name)
    {
        auto handle = thread.native_handle();
    }

}