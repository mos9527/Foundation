#pragma once
#include <cstdio>
#include <string_view>
#include <fmt/format.h>

#define LOG_STREAM stderr
enum LogLevel
{
    LogDebug,
    LogInfo,
    LogWarn,
    LogError
};
// NOLINTBEGIN
constexpr const char* format_as(LogLevel level)
{
    switch (level)
    {
    case LogDebug: return "DEBUG";
    case LogInfo: return "INFO";
    case LogWarn: return "WARN";
    case LogError: return "ERROR";
    }
}
// NOLINTEND
template<typename ...Args>
void foundationLogRuntime(const char* tag, LogLevel level, std::string_view format, Args&&... args)
{
    constexpr size_t kN = sizeof...(Args);
    if constexpr (kN > 0)
    {
        auto kStr = fmt::format(fmt::runtime(format), std::forward<Args>(args)...);
        fprintf(LOG_STREAM, fmt::format("[{}@{}] {}\n", level, tag, kStr).c_str());
    } else
    {
        fprintf(LOG_STREAM, fmt::format("[{}@{}] {}\n", level, tag, format).c_str());
    }
}

#define LOG_RUNTIME(TAG, LEVEL, FORMAT, ...) \
    foundationLogRuntime(#TAG, LEVEL, FORMAT __VA_OPT__(,) __VA_ARGS__);

#define CHECK(expr) if(!(expr)) { \
    LOG_RUNTIME(Core, LogError, "Check failed: " #expr); \
    throw std::runtime_error( #expr ); \
}

#define CHECK_MSG(expr, format_str, ...) if(!(expr)) { \
    LOG_RUNTIME(Core, LogError, format_str __VA_OPT__(,) __VA_ARGS__); \
    throw std::runtime_error( #expr ); \
}