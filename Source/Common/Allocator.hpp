#pragma once
#include "../pch.hpp"
template<typename T> using DefaultAllocator = std::allocator<T>;
// ...here's hoping this won't be necessary

/* Alternative Allocator STL templates */
template<typename Key, typename Allocator> using unordered_set = std::unordered_set<Key, std::hash<Key>, std::equal_to<Key>, Allocator>;
template<typename Key, typename Value, typename Allocator> using unordered_map = std::unordered_map<Key, Value, std::hash<Key>, std::equal_to<Key>, Allocator>;
template<typename V, typename Allocator> using vector = std::vector<V, Allocator>;

/* Typing support */
// see https://en.cppreference.com/w/cpp/types/type_info/hash_code
using ref_type_info = std::reference_wrapper<const std::type_info>;
template<> struct std::hash<ref_type_info> {
	inline auto operator()(ref_type_info type) const { return type.get().hash_code(); }
};
template<> struct std::equal_to<ref_type_info> {
	inline auto operator()(ref_type_info lhs, ref_type_info rhs) const { return lhs.get() == rhs.get(); }
};
