#include "Logging.hpp"

#include <cstdio>

namespace Foundation::Platform {

void WriteLog(LogLevel level, const char* tag, double seconds, const char* formatted)
{
    auto str = fmt::format("{}|+{:>5.5f}s|{} {}", level, seconds, tag, formatted);
    fprintf(stderr, "%s\n", str.c_str());
}

} // namespace Foundation::Platform
