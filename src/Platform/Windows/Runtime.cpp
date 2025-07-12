#include <spdlog/sinks/msvc_sink.h>
#include <crtdbg.h>
namespace Foundation {
    namespace Platform {
        spdlog::sink_ptr GetPlatformDebugLoggingSink() {
            return std::make_shared<spdlog::sinks::msvc_sink_mt>();
        }

        void ExceptionHandler(const std::exception& e, std::vector<std::string> backtrace) {
            OutputDebugStringA("*** RUNTIME EXCEPTION ***\n");
            OutputDebugStringA(e.what());
            OutputDebugStringA("\n*** BACKTRACE ***\n");
            for (const auto& frame : backtrace)
                OutputDebugStringA(frame.c_str());
            _RPT0(_CRT_ERROR, e.what());
        }
    }
}
