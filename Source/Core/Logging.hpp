#pragma once
#include <fmt/base.h>
#include <fmt/format.h>
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
    LOG(Core, LogError, "Check failed: " #expr); \
    throw std::runtime_error( #expr ); \
}

#define CHECK_MSG(expr, format_str, ...) if(!(expr)) { \
    LOG(Core, LogError, format_str __VA_OPT__(,) __VA_ARGS__); \
    throw std::runtime_error( #expr ); \
}