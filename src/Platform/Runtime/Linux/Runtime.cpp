#include <stdio.h>
#include <spdlog/sinks/stdout_color_sinks.h>
namespace Foundation {
    namespace Platform {
        spdlog::sink_ptr GetPlatformDebugLoggingSink() {
            return std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        }

        void ExceptionHandler(const std::exception& e, std::vector<std::string> backtrace) {
            printf("*** RUNTIME EXCEPTION ***\n");
            printf("%s\n", e.what());
            printf("*** BACKTRACE ***\n");
            for (const auto& frame : backtrace)
                printf("%s\n", frame.c_str());
            exit(-1);
        }
    }
}
