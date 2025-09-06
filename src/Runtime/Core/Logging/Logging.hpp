#pragma once

#include <memory>
#include <exception>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/ringbuffer_sink.h>

namespace Foundation::Core {
    const size_t kMaxBacktraceLogMessages = 1000;
    extern std::shared_ptr<spdlog::sinks::dist_sink_mt> getLoggingSink();
    extern std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> getBacktraceSink();
    extern spdlog::logger* getLogger(const char* name);
}

#define LOG_GET_GLOBAL_SINK() \
    Foundation::Core::getLoggingSink()

#define LOG_GET_LOGGER(TAG) \
	Foundation::Core::getLogger(#TAG)

#define LOG_RUNTIME(TAG, LEVEL, ...) \
	SPDLOG_LOGGER_CALL(LOG_GET_LOGGER(TAG), spdlog::level::LEVEL, __VA_ARGS__)

#ifndef _DEBUG
#define LOG_DEBUG(TAG, LEVEL, ...) (void)0
#else

#define LOG_DEBUG(TAG, LEVEL, ...) \
	SPDLOG_LOGGER_CALL(LOG_GET_LOGGER(TAG), spdlog::level::LEVEL, __VA_ARGS__)
#endif

#define CHECK(expr) if(!(expr)) { \
    LOG_RUNTIME(Core, err, "Check failed: {}", #expr); \
    throw std::runtime_error( #expr ); \
}

#define CHECK_MSG(expr, format_str, ...) if(!(expr)) { \
    LOG_RUNTIME(Core, err, "Check failed: {} - " format_str, #expr, __VA_ARGS__); \
    throw std::runtime_error( fmt::format("{} - " format_str, #expr, __VA_ARGS__) ); \
}
