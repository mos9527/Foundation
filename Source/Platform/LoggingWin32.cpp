#include "Logging.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>

namespace Foundation::Platform {

void WriteLog(LogLevel level, const char* tag, double seconds, const char* formatted)
{
    auto str = fmt::format("{}|+{:>5.5f}s|{} {}", level, seconds, tag, formatted);
    fprintf(stderr, "%s\n", str.c_str());
    if (IsDebuggerPresent())
    {
        auto windbg = fmt::format("{}|+{:>5.5f}s|{} {}\n", format_as<false>(level), seconds, tag, formatted);
        OutputDebugStringA(windbg.c_str());
    }
}

} // namespace Foundation::Platform
