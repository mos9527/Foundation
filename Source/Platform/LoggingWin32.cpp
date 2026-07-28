#include "Logging.hpp"
#include <Core/Container.hpp>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>

namespace Foundation::Platform {

static void InitConsole()
{
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    SetConsoleOutputCP(CP_UTF8);

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE)
    {
        DWORD dwMode = 0;
        if (GetConsoleMode(hOut, &dwMode))
        {
            dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, dwMode);
        }
    }

    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    if (hErr != INVALID_HANDLE_VALUE)
    {
        DWORD dwMode = 0;
        if (GetConsoleMode(hErr, &dwMode))
        {
            dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hErr, dwMode);
        }
    }
}

void WriteLog(LogLevel level, const char* tag, double seconds, const char* formatted)
{
    InitConsole();
    auto str = Foundation::Core::Format("{}|+{:>5.5f}s|{} {}\n", level, seconds, tag, formatted);
    fprintf(stderr, "%s", str.c_str());
    fflush(stderr);
    if (IsDebuggerPresent())
    {
        auto windbg = Foundation::Core::Format("{}|+{:>5.5f}s|{} {}\n", format_as<false>(level), seconds, tag, formatted);
        OutputDebugStringA(windbg.c_str());
    }
}

void Print(const char* formatted, bool flush)
{
    InitConsole();
    fprintf(stdout, "%s", formatted);
    if (flush)
        fflush(stdout);    
}
} // namespace Foundation::Platform
