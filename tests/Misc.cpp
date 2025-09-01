#include <Bits/Functional.hpp>
#include <iostream>
using namespace Foundation;

int main() {
    Variant<int, float, double> v = 4.2f;
    auto visit = [&]() {
        v.visit(
            [](int& i) { std::cout << "int: " << i << std::endl; },
            [](float& f) { std::cout << "float: " << f << std::endl; },
            [](auto&) { std::cout << "other type" << std::endl; }
        );
    };
    visit();
    v = 39;
    visit();
    v = 834.194;
    visit();
}
