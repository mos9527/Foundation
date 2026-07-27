#pragma once
#include <Core/Logging.hpp>

namespace Foundation::Platform {
void WriteLog(LogLevel level, const char* tag, double seconds, const char* formatted);
void Print(const char* formatted, bool flush);
} // namespace Foundation::Platform
