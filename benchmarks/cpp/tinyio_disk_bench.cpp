#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "tinyio/error.hpp"
#include "tinyio/net/fd.hpp"
#include "tinyio/runtime/fiber_sync.hpp"
#include "tinyio/runtime/runtime.hpp"

namespace {
using Clock = std::chrono::steady_clock;

struct Options {
    std::filesystem::path path = "/tmp/tinyio-disk-bench.dat";
    std::uint64_t bytes = 1024ull * 1024ull * 1024ull;
    std::size_t block = 64ull * 1024ull;
};

std::uint64_t parse_size(std::string_view s) {
    std::uint64_t multiplier = 1;
    if (!s.empty()) {
        const char suffix = s.back();
        if (suffix == 'k' || suffix == 'K') {
            multiplier = 1024;
            s.remove_suffix(1);
        } else if (suffix == 'm' || suffix == 'M') {
            multiplier = 1024ull * 1024ull;
            s.remove_suffix(1);
        } else if (suffix == 'g' || suffix == 'G') {
            multiplier = 1024ull * 1024ull * 1024ull;
            s.remove_suffix(1);
        }
    }
    return std::stoull(std::string{s}) * multiplier;
}

Options parse_args(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.starts_with("--path=")) {
            opts.path = arg.substr(std::strlen("--path="));
        } else if (arg.starts_with("--bytes=")) {
            opts.bytes = parse_size(arg.substr(std::strlen("--bytes=")));
        } else if (arg.starts_with("--block=")) {
            opts.block = static_cast<std::size_t>(parse_size(arg.substr(std::strlen("--block="))));
        } else {
            throw std::invalid_argument("unknown argument: " + arg);
        }
    }
    if (opts.block == 0 || opts.bytes == 0) {
        throw std::invalid_argument("bytes and block must be positive");
    }
    return opts;
}

void prepare_file(const std::filesystem::path& path, std::uint64_t bytes) {
    const int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    if (fd < 0) {
        throw std::runtime_error("open failed: " + std::string(std::strerror(errno)));
    }

    const int fallocate_res = ::posix_fallocate(fd, 0, static_cast<off_t>(bytes));
    if (fallocate_res != 0) {
        ::close(fd);
        throw std::runtime_error("posix_fallocate failed: " +
                                 std::string(std::strerror(fallocate_res)));
    }

    if (::close(fd) != 0) {
        throw std::runtime_error("close failed: " + std::string(std::strerror(errno)));
    }
}

struct BenchResult {
    std::string name;
    double seconds{};
    double mib_per_sec{};
};

void print_result(const BenchResult& r) {
    std::cout << "  " << r.name;
    for (std::size_t i = r.name.size(); i < 20; ++i) {
        std::cout << ' ';
    }
    std::cout << r.seconds << "s  " << r.mib_per_sec << " MiB/s\n";
}

tinyio::Result<void> tinyio_fiber_write_all(tinyio::IO& fio, tinyio::UniqueFd& fd,
                                            std::span<const std::byte> block, std::uint64_t bytes) {
    std::uint64_t offset = 0;
    while (offset < bytes) {
        const auto len = std::min<std::uint64_t>(block.size(), bytes - offset);
        iovec iov{.iov_base = const_cast<std::byte*>(block.data()),
                  .iov_len = static_cast<std::size_t>(len)};
        TRY(written, fio.Writev(fd, std::span<const iovec>{&iov, 1}, static_cast<off_t>(offset)));
        if (written != static_cast<int32_t>(len)) {
            return tinyio::Error::FailErrno(EIO, "writev");
        }
        offset += len;
    }
    return {};
}

BenchResult bench_tinyio_fiber(const Options& opts, std::span<const std::byte> block) {
    const auto path = opts.path.string() + ".tinyio";
    prepare_file(path, opts.bytes);

    const int raw_fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC | O_DIRECT);
    if (raw_fd < 0) {
        throw std::runtime_error("open tinyio file failed: " + std::string(std::strerror(errno)));
    }

    tinyio::UniqueFd fd{raw_fd};
    tinyio::IoOptions io_opts;
    tinyio::IO io(0, io_opts);

    std::optional<tinyio::Result<void>> result;

    const auto start = Clock::now();
    (void)io.Spawn([&](tinyio::IO& fio) noexcept {
        result = tinyio_fiber_write_all(fio, fd, block, opts.bytes);
    });

    io.RunUntilDone();
    const auto end = Clock::now();

    if (!result.value()) {
        throw std::runtime_error("tinyio write failed: " + result.value().error().Message());
    }

    const double seconds = std::chrono::duration<double>(end - start).count();
    return {"fibers=1", seconds, static_cast<double>(opts.bytes) / (1024.0 * 1024.0) / seconds};
}

