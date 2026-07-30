#pragma once
// XXX: fmt gets transitively included this way.
#define FMT_EXCEPTIONS 0
#include <fmt/base.h>
#include <fmt/format.h>
#include <cstdlib>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include <iterator>
#include "Allocator.hpp"
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
// To stdout
extern void Foundation_PrintImpl(const char* formatted, bool flush);

template<typename ...Args>
void Foundation_Log(const char* tag, LogLevel level, fmt::format_string<Args...> format, Args&&... args)
{
    fmt::basic_memory_buffer<char, fmt::inline_buffer_size, Foundation::Core::StlDefaultAllocator<char>> buffer;
    fmt::format_to(std::back_inserter(buffer), format, std::forward<Args>(args)...);
    buffer.push_back('\0');
    Foundation_LogImpl(level, tag, buffer.data());
}

namespace Foundation::Core
{
    template <typename... Args>
    void Print(fmt::format_string<Args...> format, Args&&... args)
    {
        fmt::basic_memory_buffer<char, fmt::inline_buffer_size, StlDefaultAllocator<char>> buffer;
        fmt::format_to(std::back_inserter(buffer), format, std::forward<Args>(args)...);
        buffer.push_back('\0');
        Foundation_PrintImpl(buffer.data(), false);
    }

    template <typename... Args>
    void Println(fmt::format_string<Args...> format, Args&&... args)
    {
        Print(format, std::forward<Args>(args)...);
        Foundation_PrintImpl("\n", true);
    }
} // namespace Foundation::Core

#define LOG(TAG, LEVEL, FORMAT, ...) Foundation_Log(#TAG, LEVEL, FORMAT __VA_OPT__(,) __VA_ARGS__);

#if defined(_MSC_VER)
#define FOUNDATION_TRAP() __debugbreak()
#elif defined(__clang__) && __has_builtin(__builtin_debugtrap)
#define FOUNDATION_TRAP() __builtin_debugtrap()
#else
#define FOUNDATION_TRAP() __builtin_trap()
#endif
#define FOUNDATION_STRINGIFY_2(x) #x
#define FOUNDATION_STRINGIFY(x) FOUNDATION_STRINGIFY_2(x)
#define FOUNDATION_SOURCE_LOC __FILE__ "@" FOUNDATION_STRINGIFY(__LINE__)
#define CHECK(expr) do { \
    if (!(expr)) [[unlikely]] { \
        LOG(Core, LogError, "Check failed: {}", FOUNDATION_SOURCE_LOC ":" #expr); \
        FOUNDATION_TRAP(); \
        std::terminate(); \
    } \
} while (false);

#define CHECK_MSG(expr, format_str, ...) do { \
    if (!(expr)) [[unlikely]] { \
        LOG(Core, LogError, "Check failed: {}", FOUNDATION_SOURCE_LOC ":" #expr); \
        LOG(Core, LogError, format_str __VA_OPT__(,) __VA_ARGS__); \
        FOUNDATION_TRAP(); \
        std::terminate(); \
    } \
} while (false);