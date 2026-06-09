#include "tinyio/runtime.hpp"
#include <iostream>
#include <chrono>

int main() {
    tinyio::IoOptions opts;
    tinyio::IO io(0, opts, {});
    
    constexpr size_t iterations = 10000000;
    
    io.spawn([&](tinyio::IO& rt) noexcept {
        auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < iterations; ++i) {
            auto res = rt.yield();
            if (!res) {
                std::cerr << "yield failed\n";
                break;
            }
        }
        auto end = std::chrono::steady_clock::now();
        
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        // Each yield entails 2 context switches: fiber -> scheduler -> fiber
        auto switches = iterations * 2;
        auto ns_per_switch = static_cast<double>(ns) / switches;
        
        std::cout << "Switches: " << switches << "\n";
        std::cout << "Time: " << ns / 1000000.0 << " ms\n";
        std::cout << "ns/switch: " << ns_per_switch << " ns\n";
        
        exit(0);
    });
    
    std::stop_source st;
    io.run(st.get_token());
    return 0;
}
