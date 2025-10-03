#pragma once
#include <bit>
/**
 * @brief Header-only convenience utilities
 */
namespace Foundation::Bits
{
    /**
     * @brief Wrapper for bitmask enum types that provides bitwise operators.
     * @tparam T Enum type
     * @tparam Ty Underlying type
     */
    template <typename T, typename Ty> struct BitmaskEnumWrapper {
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
        [[nodiscard]] constexpr bool is_pow2() const { return (value & (value - 1)) == 0; }
        [[nodiscard]] constexpr bool is_bitmask() const { return is_pow2(); }
        [[nodiscard]] constexpr int bit() const { 
            constexpr size_t bits = sizeof(Ty) * 8;
            return std::countr_zero(value) & (bits - 1); 
        }
    };
} // namespace Foundation::Bits
/**
 * @brief Defines a bitmask enum type {T}Bits with underlying integer type INT_T whilst defining a wrapper class of type
 * T that provides bitwise operators.
 * @param T Enum name
 * @param INT_T Underlying integer type (e.g. uint32_t, uint64_t)
 * @note The actual enum values must be defined between BITMASK_ENUM_BEGIN and BITMASK_ENUM_END.
 * @note The enum values should be powers of two, or combinations thereof.
 * @note This also defines the to_integer() function for converting the enum to its underlying integer type.
 * @note This also defines the bitwise operators for the enum type.
 */
#define BITMASK_ENUM_BEGIN(T,INT_T) \
enum class T##Bits : INT_T;	\
inline constexpr INT_T to_integer(T##Bits e) { return static_cast<INT_T>(e); } \
using T = Foundation::Bits::BitmaskEnumWrapper<T##Bits, INT_T>; \
inline T##Bits   operator	&	(T##Bits x, T##Bits y)		{	return static_cast<T##Bits>(static_cast<INT_T>(x) & static_cast<INT_T>(y));	}; \
inline T##Bits   operator	|	(T##Bits x, T##Bits y)		{	return static_cast<T##Bits>(static_cast<INT_T>(x) | static_cast<INT_T>(y));	}; \
inline T##Bits   operator	^	(T##Bits x, T##Bits y)		{	return static_cast<T##Bits>(static_cast<INT_T>(x) ^ static_cast<INT_T>(y));	}; \
inline T##Bits   operator	~	(T##Bits x)			        {	return static_cast<T##Bits>(~static_cast<INT_T>(x));                    	}; \
inline T##Bits&	 operator	&=	(T##Bits& x, T##Bits y)		{	x = static_cast<T##Bits>(x & y);	return x; }; \
inline T##Bits&	 operator	|=	(T##Bits& x, T##Bits y)		{	x = static_cast<T##Bits>(x | y);	return x; }; \
inline T##Bits&	 operator	^=	(T##Bits& x, T##Bits y)		{	x = static_cast<T##Bits>(x ^ y);	return x; }; \
enum class T##Bits : INT_T {

#define BITMASK_ENUM_END() };

/**
 * @brief Defines convince to_string() method and format_as() [fmt] for the respective enum class
 * Example usage:
 * @code{.cpp}
 *  ENUM_NAME_CONV_BEGIN(Color)
 *      ENUM_NAME(Red)
 *      ENUM_NAME(Green)
 *      ENUM_NAME(Blue)
 *  ENUM_NAME_CONV_END()
 * @endcode
 * The enum can be subsequently used as:
 * @code{.cpp}
 *  Color c = Color::Red;
 *  std::cout << to_string(c); // prints "Red"
 *  fmt::print("Color is {}\n", c); // prints "Color is Red"
 * @endcode
 * @param T Enum type
 * @note The actual enum values must be defined between ENUM_NAME_CONV_BEGIN and ENUM_NAME_CONV_END.
 * @note If the enum value is not recognized (not defined), "Unknown" is returned.
 */
#define ENUM_NAME_CONV_BEGIN(T) \
inline constexpr const char* format_as(T elem) { \
    using enum T; \
    switch (elem) {

#define ENUM_NAME_CONV_END() } \
    return "Unknown"; \
}
// Shorthand for @ref ENUM_NAME_CONV_BEGIN case statements
#define ENUM_NAME(E) case E: return #E;
