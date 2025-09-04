#pragma once

#include <memory>
#include <exception>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/ringbuffer_sink.h>

namespace Foundation::Core {
    const size_t kMaxBacktraceLogMessages = 1000;
    extern std::shared_ptr<spdlog::sinks::dist_sink_mt> GetLoggingSink();
    extern std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> GetBacktraceSink();
    extern spdlog::logger* GetLogger(const char* name);
}

#define LOG_GET_GLOBAL_SINK() \
    Foundation::Core::GetLoggingSink()

#define LOG_GET_LOGGER(TAG) \
	Foundation::Core::GetLogger(#TAG)

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
