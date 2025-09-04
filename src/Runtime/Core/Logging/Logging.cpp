#include "Logging.hpp"

#include <mutex>
#include <spdlog/sinks/stdout_color_sinks.h>

#if _WIN32
#include <spdlog/sinks/msvc_sink.h>
#endif
namespace Foundation::Core {
    static bool g_Initialized = false;
    static std::shared_ptr<spdlog::sinks::dist_sink_mt> g_LoggingSink = nullptr;
    static std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> g_BacktraceSink = nullptr;    
    std::recursive_mutex g_LoggingSinkMutex;

    std::shared_ptr<spdlog::sinks::dist_sink_mt> GetLoggingSink() {
        std::scoped_lock lck(g_LoggingSinkMutex);        
        if (!g_Initialized) {
            g_LoggingSink = std::make_shared<spdlog::sinks::dist_sink_mt>();
            g_BacktraceSink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(kMaxBacktraceLogMessages);
            g_LoggingSink->add_sink(g_BacktraceSink);
            g_LoggingSink->add_sink(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
#if _WIN32
            g_LoggingSink->add_sink(std::make_shared<spdlog::sinks::msvc_sink_mt>());
#endif
            g_Initialized = true;
        }
        return g_LoggingSink;
    }
    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> GetBacktraceSink() {
        std::scoped_lock lck(g_LoggingSinkMutex);
        if (!g_Initialized) {
            GetLoggingSink(); // Ensure the logging sink is initialized
        }
        return g_BacktraceSink;
    }
    spdlog::logger* GetLogger(const char* name) {
        auto logger = spdlog::get(name);
        if (!logger) {
            auto new_logger = std::make_shared<spdlog::logger>(
                name,
                GetLoggingSink()
            );
            spdlog::initialize_logger(new_logger);
            new_logger->set_level(spdlog::level::debug);
        }
        return spdlog::get(name).get();
    }
}
