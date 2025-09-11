#pragma once
#include <bit>
template<typename T, typename Ty> struct BitmaskEnumWrapper {    
    Ty value{};
    BitmaskEnumWrapper() : value(static_cast<Ty>(T{})) {}
    BitmaskEnumWrapper(T v) : value(static_cast<Ty>(v)) {}
    BitmaskEnumWrapper(Ty v) : value(v) {}
    BitmaskEnumWrapper operator=(auto v) { value = static_cast<Ty>(v); return *this; }
    BitmaskEnumWrapper operator|(auto v) const { return { value | static_cast<Ty>(v) }; }
    BitmaskEnumWrapper operator&(auto v) const { return { value & static_cast<Ty>(v) }; }
    BitmaskEnumWrapper operator^(auto v) const { return { value ^ static_cast<Ty>(v) }; }
    BitmaskEnumWrapper operator~() const { return { ~value }; }
    BitmaskEnumWrapper& operator|=(auto v) { value |= static_cast<Ty>(v); return *this; }
    BitmaskEnumWrapper& operator&=(auto v) { value &= static_cast<Ty>(v); return *this; }
    BitmaskEnumWrapper& operator^=(auto v) { value ^= static_cast<Ty>(v); return *this; }
    constexpr bool operator ==(auto v) const { return value == static_cast<Ty>(v); }
    constexpr operator Ty() const { return static_cast<Ty>(value); }
    constexpr operator T() const { return static_cast<T>(value); }
    constexpr operator bool() const { return value != 0; }
    constexpr bool is_pow2() const { return (value & (value - 1)) == 0; }
    constexpr bool is_bitmask() const { return is_pow2(); }
    constexpr int bit() const { return std::countr_one(value); }
};
// Defines a bitmask enum type {T}Bits with underlying integer type INT_T.
// Whilst defining a wrapper class of type T that provides bitwise operators.
#define BITMASK_ENUM_BEGIN(T,INT_T) \
enum class T##Bits : INT_T;	\
using T = BitmaskEnumWrapper<T##Bits, INT_T>; \
inline T##Bits   operator	&	(T##Bits x, T##Bits y)		{	return static_cast<T##Bits>(static_cast<INT_T>(x) & static_cast<INT_T>(y));	}; \
inline T##Bits   operator	|	(T##Bits x, T##Bits y)		{	return static_cast<T##Bits>(static_cast<INT_T>(x) | static_cast<INT_T>(y));	}; \
inline T##Bits   operator	^	(T##Bits x, T##Bits y)		{	return static_cast<T##Bits>(static_cast<INT_T>(x) ^ static_cast<INT_T>(y));	}; \
inline T##Bits   operator	~	(T##Bits x)			        {	return static_cast<T##Bits>(~static_cast<INT_T>(x));                    	}; \
inline T##Bits&	 operator	&=	(T##Bits& x, T##Bits y)		{	x = static_cast<T##Bits>(x & y);	return x; }; \
inline T##Bits&	 operator	|=	(T##Bits& x, T##Bits y)		{	x = static_cast<T##Bits>(x | y);	return x; }; \
inline T##Bits&	 operator	^=	(T##Bits& x, T##Bits y)		{	x = static_cast<T##Bits>(x ^ y);	return x; }; \
enum class T##Bits : INT_T {

#define BITMASK_ENUM_END() };

// Defines convince to_string() method and format_as() [fmt] for the respective enum class
#define ENUM_NAME_CONV_BEGIN(T) \
inline constexpr const char* to_string(T elem); \
inline auto format_as(T elem) { return to_string(elem); } \
inline constexpr const char* to_string(T elem) { \
    using enum T; \
    switch (elem) {

#define ENUM_NAME_CONV_END() } \
    return "Unknown"; \
}

#define ENUM_NAME(E) case E: return #E;
