//
// Fan-out TCP health-checker: probe a list of hostnames concurrently and
// report UP/DOWN with round-trip latency.
// Demonstrates ResultGroup (fan-out/fan-in) and Semaphore throttle.
//
#include "tinyio/tinyio.hpp"

#include <chrono>
#include <string>
#include <vector>

#include <sys/socket.h>

#include <gflags/gflags.h>

using tinyio::Fd;
using tinyio::IO;
using tinyio::Result;
using tinyio::ResultGroup;
using tinyio::Semaphore;
using tinyio::SocketAddress;

using namespace std::chrono_literals;

namespace
{
DEFINE_uint64(port, 443, "Port to connect to");

struct Target
{
    std::string host;
    SocketAddress addr;
};

std::vector<Target> targets;

Result<double> probe_target(IO& io, const Target& t, Semaphore& gate) noexcept
{
    TRY_LOG_ERR(gate.acquire(), "semaphore acquire");
    const auto t0 = std::chrono::steady_clock::now();
    const Fd sock{::socket(t.addr.addr.ss_family, SOCK_STREAM | SOCK_CLOEXEC, 0)};

    if (!sock.is_valid())
    {
        gate.release();
        return tinyio::fail(errno, "socket");
    }

    const auto r = io.connect(sock, t.addr, 2s);
    gate.release();
    TRY_LOG_ERR(r, "connect");

    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void run_probes(IO& io) noexcept
{
    Semaphore gate(io, 64);  // at most 64 probes in flight
    ResultGroup<double> probes(io, targets.size());

    for (const auto& t : targets)
    {
        if (auto res =
                probes.spawn([&gate, &t](IO& fio) noexcept -> Result<double> { return probe_target(fio, t, gate); });
            !res)
        {
            ALOG_ERROR("failed to spawn probe for {}: {}", t.host, res.error());
        }
    }

    if (auto res = probes.wait(); !res)
    {
        ALOG_ERROR("probe group interrupted: {}", res.error());
    }

    for (size_t i = 0; i < targets.size(); ++i)
    {
        if (const auto& slot = probes.results()[i]; slot && slot->has_value())
        {
            ALOG_INFO("{:<24} UP    {:>6.1f} ms", targets[i].host, slot->value());
        }
        else if (slot)
        {
            ALOG_INFO("{:<24} DOWN  ({})", targets[i].host, slot->error().code == ETIMEDOUT ? "timeout" : "refused");
        }
    }
}
}  // namespace

int main(int argc, char** argv)
{
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    if (argc < 2)
    {
        ALOG_ERROR("usage: {} [flags] website1.com website2.com...", argv[0]);
        return 1;
    }

    // DNS is blocking -> resolve up front, before the event loop runs.
    for (int i = 1; i < argc; ++i)
    {
        std::string host = argv[i];
        auto a = tinyio::resolve_ip(host, static_cast<uint16_t>(FLAGS_port));
        if (!a)
        {
            ALOG_ERROR("{:<24} resolve failed ({})", host, a.error().code);
            continue;
        }
        targets.push_back({std::move(host), *a});
    }

    if (targets.empty())
    {
        ALOG_ERROR("no valid targets to probe");
        return 1;
    }

    ALOG_INFO("probing {} target(s) on port {}", targets.size(), FLAGS_port);

    IO io{0, tinyio::IoOptions{}, {}};

    if (auto res = io.spawn(run_probes); !res.has_value())
    {
        ALOG_ERROR("failed to spawn: {}", res.error());
        return 1;
    }

    io.run_until_done();
}
