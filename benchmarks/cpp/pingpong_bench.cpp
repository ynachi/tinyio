// pingpong_bench.cpp
//
// Two fibers ping-pong one byte over a socketpair: each round trip is two
// writes and two reads, so it measures the whole syscall path (SQE, suspend,
// submit, reap, resume) with no network stack and no cross-thread machinery.
//
// The Rust twin is rust/benches/pingpong.rs; scripts/ab.sh runs them
// interleaved. Usage: pingpong_bench [rounds] [timeout_ms]; a non-zero timeout
// is put on every Read and Write, to measure the cost of arming one.
// Prints "ns/roundtrip: <f> ns".

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>

#include "tinyio/runtime/runtime.hpp"

int main(int argc, char** argv) {
    const std::size_t rounds = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 200000;
    const auto timeout = std::chrono::milliseconds(argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 0);

    std::array<int, 2> sv{};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv.data()) != 0) {
        std::cerr << "socketpair failed\n";
        return 1;
    }
    tinyio::UniqueFd a(sv[0]);
    tinyio::UniqueFd b(sv[1]);

    tinyio::IoOptions opts;
    tinyio::IO io(0, opts);

    double ns_per_roundtrip = 0.0;

    (void)io.Spawn([&](tinyio::IO& rt) noexcept {
        const auto start = std::chrono::steady_clock::now();
        std::byte byte{0};
        for (std::size_t i = 0; i < rounds; ++i) {
            if (!rt.Write(a, std::span<const std::byte>(&byte, 1), -1, timeout) ||
                !rt.Read(a, std::span<std::byte>(&byte, 1), -1, timeout)) {
                std::cerr << "ping failed\n";
                std::exit(1);
            }
        }
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        ns_per_roundtrip = static_cast<double>(ns) / static_cast<double>(rounds);
    });

    (void)io.Spawn([&](tinyio::IO& rt) noexcept {
        std::byte byte{0};
        for (std::size_t i = 0; i < rounds; ++i) {
            if (!rt.Read(b, std::span<std::byte>(&byte, 1), -1, timeout) ||
                !rt.Write(b, std::span<const std::byte>(&byte, 1), -1, timeout)) {
                std::cerr << "pong failed\n";
                std::exit(1);
            }
        }
    });

    io.RunUntilDone();
    std::cout << "ns/roundtrip: " << ns_per_roundtrip << " ns\n";
    return 0;
}
