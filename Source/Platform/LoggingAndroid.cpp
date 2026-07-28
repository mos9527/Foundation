#include "Logging.hpp"

#include <Core/Container.hpp>
#include <android/log.h>
namespace Foundation::Platform
{
    using namespace Foundation::Core;
    void WriteLog(LogLevel level, const char* tag, double seconds, const char* formatted)
    {
        // Android's stderr goes to /dev/null; route everything through logcat.
        auto str = Format("{}|+{:>5.5f}s|{} {}", level, seconds, tag, formatted);
        constexpr int kAndroidPriority[] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
        int priority = (level >= 0 && level < 4) ? kAndroidPriority[level] : ANDROID_LOG_DEFAULT;
        __android_log_print(priority, tag, "%s", str.c_str());
    }
    void Print(const char* formatted, bool flush)
    {
        fprintf(stdout, "%s", formatted);
        if (flush)
            fflush(stdout);
    }
} // namespace Foundation::Platform
