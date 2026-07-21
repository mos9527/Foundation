#pragma once
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Foundation::Core
{
inline constexpr uint64_t kFNV1a64OffsetBasis = 14695981039346656037ull;
inline constexpr uint64_t kFNV1a64Prime = 1099511628211ull;

[[nodiscard]] constexpr uint64_t FNV1a64CombineByte(uint64_t hash, uint8_t value) noexcept
{
    return (hash ^ value) * kFNV1a64Prime;
}

[[nodiscard]] inline uint64_t FNV1a64CombineBytes(uint64_t hash, void const* data, size_t size) noexcept
{
    auto bytes = static_cast<uint8_t const*>(data);
    for (size_t i = 0; i < size; ++i)
        hash = FNV1a64CombineByte(hash, bytes[i]);
    return hash;
}

template<std::integral T>
[[nodiscard]] constexpr uint64_t FNV1a64Combine(uint64_t hash, T value) noexcept
{
    if constexpr (std::same_as<T, bool>)
        return FNV1a64CombineByte(hash, static_cast<uint8_t>(value));
    else
    {
        using U = std::make_unsigned_t<T>;
        U bits = static_cast<U>(value);
        for (size_t i = 0; i < sizeof(U); ++i)
            hash = FNV1a64CombineByte(hash, static_cast<uint8_t>(bits >> (i * 8u)));
        return hash;
    }
}

template<typename T>
    requires std::is_enum_v<T>
[[nodiscard]] constexpr uint64_t FNV1a64Combine(uint64_t hash, T value) noexcept
{
    return FNV1a64Combine(hash, static_cast<std::underlying_type_t<T>>(value));
}

[[nodiscard]] constexpr uint64_t FNV1a64Combine(uint64_t hash, float value) noexcept
{
    return FNV1a64Combine(hash, std::bit_cast<uint32_t>(value));
}

[[nodiscard]] constexpr uint64_t FNV1a64Combine(uint64_t hash, double value) noexcept
{
    return FNV1a64Combine(hash, std::bit_cast<uint64_t>(value));
}

template <typename... T>
[[nodiscard]] constexpr uint64_t FNV1a64(T... values) noexcept
{
    uint64_t hash = kFNV1a64OffsetBasis;
    ((hash = FNV1a64Combine(hash, values)), ...);
    return hash;
}

template <typename T>
[[nodiscard]] constexpr uint64_t FNV1a64(T* data, size_t size) noexcept
{
    return FNV1a64CombineBytes(kFNV1a64OffsetBasis, data, size);
}
} // namespace Foundation::Core
