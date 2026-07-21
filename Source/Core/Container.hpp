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
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <ranges>
#include <algorithm>

#include "Allocator.hpp"
#include "Hash.hpp"
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
     * @brief Alias for `std::span`
     */
    template<typename T> using Span = std::span<T>;

    template<typename T> Span<const char> AsBytes(Span<T> data)
    {
        return { reinterpret_cast<const char*>(data.data()), data.size_bytes() };
    }
    /**
     * @brief Helper to construct one const r-value as a single element span.
     */
    template <typename T> Span<const T> AsSpan(T const& data) requires std::is_trivially_copyable_v<T>
    {
        return { &data, 1 };
    }
    /**
     * @brief Convenience function for constructing a Span with memory allocated from a @ref
     *        Foundation::Core::Allocator. Possibly constructs objects in-place if they are not trivially constructible (e.g.
     *        non-PODs)
     * @note Data is _not_ guaranteed to be zero-initialized. Pass in constructor args if needed.
     * @note Constructor args are only used if T is not trivially constructible, or if more than 0 args are passed in.
     */
    template <typename T, typename ...Args>
    Span<T> ConstructSpan(Allocator* resource, size_t size, Args&& ...args) {
        T* data = static_cast<T*>(resource->Allocate(size * sizeof(T), alignof(T)));
        if constexpr (!std::is_trivially_constructible_v<T> || sizeof...(Args) > 0)
        {
            for (size_t i = 0; i < size; i++)
                std::construct_at(&data[i], std::forward<Args>(args)...);
        }
        return Span<T>(data, size);
    }
    /**
     * @brief Convenience function for destructing a Span allocated with @ref ConstructSpan.
     *        Calls destructors in-place if the type is not trivially destructible (e.g. non-PODs)
     */
    template<typename T>
    void DestructSpan(Allocator* resource, Span<T> span) {
        if constexpr (!std::is_trivially_destructible_v<T>)
        {
            for (T& item : span)
                std::destroy_at(&item);
        }
        resource->Deallocate(span.data(), span.size() * sizeof(T));
    }
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
     * @brief `std::multiset` with explicit @ref Foundation::Core::StlAllocator constructor
     *
     * Construction without an allocator is disallowed, and will result in a compile-time error.
     *
     * @note Thread-safety is _not_ guaranteed as with other STL containers.
     */
    template<typename T, typename Predicate = std::less<T>>
    using MultiSet = std::multiset<T, Predicate, StlAllocator<T>>;

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
     * @brief `std::unordered_map` with explicit @ref Foundation::Core::StlAllocator constructor.
     *
     * @note Thread-safety is _not_ guaranteed as with other STL containers.
     */
    template<typename K, typename V, typename Hash = std::hash<K>, typename KeyEq = std::equal_to<K>>
    using HashMap = std::unordered_map<K, V, Hash, KeyEq, StlAllocator<Pair<const K, V>>>;

    /**
     * @brief `std::unordered_set` with explicit @ref Foundation::Core::StlAllocator constructor.
     *
     * @note Thread-safety is _not_ guaranteed as with other STL containers.
     */
    template<typename K, typename Hash = std::hash<K>, typename KeyEq = std::equal_to<K>>
    using HashSet = std::unordered_set<K, Hash, KeyEq, StlAllocator<K>>;

    /**
     * @brief `std::multimap` with explicit @ref Foundation::Core::StlAllocator constructor
     *
     * Construction without an allocator is disallowed, and will result in a compile-time error.
     *
     * @note Thread-safety is _not_ guaranteed as with other STL containers.
     */
    template<typename K, typename V, typename Predicate = std::less<K>>
    using MultiMap = std::multimap<K, V, Predicate, StlAllocator<Pair<const K, V>>>;
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

    /**
     * @brief STL Ranges extensions
     */
    namespace Ranges
    {
        using namespace std::ranges;
        /**
         * @brief Range predicate that checks if a value is contained within a given range.
         */
        template <typename Range>
        struct ContainedBy
        {
            Range const& range;
            ContainedBy(Range const& range) : range(range) {}
            constexpr bool operator()(auto&& value) const
            {
                return std::ranges::find(range, value) != std::ranges::end(range);
            }
        };
        /**
         * @brief Returns the first element of a range, or an empty Optional if the range is empty.
         */
        template <typename T>
        constexpr Optional<range_value_t<T>> FirstOf(T&& range)
        {
            if (auto it = std::ranges::begin(range); it != std::ranges::end(range))
                return *it;
            return {};
        }
    } // namespace Ranges
    /**
     * @brief STL Views extensions
     */
    namespace Views
    {
        using namespace std::views;
    } // namespace Views
    /**
    * @breif Hash helpers
    */
    template <typename T>
    [[nodiscard]] constexpr uint64_t FNV1a64(Span<const T> span) noexcept
    {
        return FNV1a64CombineBytes(kFNV1a64OffsetBasis, span.data(), span.size());
    }
    template <typename T>
    [[nodiscard]] constexpr uint64_t FNV1a64(Span<T> span) noexcept
    {
        return FNV1a64CombineBytes(kFNV1a64OffsetBasis, span.data(), span.size());
    }
}
