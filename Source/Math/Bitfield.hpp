#pragma once
#include <cstdint>
#include <bitset>
namespace Foundation::Math
{
    template<typename T> constexpr T bitfieldExtract(T value, T offset, T length)
    {
        T mask = ((static_cast<T>(1u) << length) - 1) << offset;
        return (value & mask) >> offset;
    }
    template<typename T> constexpr T bitfieldInsert(T original, T value, T offset, T length)
    {
        T mask = ((static_cast<T>(1u) << length) - 1) << offset;
        return original & ~mask | value << offset & mask;
    }
}
