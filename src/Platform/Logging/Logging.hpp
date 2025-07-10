#pragma once
#include <spdlog/sinks/sink.h>
namespace Foundation {
    namespace Platform {
        extern spdlog::sink_ptr GetPlatformDebugLoggingSink();
    }
}
