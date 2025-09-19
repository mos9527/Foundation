#pragma once
#include <ranges>
#include <Core/Core.hpp>
namespace Foundation::Bits {
    /**
     * @brief `std::ranges` extensions and utilities.
     */
    namespace Ranges
    {
        using namespace std::ranges;
        /**
         * @brief Range predicate that checks if a value is contained within a given range.
         */
        template <typename Range> struct ContainedBy
        {
            Range const& range;
            ContainedBy(Range const& range) : range(range) {}
            constexpr bool operator()(auto&& value) const {
                return std::ranges::find(range, value) != std::ranges::end(range);
            }
        };
        /**
         * @brief Returns the first element of a range, or an empty Optional if the range is empty.
         */
        template<typename T> constexpr Core::Optional<range_value_t<T>> FirstOf(T&& range)
        {
            if (auto it = std::ranges::begin(range); it != std::ranges::end(range))
                return *it;
            return {};
        }
    } // namespace Ranges
    /**
     * @brief `std::views` extensions and utilities.
     */
    namespace Views
    {
        using namespace std::views;
    }
}
