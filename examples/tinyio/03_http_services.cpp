// curl -v http://127.0.0.1:8080/
// curl -v --http1.1 -H 'Connection: close' http://127.0.0.1:8080/
//  printf 'GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n' | nc 127.0.0.1
//  8080

#include <chrono>
#include <csignal>
#include <iostream>
#include <string_view>
#include <thread>

#include "tinyio/tinyio.hpp"

using namespace tinyio;

namespace {
volatile std::sig_atomic_t g_stop_requested = 0;

struct Options {
  uint16_t port = 8080;
  size_t workers = 4;
  size_t stack_size = 64 * 1024;
};

void signal_handler(int) { g_stop_requested = 1; }
}  // namespace

// A static, valid HTTP/1.1 response with keep-alive
constexpr std::string_view kHttpResponse =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 16\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello, io_uring!";

// Write the whole buffer, retrying on partial writes
static Result<void> write_all(IO& fio, const UniqueFd& fd,
                              std::span<const std::byte> data) {
  while (!data.empty()) {
    TRY(written, fio.Write(fd, data));
    if (written == 0) {
      return Error::fail_errno(EIO, "write");
    }
    data = data.subspan(written);
  }
  return {};
}

// Handle a single client connection
static Result<void> handle_client(IO& fio, UniqueFd client_fd) noexcept {
  std::byte buf[1024];

  while (true) {
    TRY_LOG_ERR(r_n, fio.Read(client_fd, std::span{buf}), "failed to read from client");

    if (r_n == 0) {
      break;
    }

    const std::span out_buf(reinterpret_cast<const std::byte*>(kHttpResponse.data()),
                            kHttpResponse.size());

    if (auto res = write_all(fio, client_fd, out_buf); !res) {
      ELOGFMT(ERROR, "failed to write response: [{}] {}", res.error().context(),
              res.error().message());
      break;
    }
  }

  return {};
}

// Accept incoming connections on a single worker
static Result<void> server_loop(IO& fio, uint16_t port) {
  TRY_LOG_ERR(server_fd, TcpListener::Bind(port, "0.0.0.0", 4096), "failed to bind");

  ELOGFMT(INFO, "[Worker {}] Listening on http://0.0.0.0:{}", fio.Id(), port);

  return fio.AcceptLoop(
      std::move(server_fd), [](IO& worker_fio, Result<UniqueFd> client_fd_res) noexcept {
        if (!client_fd_res) {
          ELOGFMT(ERROR, "Accept failed: [{}] {}", client_fd_res.error().context(),
                  client_fd_res.error().message());
          return;
        }

        (void)worker_fio.Spawn(
            [client_fd = std::move(*client_fd_res)](IO& client_fio) mutable noexcept {
              if (auto res = handle_client(client_fio, std::move(client_fd)); !res) {
                ELOGFMT(ERROR, "Client handler failed: [{}] {}", res.error().context(),
                        res.error().message());
              }
            });
      });
}

int main() {
  // Update ALOG namespace based on new logging
  easylog::set_console(false);

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);
  // Ignore SIGPIPE to prevent the server from crashing when clients abruptly disconnect
  std::signal(SIGPIPE, SIG_IGN);

  constexpr size_t num_threads = 4;
  constexpr uint16_t port = 8080;

  std::vector<std::jthread> threads;
  std::vector<std::stop_source> stop_sources;

  std::cout << "HTTP Server starting. Press Ctrl+C to stop.\n";

  // Parallel accept on all workers (SO_REUSEPORT allows multiple threads to bind to the
  // same port)
  for (std::size_t i = 0; i < num_threads; ++i) {
    stop_sources.emplace_back();
    auto st = stop_sources.back().get_token();

    // Launch a new OS thread for each worker
    threads.emplace_back([i, st] {
      IoOptions opts{.sq_entries = 4096};
      IO worker(i, opts);

      // Spawn the initial accept loop fiber onto this worker
      (void)worker.Spawn([st](IO& fio) noexcept {
        if (auto res = server_loop(fio, port); !res) {
          ELOGFMT(ERROR, "Server loop failed: [{}] {}", res.error().context(),
                  res.error().message());
        }
      });

      // Start the I/O event loop (blocks until the stop token is triggered)
      worker.Run(st);
    });
  }

  std::cout << "HTTP Server running on http"
               "://127.0.0.1:"
            << port << "\n";

  // Wait for SIGINT / SIGTERM
  while (g_stop_requested == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  std::cout << "Shutting down workers...\n";

  // Trigger the stop token for all workers, cleanly interrupting the loops
  for (auto& source : stop_sources) {
    source.request_stop();
  }

  // std::jthread automatically joins on destruction, but we clear explicitly here
  threads.clear();

  std::cout << "Server shutdown complete. Goodbye!\n";
  return 0;
}