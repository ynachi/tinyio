// tinyio_dispatch_bench.cpp
//
// HTTP benchmark that exercises tinyio's cross-thread dispatch path.
//
// Topology:
//   Worker 0  — acceptor: runs a single accept loop; hands each accepted fd
//               to a handler via IO::Post() (the lock-free MPSC + eventfd
//               wakeup path).
//   Workers 1..N-1 — handlers: receive fds from the MPSC queue and handle
//               the full HTTP keep-alive loop on their own io_uring.
//
// When --workers=1 there are no separate handler threads; the single worker
// accepts and handles everything itself (no cross-thread dispatch).
//
// Compare against tinyio_http_bench (SO_REUSEPORT, every thread accepts) to
// isolate the cost of the dispatch mechanism.

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <future>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#include "tinyio/logging.hpp"
#include "tinyio/net/tcp_listener.hpp"
#include "tinyio/runtime/runtime.hpp"

namespace {

std::atomic_bool g_stop_requested{false};

constexpr std::string_view kHttpResponse =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 13\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello, World!";

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

struct Options {
    uint16_t port = 8080;
    size_t workers = 4;
    size_t stack_size = 64 * 1024;
};

void signal_handler(int) { g_stop_requested.store(true, std::memory_order_relaxed); }

uint64_t parse_u64(std::string_view text) {
    uint64_t v = 0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        throw std::invalid_argument("invalid integer argument");
    }
    return v;
}

Options parse_args(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        auto value_for = [&](std::string_view name) -> std::string_view {
            if (!arg.starts_with(name) || arg.size() <= name.size() || arg[name.size()] != '=') {
                return {};
            }
            return arg.substr(name.size() + 1);
        };

        if (auto v = value_for("--port"); !v.empty()) {
            opts.port = static_cast<uint16_t>(parse_u64(v));
        } else if (auto v = value_for("--workers"); !v.empty()) {
            opts.workers = static_cast<size_t>(parse_u64(v));
        } else if (auto v = value_for("--stack-size"); !v.empty()) {
            opts.stack_size = static_cast<size_t>(parse_u64(v));
        } else {
            throw std::invalid_argument(std::string("unknown argument: ") + std::string(arg));
        }
    }
    if (opts.workers == 0 || opts.stack_size == 0) {
        throw std::invalid_argument("workers and stack-size must be positive");
    }
    return opts;
}

// ---------------------------------------------------------------------------
// Hot path — called inside a fiber on the handler IO
// ---------------------------------------------------------------------------

tinyio::Result<void> write_all(tinyio::IO& fio, tinyio::UniqueFd& fd,
                               std::span<const std::byte> data) noexcept {
    while (!data.empty()) {
        TRY(written, fio.Write(fd, data));
        if (written == 0) {
            return tinyio::Error::FailErrno(EIO, "write");
        }
        data = data.subspan(static_cast<size_t>(written));
    }
    return {};
}

void handle_client(tinyio::IO& fio, int raw_fd) noexcept {
    tinyio::UniqueFd client_fd{raw_fd};
    std::byte buf[4096];
    const auto response = std::as_bytes(std::span{kHttpResponse.data(), kHttpResponse.size()});

    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        auto read_res = fio.Read(client_fd, std::span{buf});
        if (!read_res || *read_res == 0) {
            return;
        }

        if (!write_all(fio, client_fd, response)) {
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Acceptor fiber — runs on worker 0
//
// For each accepted connection, dispatches the raw fd to a handler worker via
// IO::Post().  If handlers is empty (--workers=1) it spawns the handler fiber
// locally instead.
// ---------------------------------------------------------------------------

void accept_and_dispatch(tinyio::IO& acceptor, std::vector<tinyio::IO*> handlers,
                         uint16_t port) noexcept {
    auto listener = tinyio::TcpListener::Bind(port, "0.0.0.0", 4096);
    if (!listener) {
        std::cerr << "[acceptor] bind failed: " << listener.error().Message() << '\n';
        return;
    }

    tinyio::UniqueFd server_fd = std::move(*listener);
    size_t next = 0;

    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        auto client = acceptor.Accept(server_fd);
        if (!client) {
            if (!g_stop_requested.load(std::memory_order_relaxed)) {
                std::cerr << "[acceptor] accept failed: " << client.error().Message() << '\n';
            }
            return;
        }

        const int raw_fd = client->Release();

        if (handlers.empty()) {
            // Single-worker mode: no cross-thread dispatch, handle locally.
            (void)acceptor.Spawn([raw_fd](tinyio::IO& io) noexcept { handle_client(io, raw_fd); });
        } else {
            // Cross-thread dispatch: post the fd to the next handler (round-robin).
            tinyio::IO& target = *handlers[next++ % handlers.size()];
            if (!target.Post([raw_fd](tinyio::IO& io) noexcept { handle_client(io, raw_fd); })) {
                // Post() returned false (OOM); close the fd rather than leak it.
                ::close(raw_fd);
            }
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    try {
        const Options opts = parse_args(argc, argv);
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        std::signal(SIGPIPE, SIG_IGN);

        easylog::set_console(false);

        const size_t n = opts.workers;
        std::cout << "tinyio_dispatch_bench port=" << opts.port << " workers=" << n
                  << " (1 acceptor + " << (n > 1 ? n - 1 : 0) << " handlers)"
                  << " stack_size=" << opts.stack_size << '\n';

        // Each worker thread creates its IO on its own stack (owner_tid_ is
        // recorded in the constructor), signals the pointer via a promise, then
        // enters run().  We collect the pointers before posting the accept fiber.
        std::vector<std::promise<tinyio::IO*>> io_promises(n);
        std::vector<std::future<tinyio::IO*>> io_futures;
        io_futures.reserve(n);
        for (auto& p : io_promises) {
            io_futures.push_back(p.get_future());
        }

        std::vector<std::stop_source> stop_sources(n);
        std::vector<std::jthread> threads;
        threads.reserve(n);

        for (size_t i = 0; i < n; ++i) {
            threads.emplace_back([i, opts, &promise = io_promises[i],
                                  st = stop_sources[i].get_token()] mutable noexcept {
                tinyio::IoOptions io_opts;
                io_opts.stack_size = opts.stack_size;
                tinyio::IO worker(i, io_opts);

                // Signal the IO pointer before entering run() so that the
                // main thread can Post() the accept fiber into the queue.
                promise.set_value(&worker);

                worker.Run(st);
                // worker destroyed here — safe because stop has been requested
                // before threads.clear() in main, so no Post() can arrive late.
            });
        }

        // Wait for all IO instances to be initialised.
        std::vector<tinyio::IO*> ios;
        ios.reserve(n);
        for (auto& f : io_futures) {
            ios.push_back(f.get());
        }

        // Build handler list: workers 1..N-1.
        std::vector<tinyio::IO*> handlers(ios.begin() + (n > 1 ? 1 : 0), ios.end());

        // Post the acceptor fiber to worker 0 via the cross-thread MPSC queue.
        // run() will drain it on the very first tick().
        if (!ios[0]->Post([handlers = std::move(handlers),
                           port = opts.port](tinyio::IO& io) mutable noexcept {
                accept_and_dispatch(io, std::move(handlers), port);
            })) {
            std::cerr << "failed to post accept fiber to worker 0\n";
            for (auto& src : stop_sources) {
                src.request_stop();
            }
            return 1;
        }

        while (!g_stop_requested.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        for (auto& src : stop_sources) {
            src.request_stop();
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
