#pragma once
#include <fmt/base.h>
#include <fmt/format.h>
#include <stdexcept>
enum LogLevel
{
    LogDebug,
    LogInfo,
    LogWarn,
    LogError
};
// NOLINTBEGIN
template<bool Ansi = true>
constexpr const char* format_as(LogLevel level)
{
    if constexpr (Ansi)
    {
        switch (level)
        {
        case LogDebug: return "\033[34mD";
        case LogInfo:  return "\033[37mI";
        case LogWarn:  return "\033[33mW";
        case LogError: return "\033[31mE";
        }
    }
    else
    {
        switch (level)
        {
        case LogDebug: return "D";
        case LogInfo:  return "I";
        case LogWarn:  return "W";
        case LogError: return "E";
        }
    }
    return "?";
}

// NOLINTEND


extern void Foundation_LogImpl(LogLevel level, const char* tag, const char* formatted);
template<typename ...Args>
void Foundation_Log(const char* tag, LogLevel level, fmt::format_string<Args...> format, Args&&... args)
{
    constexpr size_t kN = sizeof...(Args);
    if constexpr (kN > 0)
    {
        Foundation_LogImpl(level, tag, fmt::format(format, std::forward<Args>(args)...).c_str());
    } else
    {
        Foundation_LogImpl(level, tag, format.str.data());
    }
}

#define LOG(TAG, LEVEL, FORMAT, ...) Foundation_Log(#TAG, LEVEL, FORMAT __VA_OPT__(,) __VA_ARGS__);

#define CHECK(expr) if(!(expr)) { \
    constexpr const char* msg = "Check failed: " #expr; \
    LOG(Core, LogError, "{}", msg); \
    throw std::runtime_error(msg); \
}

#define CHECK_MSG(expr, format_str, ...) if(!(expr)) { \
    auto msg = fmt::format(format_str __VA_OPT__(,) __VA_ARGS__); \
    LOG(Core, LogError, "{}", msg); \
    throw std::runtime_error(msg); \
}