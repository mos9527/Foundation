#pragma once
#include <Platform/Logging.hpp>

#include <spdlog/sinks/msvc_sink.h>
namespace Foundation {
    namespace Platform {
        spdlog::sink_ptr GetPlatformDebugLoggingSink() {
            return std::make_shared<spdlog::sinks::msvc_sink_mt>();
        }
    }
}
