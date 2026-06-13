// curl -v http://127.0.0.1:8080/
// curl -v --http1.1 -H 'Connection: close' http://127.0.0.1:8080/
//  printf 'GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n' | nc 127.0.0.1 8080

#include "tinyio/tinyio.hpp"

#include <chrono>
#include <csignal>
#include <iostream>
#include <string_view>
#include <thread>

using namespace tinyio;

namespace
{
constexpr bool kUseRemoteDispatch = false;
volatile std::sig_atomic_t g_stop_requested = 0;

struct Options
{
    uint16_t port = 8080;
    size_t workers = 4;
    size_t stack_size = 64 * 1024;
};

void signal_handler(int)
{
    g_stop_requested = 1;
}
}  // namespace

// A static, valid HTTP/1.1 response with keep-alive
constexpr std::string_view kHttpResponse =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 16\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello, io_uring!";

// Handle a single client connection
static Result<void> handle_client(IO& fio, Fd client_fd) noexcept
{
    std::byte buf[1024];

    while (true)
    {
        auto r_n = TRY_LOG_ERR(fio.read(client_fd, std::span{buf}), "failed to read from client");

        if (r_n == 0)
        {
            break;
        }

        const std::span out_buf(reinterpret_cast<const std::byte*>(kHttpResponse.data()), kHttpResponse.size());

        if (const auto w_n = TRY_LOG_ERR(fio.write(client_fd, out_buf), "failed to write to client"); w_n == 0)
        {
            break;
        }
    }

    return {};
}

static Result<void> write_all(IO& fio, const Fd& fd, std::span<const std::byte> data)
{
    while (!data.empty())
    {
        const auto written = TRY(fio.write(fd, data));
        if (written == 0)
        {
            return fail(EIO, "write");
        }
        data = data.subspan(written);
    }
    return {};
}
// Accept incoming connections on a single worker
static Result<void> server_loop(IO& fio, uint16_t port, std::stop_token st)
{
    auto server_fd = TRY_LOG_ERR(TcpListener::bind(port, "0.0.0.0", 4096), "failed to bind");

    ALOG_INFO("[Worker {}] Listening on http://0.0.0.0:{}", fio.id(), port);

    while (!st.stop_requested())
    {
        auto client_fd = TRY_LOG_ERR(fio.accept(server_fd), "failed to accept");

        // Schedule the client handler on the same worker
        (void)fio.spawn(
            [client_fd = std::move(client_fd)](IO& worker_fio) mutable noexcept
            {
                // If handle_client returns a Result, you might want to log it if it fails
                if (auto res = handle_client(worker_fio, std::move(client_fd)); !res)
                {
                    ALOG_ERROR("Client handler failed: {}", res.error().error_code().message());
                }
            });
    }

    return {};
}

int main()
{
    // Update ALOG namespace based on new logging
    tinyio::log::set_level(log::Level::Disabled);

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    // Ignore SIGPIPE to prevent the server from crashing when clients abruptly disconnect
    std::signal(SIGPIPE, SIG_IGN);

    constexpr size_t num_threads = 4;
    constexpr uint16_t port = 8080;

    std::vector<std::jthread> threads;
    std::vector<std::stop_source> stop_sources;

    std::cout << "HTTP Server starting. Press Ctrl+C to stop.\n";

    // Parallel accept on all workers (SO_REUSEPORT allows multiple threads to bind to the same port)
    for (std::size_t i = 0; i < num_threads; ++i)
    {
        stop_sources.emplace_back();
        auto st = stop_sources.back().get_token();

        // Launch a new OS thread for each worker
        threads.emplace_back(
            [i, port, st]
            {
                IoOptions opts;
                IO worker(i, opts, {});

                // Spawn the initial accept loop fiber onto this worker
                (void)worker.spawn(
                    [port, st](IO& fio) noexcept
                    {
                        if (auto res = server_loop(fio, port, st); !res)
                        {
                            ALOG_ERROR("Server loop failed: {}", res.error().error_code().message());
                        }
                    });

                // Start the I/O event loop (blocks until the stop token is triggered)
                worker.run(st);
            });
    }

    std::cout << "HTTP Server running on http"
                 "://127.0.0.1:"
              << port << "\n";

    // Wait for SIGINT / SIGTERM
    while (g_stop_requested == 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "Shutting down workers...\n";

    // Trigger the stop token for all workers, cleanly interrupting the loops
    for (auto& source : stop_sources)
    {
        source.request_stop();
    }

    // std::jthread automatically joins on destruction, but we clear explicitly here
    threads.clear();

    std::cout << "Server shutdown complete. Goodbye!\n";
    return 0;
}