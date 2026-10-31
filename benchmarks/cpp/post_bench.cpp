// post_bench.cpp
//
// Cross-thread dispatch: producer threads IO::Post() no-op tasks to a worker
// running Run(); the last task stops it. Each row is wall time per post, from
// the first post to the worker returning. Prints "post <n> producer(s): <f> ns/op".
// The Rust twin is rust/benches/post.rs; rust/scripts/ab.sh runs them interleaved.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stop_token>
#include <thread>
#include <vector>

#include "tinyio/runtime/runtime.hpp"

struct Ctx {
    std::atomic<size_t> done{0};
    size_t total = 0;
    std::stop_source st;
};

static double run(size_t producers, size_t per) {
    tinyio::IoOptions opts;
    tinyio::IO io(0, opts);
    Ctx ctx;
    ctx.total = producers * per;
    Ctx* c = &ctx;
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> ts;
    for (size_t p = 0; p < producers; ++p) {
        ts.emplace_back([&io, c, per] {
            for (size_t i = 0; i < per; ++i) {
                while (!io.Post([c](tinyio::IO&) noexcept {
                    if (c->done.fetch_add(1) + 1 == c->total) c->st.request_stop();
                })) {
                    std::this_thread::yield();
                }
            }
        });
    }
    io.Run(ctx.st.get_token());
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - start).count();
    for (auto& t : ts) t.join();
    return static_cast<double>(ns) / static_cast<double>(ctx.total);
}

int main() {
    constexpr size_t kTotal = 400000;
    std::printf("post 1 producer: %.1f ns/op\n", run(1, kTotal));
    std::printf("post 4 producers: %.1f ns/op\n", run(4, kTotal / 4));
}
