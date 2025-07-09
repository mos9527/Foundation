#include <Core/Logging.hpp>

namespace Foundation {
    namespace Core {
        namespace Logging {
            std::shared_ptr<spdlog::sinks::dist_sink_mt> GetLoggingSink() {
                static std::shared_ptr<spdlog::sinks::dist_sink_mt> g_LoggingSink = nullptr;
                // Lazily initialize the global logging sink if it does not exist.
                if (!g_LoggingSink) {
                    g_LoggingSink = std::make_shared<spdlog::sinks::dist_sink_mt>();
                }
                return g_LoggingSink;
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
