#pragma once
#include "Math.hpp"
#include <bit>
namespace Foundation::Math
{
    template <typename T> constexpr bool is_pow2(T v)
    {
        return (v & (v - 1)) == 0;
    }
}