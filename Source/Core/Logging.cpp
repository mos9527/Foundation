using namespace Foundation;
using namespace Core;

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__ANDROID__)
#include <android/log.h>
#endif

Mutex gLogImplMutex;
void Foundation_LogImpl(LogLevel level, const char* tag, const char* formatted)
{
    static auto init = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now() - init;
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    auto str = fmt::format("{}|+{:>5.5f}s|{} {}", level, ns / 1e9, tag, formatted);
    std::unique_lock lock(gLogImplMutex);
#if defined(__ANDROID__)
    // Android's stderr goes to /dev/null; route everything through logcat.
    constexpr int kAndroidPriority[] = {
        ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
    int priority = (level >= 0 && level < 4) ? kAndroidPriority[level] : ANDROID_LOG_DEFAULT;
    __android_log_print(priority, tag, "%s", str.c_str());
#else
    fprintf(stderr, "%s\n", str.c_str());
#endif
#if defined(_WIN32)
    if (IsDebuggerPresent())
    {
        auto windbg = fmt::format("{}|+{:>5.5f}s|{} {}\n", format_as<false>(level), ns / 1e9, tag, formatted);
        OutputDebugStringA(windbg.c_str());
    }
#endif
}
