#pragma once
#include <map>
#include <set>
#include <vector>
#include <queue>
#include <span>
#include <numeric>
#include <array>

#include <Allocator/Allocator.hpp>
namespace Foundation::Core {
    template<typename T, size_t Size>
    using StlArray = std::array<T, Size>;

    template<typename T>
    using StlVector = std::vector<T, StlAllocator<T>>;

    template<typename T, typename Predicate = std::less<T>>
    using StlSet = std::set<T, Predicate, StlAllocator<T>>;

    template<typename K, typename V, typename Predicate = std::less<K>>
    using StlMap = std::map<K, V, Predicate, StlAllocator<std::pair<const K, V>>>;

    template<typename T>
    using StlDeque = std::deque<T, StlAllocator<T>>;

    template<typename T, typename Container = StlDeque<T>>
    using StlQueue = std::queue<T, Container>;

    template<typename T, typename Predicate = std::less<T>, typename Container = StlVector<T>>
    using StlPriorityQueue = std::priority_queue<T, Container, Predicate>;

    class SpanReinterpretTag{};
    template<typename T>
    class StlSpan : public std::span<T> {
    public:
        using std::span<T>::span; 
        StlSpan() = default;
        /**
         * @brief Relaxed ctor for pointer-aliasing types
         */
        template<typename U> requires
            (sizeof(std::remove_reference<T>) == sizeof(std::remove_reference<U>) &&
            std::is_convertible_v<U*, T*>)
        StlSpan(U* data, size_t size) : std::span<T>(static_cast<T*>(data), size) {}

        /// For initializer lists, see
        /// https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2023/p2447r4.html
        /// i.e. StlSpan<const T>({ { 1, 2, 3 } })

        /**
         * @brief Relaxed ctor for C-style arrays
         */
        template<typename U, size_t Size>
        StlSpan(U(&array)[Size]) : StlSpan(array, Size) {}

        /**
         * @brief Relaxed ctor for contiguous STL containers
         */
        template<typename U>
        requires requires (U a) { a.data(); a.size(); }
        StlSpan(U& array) : StlSpan(array.data(), array.size())
        {}

        /**
         * @brief Shorthand for single l-value item
         */
        template<typename U> requires std::is_convertible_v<U*, T*>
        StlSpan(U& item) : StlSpan(&item, 1) {}

        /**
         * @brief Provides a byte-level view of the underlying data.
         */
        StlSpan<const char> AsBytes() const {
            return StlSpan<const char>{ reinterpret_cast<const char*>(this->data()), this->size_bytes() };
        }
    };
}
