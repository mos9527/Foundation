#pragma once
#include <bit>
#include <string>
#include <fmt/format.h>

namespace Foundation {
    constexpr const char* kSuffixes[]{ "B", "KB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB" };
    inline std::string formatHumanReadableSize(const uint64_t size)
    {
        const int bits = 63 - std::countl_zero(size);
        const int index = std::min(bits / 10, static_cast<int>(std::size(kSuffixes) - 1));
        double value = static_cast<double>(size) / (1LL << (index * 10));
        return fmt::format("{:.2f} {}", value, kSuffixes[index]);
    }
}
