#pragma once
#include <Core/Logging.hpp>

namespace Foundation::Platform {

// Platform sink for a fully composed log record (timing owned by Core).
void WriteLog(LogLevel level, const char* tag, double seconds, const char* formatted);

} // namespace Foundation::Platform
