#pragma once
template<typename T, typename Ty> struct BitmaskEnumWrapper {    
    Ty value{};
    BitmaskEnumWrapper() : value(static_cast<Ty>(T{})) {}
    BitmaskEnumWrapper(auto v) : value(static_cast<Ty>(v)) {}
    BitmaskEnumWrapper<T, Ty> operator=(auto v) { value = static_cast<Ty>(v); return *this; }
    BitmaskEnumWrapper<T, Ty> operator|(auto v) const { return { value | static_cast<Ty>(v) }; }
    BitmaskEnumWrapper<T, Ty> operator&(auto v) const { return { value & static_cast<Ty>(v) }; }
    BitmaskEnumWrapper<T, Ty> operator^(auto v) const { return { value ^ static_cast<Ty>(v) }; }
    BitmaskEnumWrapper<T, Ty> operator~() const { return { ~value }; }
    BitmaskEnumWrapper<T, Ty>& operator|=(auto v) { value |= static_cast<Ty>(v); return *this; }
    BitmaskEnumWrapper<T, Ty>& operator&=(auto v) { value &= static_cast<Ty>(v); return *this; }
    BitmaskEnumWrapper<T, Ty>& operator^=(auto v) { value ^= static_cast<Ty>(v); return *this; }    
    inline constexpr bool operator ==(auto v) const { return value == static_cast<Ty>(v); }
    inline constexpr operator Ty() const { return static_cast<Ty>(value); }
    inline constexpr operator T() const { return static_cast<T>(value); }
    inline constexpr operator bool() const { return value != 0; }
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

