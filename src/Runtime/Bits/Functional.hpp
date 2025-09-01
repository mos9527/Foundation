#pragma once
#include <variant>
#include <concepts>
namespace Foundation {
    template<typename Arg, typename... T> concept _no_visitor = (!std::is_invocable<T, Arg&>::value && ...);
    template<typename ...T> struct _visitor_overloads : T... {
        using T::operator()...;
        template<typename Arg> requires _no_visitor<Arg, T...> auto operator()(Arg&) { /* nop */ };
    };
    template<typename ...Args> struct Variant : public std::variant<Args...> {
        using std::variant<Args...>::variant;
        using std::variant<Args...>::operator=;

        // C++23 visit() behavior with default no-op visitor.        
        template<typename ...Visitor>
        auto visit(Visitor&&... visitors) {
            return std::visit(_visitor_overloads{ std::forward<Visitor>(visitors)... }, *this);
        }
        template<typename ...Visitor>
        auto visit(Visitor&&... visitors) const {
            return std::visit(_visitor_overloads{ std::forward<Visitor>(visitors)... }, *this);
        }

        // std::get<T>
        template<typename T>
        T& Get() {
            return std::get<T>(*this);
        }

        // std::get_if<T>
        template<typename T>
        T* GetIf() {
            return std::get_if<T>(this);
        }
    };
}

