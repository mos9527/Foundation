#pragma once
#include <bit>
#include <Core/Container/Common.hpp>
#include <fmt/format.h>

namespace Foundation {
    using namespace Foundation::Core;
    constexpr const char* kSuffixes[]{ "B", "KB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB" };
    size_t operator"" _B(unsigned long long k) { return k;}
    size_t operator"" _KB(unsigned long long k) { return k * (1 << 10LL);}
    size_t operator"" _MB(unsigned long long k) { return k * (1 << 20LL);}
    size_t operator"" _GB(unsigned long long k) { return k * (1 << 30LL);}
    inline String formatHumanReadableSize(const uint64_t size)
    {
        const int bits = 63 - std::countl_zero(size);
        const int index = std::min(bits / 10, static_cast<int>(std::size(kSuffixes) - 1));
        double value = static_cast<double>(size) / (1LL << (index * 10));
        return fmt::format("{:.2f} {}", value, kSuffixes[index]);
    }
}
