//
// Multithreaded echo client using tinyio
//
#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <gflags/gflags.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "tinyio/tinyio.hpp"

using tinyio::IO;
using tinyio::Result;
using tinyio::SocketAddress;
using tinyio::UniqueFd;

namespace {
DEFINE_string(ip, "127.0.0.1", "IP address to connect to");
DEFINE_uint64(port, 8080, "Port to connect to");
DEFINE_uint64(threads, 4, "Number of OS threads");
DEFINE_uint64(clients_per_thread, 10, "Number of concurrent clients per thread");
DEFINE_uint64(messages, 100, "Number of messages per client");

std::atomic<uint64_t> total_success{0};
std::atomic<uint64_t> total_errors{0};

Result<void> run_client(IO& io, const uint64_t thread_id, const uint64_t client_id) {
  TRY(addr, SocketAddress::V4(static_cast<uint16_t>(FLAGS_port), FLAGS_ip.c_str()));
  UniqueFd sock{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
  if (!sock.Valid()) {
    return tinyio::Error::fail_errno(errno, "socket");
  }

  if (auto err = io.Connect(sock, addr); !err.has_value()) {
    return err;
  }

  const std::string msg = "Hello from thread " + std::to_string(thread_id) + " client " +
                          std::to_string(client_id);
  std::byte buf[4096];

  for (uint64_t i = 0; i < FLAGS_messages; ++i) {
    std::span to_write(reinterpret_cast<const std::byte*>(msg.data()), msg.size());
    while (!to_write.empty()) {
      TRY_LOG_ERR(written, io.Write(sock, to_write), "write to server");
      if (written == 0) return tinyio::Error::fail_errno(EIO, "write closed");
      to_write = to_write.subspan(written);
    }

    const size_t expected = msg.size();
    size_t received = 0;
    while (received < expected) {
      std::span<std::byte> read_buf(buf);
      TRY_LOG_ERR(n, io.Read(sock, read_buf.subspan(received)), "read from server");
      if (n == 0) return tinyio::Error::fail_errno(EIO, "read closed");
      received += n;
    }

    std::string_view rec_msg(reinterpret_cast<const char*>(buf), received);
    if (rec_msg != msg) {
      return tinyio::Error::fail_errno(EBADMSG, "corrupted message");
    }
  }

  ++total_success;
  TRY_LOG_ERR_VOID(io.Close(std::move(sock)), "close sock");
  return {};
}

void thread_worker(uint64_t thread_id) {
  IO io{static_cast<uint16_t>(thread_id), tinyio::IoOptions{}};

  for (uint64_t c = 0; c < FLAGS_clients_per_thread; ++c) {
    std::move_only_function<void(IO&) noexcept> task =
        [thread_id, c](IO& nio) mutable noexcept -> void {
      if (auto err = run_client(nio, thread_id, c); !err.has_value()) {
        ++total_errors;
        ELOGFMT(ERROR, "Client {}-{} error: [{}] {}", thread_id, c, err.error().context(),
                err.error().message());
      }
    };

    if (auto res = io.Spawn(std::move(task)); !res.has_value()) {
      ++total_errors;
      ELOGFMT(ERROR, "Failed to spawn client {}-{}", thread_id, c);
    }
  }

  io.RunUntilDone();
}
}  // namespace

int main(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  ELOGFMT(
      INFO,
      "Starting client with {} threads, {} clients per thread, {} messages per client",
      FLAGS_threads, FLAGS_clients_per_thread, FLAGS_messages);

  std::vector<std::thread> threads;
  for (uint64_t i = 0; i < FLAGS_threads; ++i) {
    threads.emplace_back(thread_worker, i);
  }

  for (auto& t : threads) {
    t.join();
  }

  ELOGFMT(INFO, "Total success: {}", total_success.load());
  ELOGFMT(INFO, "Total errors: {}", total_errors.load());

  return total_errors.load() == 0 ? 0 : 1;
}
