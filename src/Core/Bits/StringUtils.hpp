#pragma once
#include <bit>
#include <string>
#include <fmt/format.h>

namespace Foundation {
    namespace Core {
        namespace Bits {
            constexpr const char* kSuffixes[]{ "B", "KB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB" };
            inline const std::string ByteSizeToString(uint64_t size) {
                int bits = 63 - std::countl_zero(size);
                int index = std::min(bits / 10, static_cast<int>(sizeof(kSuffixes) / sizeof(kSuffixes[0]) - 1));
                double value = (double)(size) / (1LL << (index * 10));
                return fmt::format("{:.2f} {}", value, kSuffixes[index]);
            }
        }
    }
}
