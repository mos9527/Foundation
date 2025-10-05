#pragma once
#include <array>
#include <bitset>
#include <list>
#include <map>
#include <numeric>
#include <optional>
#include <queue>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Allocator.hpp"
namespace Foundation::Core {

    /* -- STL Value types -- */

    /**
     * @brief Alias for `std::optional`
     */
    template<typename T>
    using Optional = std::optional<T>;

    /**
     * @brief Alias for `std::pair`
     */
    template <typename First, typename Second>
    using Pair = std::pair<First, Second>;

    /**
     * @brief Alias for `std::tuple`
     */
    template<typename ...Args>
    using Tuple = std::tuple<Args...>;

    /**
     * @brief Alias for `std::array`
     */
    template<typename T, size_t Size>
    using Array = std::array<T, Size>;

    /**
     * @brief Alias for `std::bitset`
     */
    template<size_t Size>
    using Bitset = std::bitset<Size>;

    /**
    * @brief Alias for `std::basic_string_view<char>`
    */
    using StringView = std::basic_string_view<char>;

    /**
     * @brief std::span with relaxed constructors for pointer-aliasing types and common containers.
     */
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
         * @brief Provides casting to a byte-level view of the underlying data.
         */
        [[nodiscard]] Span<const char> AsBytes() const {
            return Span<const char>{ reinterpret_cast<const char*>(this->data()), this->size_bytes()  };
        }
    };

    /* -- STL Containers -- */

    /**
     * @brief Alias for `std::basic_string<char>`, without an explicit allocator constructor
     *
     * Allocation of strings on heap is done with the default global allocator.
     *
     * @note Thread-safety is _not_ guaranteed as with other STL containers.
     */
    using String = std::basic_string<char>;
    /**
     * @brief `std::basic_string<char>` with explicit @ref Foundation::Core::StlAllocator constructor
     *
     * Construction without an allocator is disallowed, and will result in a compile-time error.
     *
     * @note Thread-safety is _not_ guaranteed as with other STL containers.
     */
    using StringAlloc = std::basic_string<char, std::char_traits<char>, StlAllocator<char>>;

    /**
     * @brief `std::vector` with explicit @ref Foundation::Core::StlAllocator constructor
     *
     * Construction without an allocator is disallowed, and will result in a compile-time error.
     *
     * @note Thread-safety is _not_ guaranteed as with other STL containers.
     */
    template<typename T>
    using Vector = std::vector<T, StlAllocator<T>>;

    /**
     * @brief `std::set` with explicit @ref Foundation::Core::StlAllocator constructor
     *
     * Construction without an allocator is disallowed, and will result in a compile-time error.
     *
     * @note Thread-safety is _not_ guaranteed as with other STL containers.
     */
    template<typename T, typename Predicate = std::less<T>>
    using Set = std::set<T, Predicate, StlAllocator<T>>;
    /**
     * @brief `std::map` with explicit @ref Foundation::Core::StlAllocator constructor
     *
     * Construction without an allocator is disallowed, and will result in a compile-time error.
     *
     * @note Thread-safety is _not_ guaranteed as with other STL containers.
     */
    template<typename K, typename V, typename Predicate = std::less<K>>
    using Map = std::map<K, V, Predicate, StlAllocator<Pair<const K, V>>>;
    /**
     * @brief `std::deque` with explicit @ref Foundation::Core::StlAllocator constructor
     *
     * Construction without an allocator is disallowed, and will result in a compile-time error.
     *
     * @note Thread-safety is _not_ guaranteed as with other STL containers.
     */
    template<typename T>
    using Deque = std::deque<T, StlAllocator<T>>;
    /**
     * @brief `std::list` with explicit @ref Foundation::Core::StlAllocator constructor
     *
     * Construction without an allocator is disallowed, and will result in a compile-time error.
     *
     * @note Thread-safety is _not_ guaranteed as with other STL containers.
     */
    template<typename T>
    using List = std::list<T, StlAllocator<T>>;
    /**
     * @brief `std::queue` with explicit @ref Foundation::Core::StlAllocator constructor
     *
     * Construction without an allocator is disallowed, and will result in a compile-time error.
     *
     * @note Thread-safety is _not_ guaranteed as with other STL containers.
     */
    template<typename T, typename Container = Deque<T>>
    using Queue = std::queue<T, Container>;
    /**
     * @brief `std::priority_queue` with explicit @ref Foundation::Core::StlAllocator constructor
     *
     * Construction without an allocator is disallowed, and will result in a compile-time error.
     *
     * @note Thread-safety is _not_ guaranteed as with other STL containers.
     */
    template<typename T, typename Predicate = std::less<T>, typename Container = Vector<T>>
    using PriorityQueue = std::priority_queue<T, Container, Predicate>;
}
