#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

#include "tinyio/tinyio.hpp"

using namespace tinyio;
using namespace std::chrono;

namespace {
volatile std::sig_atomic_t g_stop_requested = 0;
void signal_handler(int) { g_stop_requested = 1; }
}  // namespace

Result<void> DoWrite(IO& fio, const std::string& filename) {
  auto fd = TRY_LOG_ERR(fio.Open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0644), "open");

  std::vector<uint8_t> block(64 * 1024, 'A');
  constexpr std::int64_t total_size = 1024LL * 1024 * 1024;

  auto start = steady_clock::now();
  std::int64_t offset = 0;
  while (offset < total_size) {
    TRY_LOG_ERR(
        fio.Write(
            fd, std::span{reinterpret_cast<const std::byte*>(block.data()), block.size()},
            offset),
        "write");
    offset += static_cast<std::int64_t>(block.size());
  }
  TRY(fio.Close(std::move(fd)));

  auto end = steady_clock::now();
  std::cout << "Sequential Write (1GB): "
            << duration_cast<milliseconds>(end - start).count() << " ms\n";
  return {};
}

Result<void> DoRead(IO& fio, const std::string& filename) {
  auto raw_fd = TRY_LOG_ERR(fio.Open(filename, O_RDONLY), "open");
  auto fd = TRY_LOG_ERR(fio.RegisterFile(std::move(raw_fd)), "register_file");

  std::vector<uint8_t> block(4096);
  constexpr std::int64_t total_size = 1024LL * 1024 * 1024;
  constexpr size_t num_reads = 100000;

  std::mt19937_64 rng(42);
  std::uniform_int_distribution<off_t> dist(0, total_size - 4096);

  auto start = steady_clock::now();
  for (size_t i = 0; i < num_reads; ++i) {
    off_t offset = dist(rng);
    TRY_LOG_ERR(
        fio.Read(fd, std::span{reinterpret_cast<std::byte*>(block.data()), block.size()},
                 offset),
        "read");
  }
  TRY(fio.Close(std::move(fd)));

  auto end = steady_clock::now();
  std::cout << "Random Read (100k 4KB reads): "
            << duration_cast<milliseconds>(end - start).count() << " ms\n";
  return {};
}

Result<void> handle_client(IO& fio, Fd client_fd) noexcept {
  std::byte buf[4096];
  while (true) {
    auto r_n = TRY(fio.Read(client_fd, std::span{buf}));
    if (r_n == 0) break;

    const std::string response =
        "HTTP/1.1 200 OK\r\nContent-Length: 13\r\nConnection: keep-alive\r\n\r\nHello, "
        "World!";
    if (!fio.Write(client_fd,
                   std::span{reinterpret_cast<const std::byte*>(response.data()),
                             response.size()}))
      break;
  }
  return {};
}

Result<void> server_loop(IO& fio, uint16_t port) {
  auto server_fd = TRY_LOG_ERR(TcpListener::Bind(port, "0.0.0.0", 4096), "bind");
  ALOG_INFO("[Worker {}] Listening on http://0.0.0.0:{}", fio.Id(), port);

  return fio.AcceptLoop(std::move(server_fd),
                        [](IO& worker_fio, Result<Fd> client_fd) noexcept {
                          if (client_fd) {
                            (void)handle_client(worker_fio, std::move(*client_fd));
                          }
                        });
}

int main(int argc, char* argv[]) {
  tinyio::log::SetLevel(log::Level::Disabled);
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);
  std::signal(SIGPIPE, SIG_IGN);

  std::string mode = "write";
  if (argc > 1) mode = argv[1];

  const std::string filename = "bench.dat";
  IoOptions opts{.sq_entries = 4096};

  if (mode == "write" || mode == "read") {
    IO worker(0, opts, {});
    (void)worker.Spawn([&](IO& fio) noexcept {
      if (mode == "write")
        (void)DoWrite(fio, filename);
      else if (mode == "read")
        (void)DoRead(fio, filename);
      g_stop_requested = 1;
    });

    std::stop_source st;
    std::thread t([&] {
      while (!g_stop_requested) std::this_thread::sleep_for(milliseconds(100));
      st.request_stop();
    });
    worker.Run(st.get_token());
    t.join();
  } else if (mode == "server") {
    constexpr size_t num_threads = 4;
    constexpr uint16_t port = 8082;
    std::vector<std::jthread> threads;
    std::vector<std::stop_source> stop_sources;

    for (std::size_t i = 0; i < num_threads; ++i) {
      stop_sources.emplace_back();
      auto st = stop_sources.back().get_token();
      threads.emplace_back([i, st, opts] {
        IO worker(i, opts, {});
        // port is constexpr, so the inner lambda does not have to capture it.
        (void)worker.Spawn([](IO& fio) noexcept { (void)server_loop(fio, port); });
        worker.Run(st);
      });
    }

    while (g_stop_requested == 0) std::this_thread::sleep_for(milliseconds(200));
    for (auto& source : stop_sources) source.request_stop();
    threads.clear();
  }
  return 0;
}
