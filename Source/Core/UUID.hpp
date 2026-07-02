#pragma once
#include <cstdint>
#include <functional>
#include <string_view>

namespace Foundation::Core {
    // http://www.jstatsoft.org/v08/i14/paper
    class XorShift128
    {
    public:
        XorShift128(uint64_t seed) noexcept { Reseed(seed); }
        XorShift128(uint64_t s0, uint64_t s1) noexcept { Reseed(s0, s1); }
        void Reseed(uint64_t seed) noexcept;
        // Seeds all 128 bits of state from two independent entropy words.
        void Reseed(uint64_t s0, uint64_t s1) noexcept;
        uint64_t Next() noexcept;

    private:
        uint64_t _x{ 0x9E3779B97F4A7C15ull };
        uint64_t _y{ 0xBF58476D1CE4E5B9ull };
        uint64_t _z{ 0x94D049BB133111EBull };
        uint64_t _w{ 0xFF51AFD7ED558CCDull };
    };

    /**
     * @brief 128-bit identifier. Two flavors, both well-formed per RFC 9562:
     *   - @ref Generate  : version-4 (random) UUID. Use for entity identities. Backed by
     *                      @ref XorShift128 (fast, not a CSPRNG) - fine for asset ids, not for secrets.
     *   - @ref FromBytes : version-8 (custom) UUID whose payload bits are a deterministic hash of the
     *                      input. Equal inputs fold to the same id (content addressing); NOT random.
     * The nil id (all-zero, @ref kNilUUID) is reserved for "unset" and is not a valid v4/v8 UUID.
     */
    struct FUUID
    {
        uint64_t hi{0};
        uint64_t lo{0};

        [[nodiscard]] bool IsNil() const noexcept { return hi == 0 && lo == 0; }

        friend bool operator==(FUUID, FUUID) noexcept = default;
        friend bool operator!=(FUUID, FUUID) noexcept = default;
        friend bool operator<(FUUID a, FUUID b) noexcept { return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo; }
        friend bool operator>(FUUID a, FUUID b) noexcept { return b < a; }
        friend bool operator<=(FUUID a, FUUID b) noexcept { return !(b < a); }
        friend bool operator>=(FUUID a, FUUID b) noexcept { return !(a < b); }

        [[nodiscard]] static FUUID Generate();
        [[nodiscard]] static FUUID FromBytes(const void* data, size_t size);
        [[nodiscard]] static FUUID FromString(std::string_view s) { return FromBytes(s.data(), s.size()); }

        // RFC 9562 canonical 8-4-4-4-12 lowercase hex; writes "nil" when @ref IsNil().
        [[nodiscard]] char const* Format(char* buf, size_t bufSize) const noexcept;
    };

    static constexpr FUUID kNilUUID{};

} // namespace Foundation::Core

template <>
struct std::hash<Foundation::Core::FUUID>
{
    size_t operator()(Foundation::Core::FUUID const& id) const noexcept
    {
        // 128 -> 64 mix (boost::hash_combine shape, folding lo into a hi-derived seed).
        size_t seed = std::hash<uint64_t>{}(id.hi);
        seed ^= std::hash<uint64_t>{}(id.lo) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
        return seed;
    }
};
