#pragma once
#include <map>
#include <set>
#include <vector>
#include <queue>
#include <span>
#include <Core/Allocator/Allocator.hpp>
namespace Foundation
{
    namespace Core {
        template<typename T>
        using StlVector = std::vector<T, StlAllocator<T>>;

        template<typename T, typename Predicate = std::less<T>>
        using StlSet = std::set<T, Predicate, StlAllocator<T>>;

        template<typename K, typename V, typename Predicate = std::less<K>>
        using StlMap = std::map<K, V, Predicate, StlAllocator<std::pair<const K, V>>>;

        // NO. Screw unordered_map<> et al.
        // They are the LEAST ABI stable containers in the STL and that's saying something.
        // If you need a hash map, think again until you don't need to.

        template<typename T, typename Container = StlVector<T>>
        using StlQueue = std::queue<T, Container>;

        template<typename T, typename Predicate = std::less<T>, typename Container = StlVector<T>>
        using StlPriorityQueue = std::priority_queue<T, Container, Predicate>;

        template<typename T>
        class StlSpan : public std::span<T> {
        public:
            using std::span<T>::span; 
            StlSpan() = default;
            /// <summary>
            /// Relaxed ctor for pointer-aliasing types
            /// </summary>
            template<typename U> requires
                (sizeof(std::remove_reference<T>) == sizeof(std::remove_reference<U>) &&
                std::is_convertible_v<U*, T*>)
            StlSpan(U* data, size_t size) : std::span<T>(static_cast<T*>(data), size) {}

            /// For initializer lists, see
            /// https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2023/p2447r4.html
            /// For now you'll be making sandwhiches instead.
            /// ( i.e. StlSpan<const T>({ { 1, 2, 3 } }), StlSpan<T* const>({ { look_ma_a_single_pointer } })
            
            /// <summary>
            /// Shorthand for single l-value item
            /// </summary>           
            template<typename U> requires std::is_convertible_v<U*, T*>
            StlSpan(U& item) : StlSpan(&item, 1) {}

            /// <summary>
            /// Relaxed ctor for C-style arrays
            /// </summary>
            template<typename U, size_t Size>
            StlSpan(U(&array)[Size]) : StlSpan(array, Size) {}

            /// <summary>
            /// Relaxed ctor for contiguous STL containers
            /// </summary>            
            template<typename U>
            StlSpan(U& array) : StlSpan(array.data(), array.size())
            {}
        };
    }
}
