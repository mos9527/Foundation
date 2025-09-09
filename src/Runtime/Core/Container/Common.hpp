#pragma once
#include <map>
#include <set>
#include <vector>
#include <queue>
#include <span>
#include <numeric>
#include <array>
#include <optional>

#include <Allocator/Allocator.hpp>
namespace Foundation::Core {
    template<typename T>
    using Optional = std::optional<T>;

    template <typename First, typename Second>
    using Pair = std::pair<First, Second>;

    template<typename ...Args>
    using Tuple = std::tuple<Args...>;

    using String = std::basic_string<char, std::char_traits<char>, StlAllocator<char>>;

    template<typename T, size_t Size>
    using Array = std::array<T, Size>;

    template<typename T>
    using Vector = std::vector<T, StlAllocator<T>>;

    template<typename T, typename Predicate = std::less<T>>
    using Set = std::set<T, Predicate, StlAllocator<T>>;

    template<typename K, typename V, typename Predicate = std::less<K>>
    using Map = std::map<K, V, Predicate, StlAllocator<Pair<const K, V>>>;

    template<typename T>
    using Deque = std::deque<T, StlAllocator<T>>;

    template<typename T, typename Container = Deque<T>>
    using Queue = std::queue<T, Container>;

    template<typename T, typename Predicate = std::less<T>, typename Container = Vector<T>>
    using PriorityQueue = std::priority_queue<T, Container, Predicate>;

    template<typename T>
    class Span : public std::span<T> {
    public:
        using std::span<T>::span; 
        Span() = default;
        /**
         * @brief Relaxed ctor for pointer-aliasing types
         */
        template<typename U> requires
            (sizeof(std::remove_reference<T>) == sizeof(std::remove_reference<U>) &&
            std::is_convertible_v<U*, T*>)
        Span(U* data, size_t size) : std::span<T>(static_cast<T*>(data), size) {}

        /// For initializer lists, see
        /// https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2023/p2447r4.html
        /// i.e. StlSpan<const T>({ { 1, 2, 3 } })

        /**
         * @brief Relaxed ctor for C-style arrays
         */
        template<typename U, size_t Size>
        Span(U(&array)[Size]) : Span(array, Size) {}

        /**
         * @brief Relaxed ctor for contiguous STL containers
         */
        template<typename U>
        requires requires (U a) { a.data(); a.size(); }
        Span(U& array) : Span(array.data(), array.size())
        {}

        /**
         * @brief Shorthand for single l-value item
         */
        template<typename U> requires std::is_convertible_v<U*, T*>
        Span(U& item) : Span(&item, 1) {}

        /**
         * @brief Provides a byte-level view of the underlying data.
         */
        Span<const char> AsBytes() const {
            return Span<const char>{ reinterpret_cast<const char*>(this->data()), this->size_bytes() };
        }
    };
}
