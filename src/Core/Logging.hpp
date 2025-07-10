#pragma once
#include <Core/Core.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/dist_sink.h>
namespace Foundation {
	namespace Core {
		namespace Logging {
            // Returns a pointer to the global logging sink.
            // dist_sink can be multiplexed to multiple loggers.
            extern std::shared_ptr<spdlog::sinks::dist_sink_mt> GetLoggingSink();
			// Returns a logger with the given name, creating it if it does not exist.
			// The lifetime of the logger is managed by spdlog, so you do not need to delete it.
            // By default, the logger will log to a ring buffer sink with a size of 1000 messages.
			extern spdlog::logger* GetLogger(const char* name);
			// Destroys the logger with the given name, if it exists.
			extern void DestroyLogger(const char* name);
		}
	}
}
#define LOG_GET_GLOBAL_SINK() \
    Foundation::Core::Logging::GetLoggingSink()

#define LOG_GET_LOGGER(TAG) \
	Foundation::Core::Logging::GetLogger(#TAG)

#define LOG_RUNTIME(TAG, LEVEL, ...) \
	SPDLOG_LOGGER_CALL(LOG_GET_LOGGER(TAG), spdlog::level::LEVEL, __VA_ARGS__)

#ifndef _DEBUG
#define LOG_DEBUG(TAG, LEVEL, ...) (void)0
#else
#define LOG_DEBUG(TAG, LEVEL, ...) \
	SPDLOG_LOGGER_CALL(LOG_GET_LOGGER(TAG), spdlog::level::LEVEL, __VA_ARGS__)
#endif
