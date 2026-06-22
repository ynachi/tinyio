#include <chrono>
#include <csignal>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

#include "../../src/tinyio/tinyio.hpp"

using namespace tinyio;
using namespace std::chrono;

namespace {
    volatile std::sig_atomic_t g_stop_requested = 0;
    void signal_handler(int) { g_stop_requested = 1; }
}

Result<void> DoWrite(IO& fio, const std::string& filename) {
    auto fd = TRY_LOG_ERR(fio.open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0644), "open");
    
    std::vector<uint8_t> block(64 * 1024, 'A');
    const size_t total_size = 1024 * 1024 * 1024;
    
    auto start = steady_clock::now();
    off_t offset = 0;
    while (offset < total_size) {
        TRY_LOG_ERR(fio.write(fd, std::span{reinterpret_cast<const std::byte*>(block.data()), block.size()}, offset), "write");
        offset += block.size();
    }
    TRY(fio.close(std::move(fd)));
    
    auto end = steady_clock::now();
    std::cout << "Sequential Write (1GB): " 
              << duration_cast<milliseconds>(end - start).count() << " ms\n";
    return {};
}

Result<void> DoRead(IO& fio, const std::string& filename) {
    auto raw_fd = TRY_LOG_ERR(fio.open(filename, O_RDONLY), "open");
    auto fd = TRY_LOG_ERR(fio.register_file(std::move(raw_fd)), "register_file");

    
    std::vector<uint8_t> block(4096);
    const size_t total_size = 1024 * 1024 * 1024;
    const size_t num_reads = 100000;
    
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<off_t> dist(0, total_size - 4096);
    
    auto start = steady_clock::now();
    for (size_t i = 0; i < num_reads; ++i) {
        off_t offset = dist(rng);
        TRY_LOG_ERR(fio.read(fd, std::span{reinterpret_cast<std::byte*>(block.data()), block.size()}, offset), "read");
    }
    TRY(fio.close(std::move(fd)));
    
    auto end = steady_clock::now();
    std::cout << "Random Read (100k 4KB reads): " 
              << duration_cast<milliseconds>(end - start).count() << " ms\n";
    return {};
}

Result<void> handle_client(IO& fio, Fd client_fd, const std::string& filename) noexcept {
    std::byte buf[4096];
    while (true) {
        auto r_n = TRY(fio.read(client_fd, std::span{buf}));
        if (r_n == 0) break;
        
        const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 13\r\nConnection: keep-alive\r\n\r\nHello, World!";
        if (!fio.write(client_fd, std::span{reinterpret_cast<const std::byte*>(response.data()), response.size()})) break;
    }
    return {};
}

Result<void> server_loop(IO& fio, uint16_t port, std::stop_token st, const std::string& filename) {
    auto server_fd = TRY_LOG_ERR(TcpListener::bind(port, "0.0.0.0", 4096), "bind");
    ALOG_INFO("[Worker {}] Listening on http://0.0.0.0:{}", fio.id(), port);

    return fio.accept_loop(std::move(server_fd), [filename](IO& worker_fio, Result<Fd> client_fd) noexcept {
        if (client_fd) {
            (void)handle_client(worker_fio, std::move(*client_fd), filename);
        }
    });
}

int main(int argc, char* argv[]) {
    tinyio::log::set_level(log::Level::Disabled);
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);

    std::string mode = "write";
    if (argc > 1) mode = argv[1];
    
    const std::string filename = "bench.dat";
    IoOptions opts{.sq_entries = 4096};
    
    if (mode == "write" || mode == "read") {
        IO worker(0, opts, {});
        worker.spawn([&](IO& fio) noexcept {
            if (mode == "write") (void)DoWrite(fio, filename);
            else if (mode == "read") (void)DoRead(fio, filename);
            g_stop_requested = 1;
        });
        
        std::stop_source st;
        std::thread t([&]{
            while (!g_stop_requested) std::this_thread::sleep_for(milliseconds(100));
            st.request_stop();
        });
        worker.run(st.get_token());
        t.join();
    } else if (mode == "server") {
        constexpr size_t num_threads = 4;
        constexpr uint16_t port = 8082;
        std::vector<std::jthread> threads;
        std::vector<std::stop_source> stop_sources;

        for (std::size_t i = 0; i < num_threads; ++i) {
            stop_sources.emplace_back();
            auto st = stop_sources.back().get_token();
            threads.emplace_back([i, port, st, filename, opts] {
                IO worker(i, opts, {});
                (void)worker.spawn([port, st, filename](IO& fio) noexcept {
                    (void)server_loop(fio, port, st, filename);
                });
                worker.run(st);
            });
        }
        
        while (g_stop_requested == 0) std::this_thread::sleep_for(milliseconds(200));
        for (auto& source : stop_sources) source.request_stop();
        threads.clear();
    }
    return 0;
}
