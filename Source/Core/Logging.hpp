#pragma once
#include <fmt/format.h>

#define LOG_RUNTIME(TAG, LEVEL, ...) \
    printf("[" #LEVEL "]" " " #TAG " %s", fmt::format(__VA_ARGS__));

#define CHECK(expr) if(!(expr)) { \
LOG_RUNTIME(Core, err, "Check failed: {}", #expr); \
throw std::runtime_error( #expr ); \
}

#define CHECK_MSG(expr, format_str, ...) if(!(expr)) { \
std::string __message = fmt::format(format_str __VA_OPT__(,) __VA_ARGS__); \
LOG_RUNTIME(Core, err, __message); \
throw std::runtime_error(__message); \
}