#include <Platform/Logging.hpp>

using namespace Foundation;
using namespace Core;

Mutex gLogImplMutex;
void Foundation_LogImpl(LogLevel level, const char* tag, const char* formatted)
{
    static auto init = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now() - init;
    auto seconds = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() / 1e9;
    std::unique_lock lock(gLogImplMutex);
    Platform::WriteLog(level, tag, seconds, formatted);
}
