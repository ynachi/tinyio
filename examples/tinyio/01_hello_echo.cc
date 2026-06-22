//
// Simple echo server to demonstrate a concurrent, single threaded network server
// The code looks blocking but is in fact, non-blocking
// Demonstrate
//  - timout
//  - error management
#include <chrono>

#include <gflags/gflags.h>

#include "../../src/tinyio/tinyio.hpp"

using tinyio::Fd;
using tinyio::IO;
using tinyio::Result;

using namespace std::chrono_literals;

namespace
{
DEFINE_string(ip, "0.0.0.0", "IP address to bind to");
DEFINE_uint64(port, 8080, "Port to listen on");

Result<void> write_all(IO& fio, const Fd& fd, std::span<const std::byte> data)
{
    while (!data.empty())
    {
        const auto written = TRY_LOG_ERR(fio.write(fd, data), "failed to write to client");
        if (written == 0)
        {
            return tinyio::fail(EIO, "write");
        }
        data = data.subspan(written);
    }
    return {};
}

Result<void> handle_client(IO& io, Fd client)
{
    std::byte buf[4096];
    while (true)
    {
        // Looks blocking yet asynchronous
        // TRY_LOG_ERR, unwrap the value if any or log the error and propagate it to
        // the caller. Why is it useful here ? Because the log line would point us to
        // this exact source location which caused the error, diving us plenty of
        // context to debug.
        // This macro revealed to be almost zero overhead during micro benchs so don't be
        // afraid to use it.
        // Note: if this fail, the function will exit and the Fd close will not run
        // But the fd destructor will close it anyway. The only drawback is that it does
        // it synchronously.
        const auto n = TRY_LOG_ERR(io.read(client, buf), "failed to read from client");

        if (n == 0)
        {
            ALOG_INFO("client disconnected");
            break;
        }

        // another way to manage error
        if (auto err = write_all(io, client, std::span<const std::byte>(buf).first(n)); !err.has_value())
        {
            ALOG_ERROR("failed to write to client, err = {}", err.error());
            break;
        }
    }

    TRY_LOG_ERR(io.close(std::move(client)), "failed to close client fd");
    return {};
}

void server_loop(IO& io) noexcept
{
    // closed by the destructor.
    // Need to better handle shutdown
    const auto listener = tinyio::TcpListener::bind(FLAGS_port, FLAGS_ip.c_str());
    if (!listener)
    {
        ALOG_ERROR("failed to bind tcp listener: {}", FLAGS_port);
        ALOG_ERROR("{}", listener.error());
        return;
    }

    while (true)
    {
        // Timeout in action, if no client after 30s, we just kill the server
        auto client = io.accept(*listener, 30s);
        if (!client)
        {
            ALOG_ERROR("failed to accept connexion: {}", FLAGS_port);
            ALOG_ERROR("{}", client.error());
            return;
        }

        if (auto err = io.spawn([c = std::move(*client)](IO& nio) mutable noexcept -> Result<void>
                                { return handle_client(nio, std::move(c)); });
            !err.has_value())
        {
            ALOG_ERROR("failed to accept client: {}", err.error());
        }
    }
}
}  // namespace

int main()
{
    ALOG_INFO("listening on {}:{}", FLAGS_ip, FLAGS_port);

    IO io{0, tinyio::IoOptions{}, {}};

    if (auto res = io.spawn(server_loop); !res.has_value())
    {
        ALOG_ERROR("failed to spawn server: {}", res.error());
        std::abort();
    }

    // accept fiber never exits: Ctrl-C to quit
    io.run_until_done();
}
