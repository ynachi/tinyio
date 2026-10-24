//
// Fan-out TCP health-checker: probe a list of hostnames concurrently and
// report UP/DOWN with round-trip latency.
// Demonstrates TaskGroup (fan-out/fan-in) and Semaphore throttle.
//
#include <chrono>
#include <string>
#include <vector>

#include <gflags/gflags.h>
#include <sys/socket.h>

#include "tinyio/tinyio.hpp"

using tinyio::IO;
using tinyio::Result;
using tinyio::Semaphore;
using tinyio::SocketAddress;
using tinyio::TaskGroup;
using tinyio::UniqueFd;

using namespace std::chrono_literals;

namespace {
DEFINE_uint64(port, 443, "Port to connect to");

struct Target {
  std::string host;
  SocketAddress addr;
};

std::vector<Target> targets;

Result<double> probe_target(IO& io, const Target& t, Semaphore& gate) noexcept {
  TRY_LOG_ERR_VOID(gate.Acquire(), "semaphore acquire");
  const auto t0 = std::chrono::steady_clock::now();
  const UniqueFd sock{::socket(t.addr.addr.ss_family, SOCK_STREAM | SOCK_CLOEXEC, 0)};

  if (!sock.Valid()) {
    gate.Release();
    return tinyio::Error::FailErrno(errno, "socket");
  }

  const auto r = io.Connect(sock, t.addr, 2s);
  gate.Release();
  TRY_LOG_ERR_VOID(r, "connect");

  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
      .count();
}

void run_probes(IO& io) noexcept {
  Semaphore gate(io, 64);  // at most 64 probes in flight
  TaskGroup<double> probes(io, targets.size());

  for (const auto& t : targets) {
    if (auto res = probes.Spawn([&gate, &t](IO& fio) noexcept -> Result<double> {
          return probe_target(fio, t, gate);
        });
        !res) {
      ELOGFMT(ERROR, "failed to spawn probe for {}: [{}] {}", t.host,
              res.error().Context(), res.error().Message());
    }
  }

  // everyone wait here, the line completed before we move on
  if (auto res = probes.Wait(); !res) {
    ELOGFMT(ERROR, "probe group interrupted: [{}] {}", res.error().Context(),
            res.error().Message());
  }

  for (size_t i = 0; i < targets.size(); ++i) {
    if (const auto& slot = probes.Results()[i]; slot && slot->has_value()) {
      ELOGFMT(INFO, "{:<24} UP    {:>6.1f} ms", targets[i].host, slot->value());
    } else if (slot) {
      ELOGFMT(INFO, "{:<24} DOWN  ({})", targets[i].host,
              slot->error().Value() == ETIMEDOUT ? "timeout" : "refused");
    }
  }
}
}  // namespace

int main(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (argc < 2) {
    ELOGFMT(ERROR, "usage: {} [flags] website1.com website2.com...", argv[0]);
    return 1;
  }

  // DNS is blocking -> resolve up front, before the event loop runs.
  for (int i = 1; i < argc; ++i) {
    std::string host = argv[i];
    auto a = tinyio::ResolveIp(host, static_cast<uint16_t>(FLAGS_port));
    if (!a) {
      ELOGFMT(ERROR, "{:<24} resolve failed ({})", host, a.error().Value());
      continue;
    }
    targets.push_back({std::move(host), *a});
  }

  if (targets.empty()) {
    ELOGFMT(ERROR, "no valid targets to probe");
    return 1;
  }

  ELOGFMT(INFO, "probing {} target(s) on port {}", targets.size(), FLAGS_port);

  IO io{0, tinyio::IoOptions{}};

  if (auto res = io.Spawn(run_probes); !res.has_value()) {
    ELOGFMT(ERROR, "failed to spawn: [{}] {}", res.error().Context(),
            res.error().Message());
    return 1;
  }

  io.RunUntilDone();
}