// N fibers each write a non-overlapping segment of the file concurrently.
// All fibers are on the same thread/io_uring instance, so their SQEs batch
// together in a single io_uring_submit call per tick — this is the true QD>1
// test for io_uring batching efficiency.
BenchResult bench_tinyio_concurrent(const Options& opts, std::span<const std::byte> block,
                                    std::size_t n_fibers) {
    // Align segment size down to a multiple of block (required for O_DIRECT).
    const std::uint64_t segment = (opts.bytes / n_fibers / opts.block) * opts.block;
    if (segment == 0) {
        throw std::invalid_argument(
            "too many fibers: segment size rounded to zero, reduce --fibers or increase "
            "--bytes/--block");
    }
    const std::uint64_t total = segment * n_fibers;

    const auto path = opts.path.string() + ".concurrent";
    prepare_file(path, total);

    const int raw_fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC | O_DIRECT);
    if (raw_fd < 0) {
        throw std::runtime_error("open concurrent file failed: " +
                                 std::string(std::strerror(errno)));
    }

    tinyio::UniqueFd fd{raw_fd};
    tinyio::IoOptions io_opts;
    tinyio::IO io(0, io_opts);

    const auto start = Clock::now();

    (void)io.Spawn([&](tinyio::IO& fio) noexcept {
        tinyio::TaskGroup<std::uint64_t> group(fio, n_fibers);

        for (std::size_t i = 0; i < n_fibers; ++i) {
            const std::uint64_t seg_start = i * segment;
            const std::uint64_t seg_end = seg_start + segment;

            // Each fiber writes its own region; all use the same read-only
            // block buffer (safe: single thread, buffer is never modified).
            (void)group.Spawn([&fio, &fd, &block, seg_start,
                               seg_end](tinyio::IO&) noexcept -> tinyio::Result<std::uint64_t> {
                std::uint64_t offset = seg_start;
                while (offset < seg_end) {
                    const auto len = std::min<std::uint64_t>(block.size(), seg_end - offset);
                    iovec iov{.iov_base = const_cast<std::byte*>(block.data()),
                              .iov_len = static_cast<std::size_t>(len)};
                    TRY_VOID(fio.Writev(fd, std::span<const iovec>{&iov, 1},
                                        static_cast<off_t>(offset)));
                    offset += len;
                }
                return seg_end - seg_start;
            });
        }

        (void)group.Wait();
    });

    io.RunUntilDone();
    const auto end = Clock::now();

    const double seconds = std::chrono::duration<double>(end - start).count();
    const std::string name = "fibers=" + std::to_string(n_fibers);
    return {name, seconds, static_cast<double>(total) / (1024.0 * 1024.0) / seconds};
}
}  // namespace

int main(int argc, char** argv) {
    void* ptr = nullptr;
    try {
        const auto opts = parse_args(argc, argv);

        if (::posix_memalign(&ptr, 4096, opts.block) != 0) {
            throw std::runtime_error("posix_memalign failed");
        }
        std::memset(ptr, 0x42, opts.block);
        std::span<const std::byte> block(static_cast<const std::byte*>(ptr), opts.block);

        easylog::set_console(false);

        std::cout << "Benchmarking disk write (O_DIRECT, sequential per fiber):\n"
                  << "  Total bytes : " << opts.bytes << '\n'
                  << "  Block size  : " << opts.block << '\n'
                  << "  Path        : " << opts.path << '\n'
                  << '\n';

        print_result(bench_tinyio_fiber(opts, block));

        for (const std::size_t n : {2, 4, 8, 16, 32}) {
            print_result(bench_tinyio_concurrent(opts, block, n));
        }

        std::free(ptr);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        if (ptr) {
            std::free(ptr);
        }
        return 1;
    }
    return 0;
}
