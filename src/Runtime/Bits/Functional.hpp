#pragma once
#include <variant>
namespace Foundation {
    /*! \cond */
    template <typename ...T> struct Visitor : T... {
        using T::operator()...;
        template<typename Arg> requires (!std::is_invocable_v<T, Arg&> && ...) auto operator()(Arg&) { /* nop */ };
    };
    /*! \endcond */

    /**
     * @brief Extended std::variant with C++23 visit() behavior and convenience Get()/GetIf() methods.
     */
    template<typename ...Args> struct Variant : public std::variant<Args...> {
        using std::variant<Args...>::variant;
        using std::variant<Args...>::operator=;

        // C++23 visit() behavior with default no-op visitor.        
        template<typename ...Visitors>
        auto visit(Visitors&&... visitors) {
            return std::visit(Visitor{ std::forward<Visitors>(visitors)... }, *this);
        }
        template<typename ...Visitors>
        auto visit(Visitors&&... visitors) const {
            return std::visit(Visitor{ std::forward<Visitors>(visitors)... }, *this);
        }

        // std::get<T>
        template<typename T>
        constexpr T& Get() {
            return std::get<T>(*this);
        }

        // std::get<T>
        template<typename T>
        constexpr const T& Get() const {
            return std::get<T>(*this);
        }

        // std::get_if<T>
        template<typename T>
        constexpr T* GetIf() {
            return std::get_if<T>(this);
        }

        // std::get_if<T>
        template<typename T>
        constexpr const T* GetIf() const {
            return std::get_if<T>(this);
        }
    };
}

