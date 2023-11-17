#pragma once
#include "../pch.hpp"
template<typename T> using DefaultAllocator = std::allocator<T>;
// ...here's hoping this won't be necessary

/* Alternative Allocator STL templates */
template<typename Key, typename Allocator> using unordered_set = std::unordered_set<Key, std::hash<Key>, std::equal_to<Key>, Allocator>;
template<typename Key, typename Value, typename Allocator> using unordered_map = std::unordered_map<Key, Value, std::hash<Key>, std::equal_to<Key>, Allocator>;
template<typename V, typename Allocator> using vector = std::vector<V, Allocator>;
