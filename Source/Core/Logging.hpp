#pragma once
#include <cstdio>
#include <string_view>
#include <fmt/format.h>

#define LOG_STREAM stderr

// TODO: level should be enums
template<typename ...Args>
void foundationLogRuntime(const char* tag, const char* level, std::string_view format, Args&&... args)
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
    foundationLogRuntime(#TAG, #LEVEL, FORMAT __VA_OPT__(,) __VA_ARGS__);

#define CHECK(expr) if(!(expr)) { \
    LOG_RUNTIME(Core, err, "Check failed: " #expr); \
    throw std::runtime_error( #expr ); \
}

#define CHECK_MSG(expr, format_str, ...) if(!(expr)) { \
    LOG_RUNTIME(Core, err, format_str __VA_OPT__(,) __VA_ARGS__); \
    throw std::runtime_error( #expr ); \
}