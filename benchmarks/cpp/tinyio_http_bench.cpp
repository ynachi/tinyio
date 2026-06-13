#include "tinyio/runtime.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#include <tinyio/logger.hpp>
#include <tinyio/tcp_listener.hpp>

namespace
{
std::atomic_bool g_stop_requested{false};

constexpr std::string_view kHttpResponse =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 13\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello, World!";

struct Options
{
    uint16_t port = 8080;
    size_t workers = 4;
    size_t stack_size = 64 * 1024;
};

void signal_handler(int)
{
    g_stop_requested.store(true, std::memory_order_relaxed);
}

uint64_t parse_u64(std::string_view text)
{
    uint64_t value = 0;
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last)
    {
        throw std::invalid_argument("invalid integer argument");
    }
    return value;
}

Options parse_args(int argc, char** argv)
{
    Options opts;
    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg = argv[i];
        auto value_for = [&](std::string_view name) -> std::string_view
        {
            if (!arg.starts_with(name) || arg.size() <= name.size() || arg[name.size()] != '=')
            {
                return {};
            }
            return arg.substr(name.size() + 1);
        };

        if (auto value = value_for("--port"); !value.empty())
        {
            opts.port = static_cast<uint16_t>(parse_u64(value));
        }
        else if (auto value = value_for("--workers"); !value.empty())
        {
            opts.workers = static_cast<size_t>(parse_u64(value));
        }
        else if (auto value = value_for("--stack-size"); !value.empty())
        {
            opts.stack_size = static_cast<size_t>(parse_u64(value));
        }
        else
        {
            throw std::invalid_argument("unknown argument");
        }
    }

    if (opts.workers == 0 || opts.stack_size == 0)
    {
        throw std::invalid_argument("workers and stack-size must be positive");
    }
    return opts;
}

tinyio::Result<void> write_all(tinyio::IO& fio, tinyio::Fd& fd, std::span<const std::byte> data)
{
    while (!data.empty())
    {
        auto written = TRY(fio.write(fd, data));
        if (written == 0)
        {
            return tinyio::fail(EIO, "write");
        }
        data = data.subspan(written);
    }
    return {};
}

void handle_client(tinyio::IO& fio, int raw_fd)
{
    tinyio::Fd client_fd{raw_fd};
    std::byte buf[4096];
    const auto response = std::as_bytes(std::span{kHttpResponse.data(), kHttpResponse.size()});

    while (!g_stop_requested.load(std::memory_order_relaxed))
    {
        auto read_res = fio.read(client_fd, std::span{buf});
        if (!read_res || *read_res == 0)
        {
            return;
        }

        auto write_res = write_all(fio, client_fd, response);
        if (!write_res)
        {
            return;
        }
    }
}

void accept_loop(tinyio::IO& worker, uint16_t port, size_t stack_size)
{
    auto listener = tinyio::TcpListener::bind(port, "0.0.0.0", 4096);
    if (!listener)
    {
        std::cerr << "bind failed: " << listener.error().error_code().message() << '\n';
        return;
    }

    tinyio::Fd server_fd = std::move(*listener);
    while (!g_stop_requested.load(std::memory_order_relaxed))
    {
        auto client_res = worker.accept(server_fd);
        if (!client_res)
        {
            if (!g_stop_requested.load(std::memory_order_relaxed))
            {
                std::cerr << "accept failed: " << client_res.error().error_code().message() << '\n';
            }
            return;
        }

        const int raw_fd = client_res->release();
        (void)worker.spawn([raw_fd](tinyio::IO& fio) noexcept { handle_client(fio, raw_fd); });
    }
}
}  // namespace

int main(int argc, char** argv)
{
    try
    {
        const Options opts = parse_args(argc, argv);
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        std::signal(SIGPIPE, SIG_IGN);

        tinyio::log::set_level(tinyio::log::Level::Disabled);

        std::vector<std::jthread> threads;
        std::vector<std::stop_source> stop_sources;

        std::cout << "tinyio_http_bench listening on port " << opts.port << " workers=" << opts.workers
                  << " stack_size=" << opts.stack_size << '\n';

        for (size_t i = 0; i < opts.workers; ++i)
        {
            stop_sources.emplace_back();
            auto st = stop_sources.back().get_token();
            threads.emplace_back(
                [i, opts, st]
                {
                    tinyio::IoOptions io_opts;
                    io_opts.stack_size = opts.stack_size;
                    tinyio::IO worker(i, io_opts, {});

                    (void)worker.spawn([opts](tinyio::IO& io) noexcept
                                       { accept_loop(io, opts.port, opts.stack_size); });

                    worker.run(st);
                });
        }

        while (!g_stop_requested.load(std::memory_order_relaxed))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        for (auto& source : stop_sources)
        {
            source.request_stop();
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
