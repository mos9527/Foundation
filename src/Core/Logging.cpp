#include <Core/Logging.hpp>
#include <mutex>

namespace Foundation {
    namespace Core {
        namespace Logging {
            static bool g_Initialized = false;
            static std::shared_ptr<spdlog::sinks::dist_sink_mt> g_LoggingSink = nullptr;
            static std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> g_BacktraceSink = nullptr;
            size_t kBacktraceLogMessages = 1000; // Number of messages to keep in the ring buffer for backtrace logging

            std::recursive_mutex g_LoggingSinkMutex;
            std::shared_ptr<spdlog::sinks::dist_sink_mt> GetLoggingSink() {
                std::scoped_lock lck(g_LoggingSinkMutex);
                // Lazily initialize the global logging sink if it does not exist.
                if (!g_Initialized) {
                    g_LoggingSink = std::make_shared<spdlog::sinks::dist_sink_mt>();
                    g_BacktraceSink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(kBacktraceLogMessages);
                    // Log to platform sink and ringbuffer sink for backtrace logging
                    g_LoggingSink->add_sink(g_BacktraceSink);  
                    g_LoggingSink->add_sink(Platform::GetPlatformDebugLoggingSink());
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
                }
                return spdlog::get(name).get();
            }
            void DestroyLogger(const char* name) {
                auto logger = spdlog::get(name);
                if (logger) {
                    spdlog::details::registry::instance().drop(name);
                }
            }
        }
    }
}
