#include "UUID.hpp"
#include <cstdio>
#include <random>

namespace Foundation::Core {

// Add UUIDv4 bits
FUUID UUIDv4(FUUID id, uint64_t version) noexcept
{
    id.hi = (id.hi & 0xFFFFFFFFFFFF0FFFull) | (version << 12);
    id.lo = (id.lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;
    return id;
}

FUUID FUUID::FromBytes(const void* data, size_t size)
{
    constexpr uint64_t kLoBasis = 0xcbf29ce484222325ull; // FNV-1a 64-bit offset basis
    constexpr uint64_t kLoPrime = 0x00000100000001b3ull; // FNV-1a 64-bit prime
    constexpr uint64_t kHiBasis = 0xff51afd7ed558ccdull; // splitmix64 constant
    constexpr uint64_t kHiPrime = 0x9e3779b97f4a7c15ull; // golden-ratio odd multiplier

    uint64_t lo = kLoBasis;
    uint64_t hi = kHiBasis;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i)
    {
        lo = (lo ^ bytes[i]) * kLoPrime;
        hi = (hi ^ bytes[i]) * kHiPrime;
    }
    return UUIDv4(FUUID{ .hi = hi, .lo = lo }, 8);
}

uint64_t SplitMix64(uint64_t state) noexcept
{
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void XorShift128::Reseed(uint64_t seed) noexcept
{
    if (seed == 0) seed = 0x9E3779B97F4A7C15ull;
    Reseed(seed, ~seed);
}

void XorShift128::Reseed(uint64_t s0, uint64_t s1) noexcept
{
    _x = SplitMix64(s0);
    _y = SplitMix64(s0 ^ 0x9E3779B97F4A7C15ull);
    _z = SplitMix64(s1);
    _w = SplitMix64(s1 ^ 0xBF58476D1CE4E5B9ull);
    if ((_x | _y | _z | _w) == 0) _w = 0x9E3779B97F4A7C15ull;
}

uint64_t XorShift128::Next() noexcept
{
    uint64_t t = _x ^ (_x << 11);
    _x = _y;
    _y = _z;
    _z = _w;
    return _w = (_w ^ (_w >> 19)) ^ (t ^ (t >> 8));
}

namespace {
    void MakeSeed(uint64_t& s0, uint64_t& s1) noexcept
    {
        std::random_device rd;
        auto draw = [&rd] { return (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd()); };
        s0 = draw();
        s1 = draw();
    }
    XorShift128& Rng()
    {
        thread_local XorShift128 engine = []
        {
            uint64_t s0, s1;
            MakeSeed(s0, s1);
            return XorShift128{ s0, s1 };
        }();
        return engine;
    }
} // namespace

FUUID FUUID::Generate()
{
    auto& engine = Rng();
    return UUIDv4(FUUID{ .hi = engine.Next(), .lo = engine.Next() }, 4);
}

char const* FUUID::Format(char* buf, size_t bufSize) const noexcept
{
    if (bufSize == 0)
        return buf;
    if (IsNil())
    {
        std::snprintf(buf, bufSize, "nil");
        return buf;
    }
    auto byte = [](uint64_t word, int shift) -> unsigned
    {
        return static_cast<unsigned>((word >> shift) & 0xffu);
    };
    std::snprintf(buf, bufSize,
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  byte(hi, 56), byte(hi, 48), byte(hi, 40), byte(hi, 32),
                  byte(hi, 24), byte(hi, 16), byte(hi, 8), byte(hi, 0),
                  byte(lo, 56), byte(lo, 48), byte(lo, 40), byte(lo, 32),
                  byte(lo, 24), byte(lo, 16), byte(lo, 8), byte(lo, 0));
    return buf;
}

} // namespace Foundation::Core
