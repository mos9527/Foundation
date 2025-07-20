#pragma once
#define BITMASK_ENUM_BEGIN(T,INT_T) \
enum class T : INT_T;	\
inline INT_T operator	&	(T x, T y)		{	return static_cast<INT_T>(x) & static_cast<INT_T>(y);	}; \
inline INT_T operator	|	(T x, T y)		{	return static_cast<INT_T>(x) | static_cast<INT_T>(y);	}; \
inline INT_T operator	^	(T x, T y)		{	return static_cast<INT_T>(x) ^ static_cast<INT_T>(y);	}; \
inline INT_T operator	~	(T x)			{	return ~static_cast<INT_T>(x);							}; \
inline T&	 operator	&=	(T& x, T y)		{	x = static_cast<T>(x & y);	return x; }; \
inline T&	 operator	|=	(T& x, T y)		{	x = static_cast<T>(x | y);	return x; }; \
inline T&	 operator	^=	(T& x, T y)		{	x = static_cast<T>(x ^ y);	return x; }; \
enum class T : INT_T {
#define BITMASK_ENUM_END() };
