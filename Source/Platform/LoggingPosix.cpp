#include "Logging.hpp"

#include <cstdio>

namespace Foundation::Platform {

void WriteLog(LogLevel level, const char* tag, double seconds, const char* formatted)
{
    auto str = Format("{}|+{:>5.5f}s|{} {}", level, seconds, tag, formatted);
    fprintf(stderr, "%s\n", str.c_str());
}

void Print(const char* formatted, bool flush)
{
    fprintf(stdout, "%s", formatted);
    if (flush)
        fflush(stdout);
}
} // namespace Foundation::Platform
