#include <functional>
#include <iostream>

struct IO {};

int main() {
    auto f1 = [](IO&) noexcept -> void {};
    auto f2 = [](IO&) mutable noexcept -> void {};
    int c = 0;
    auto f3 = [c](IO&) noexcept -> void {};
    auto f4 = [c](IO&) mutable noexcept -> void {};

    std::cout << std::is_constructible_v<std::move_only_function<void(IO&) noexcept>, decltype(f1)> << std::endl;
    std::cout << std::is_constructible_v<std::move_only_function<void(IO&) noexcept>, decltype(f2)> << std::endl;
    std::cout << std::is_constructible_v<std::move_only_function<void(IO&) noexcept>, decltype(f3)> << std::endl;
    std::cout << std::is_constructible_v<std::move_only_function<void(IO&) noexcept>, decltype(f4)> << std::endl;
}
