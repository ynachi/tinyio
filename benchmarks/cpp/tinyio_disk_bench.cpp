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

#include <fcntl.h>
#include <unistd.h>

#include <tinyio/runtime.hpp>
#include <tinyio/fd.hpp>
#include <tinyio/error.hpp>

namespace
{
using Clock = std::chrono::steady_clock;

struct Options
{
    std::filesystem::path path = "/tmp/tinyio-disk-bench.dat";
    std::uint64_t bytes = 1024ull * 1024ull * 1024ull;
    std::size_t block = 64ull * 1024ull;
};

std::uint64_t parse_size(std::string_view s)
{
    std::uint64_t multiplier = 1;
    if (!s.empty())
    {
        const char suffix = s.back();
        if (suffix == 'k' || suffix == 'K')
        {
            multiplier = 1024;
            s.remove_suffix(1);
        }
        else if (suffix == 'm' || suffix == 'M')
        {
            multiplier = 1024ull * 1024ull;
            s.remove_suffix(1);
        }
        else if (suffix == 'g' || suffix == 'G')
        {
            multiplier = 1024ull * 1024ull * 1024ull;
            s.remove_suffix(1);
        }
    }
    return std::stoull(std::string{s}) * multiplier;
}

Options parse_args(int argc, char** argv)
{
    Options opts;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg.starts_with("--path="))
        {
            opts.path = arg.substr(std::strlen("--path="));
        }
        else if (arg.starts_with("--bytes="))
        {
            opts.bytes = parse_size(arg.substr(std::strlen("--bytes=")));
        }
        else if (arg.starts_with("--block="))
        {
            opts.block = static_cast<std::size_t>(parse_size(arg.substr(std::strlen("--block="))));
        }
        else
        {
            throw std::invalid_argument("unknown argument: " + arg);
        }
    }
    if (opts.block == 0 || opts.bytes == 0)
    {
        throw std::invalid_argument("bytes and block must be positive");
    }
    return opts;
}

void prepare_file(const std::filesystem::path& path, std::uint64_t bytes)
{
    const int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        throw std::runtime_error("open failed: " + std::string(std::strerror(errno)));
    }

    const int fallocate_res = ::posix_fallocate(fd, 0, static_cast<off_t>(bytes));
    if (fallocate_res != 0)
    {
        ::close(fd);
        throw std::runtime_error("posix_fallocate failed: " + std::string(std::strerror(fallocate_res)));
    }

    if (::close(fd) != 0)
    {
        throw std::runtime_error("close failed: " + std::string(std::strerror(errno)));
    }
}

struct BenchResult
{
    std::string name;
    double seconds{};
    double mib_per_sec{};
};

void print_result(const BenchResult& r)
{
    std::cout << "  " << r.name;
    for (std::size_t i = r.name.size(); i < 16; ++i)
    {
        std::cout << ' ';
    }
    std::cout << r.seconds << "s  " << r.mib_per_sec << " MiB/s\n";
}

tinyio::Result<void> tinyio_fiber_write_all(tinyio::IO& fio, tinyio::Fd& fd, std::span<const std::byte> block,
                                         std::uint64_t bytes)
{
    std::uint64_t offset = 0;
    while (offset < bytes)
    {
        const auto len = std::min<std::uint64_t>(block.size(), bytes - offset);
        iovec iov{.iov_base = const_cast<std::byte*>(block.data()), .iov_len = static_cast<std::size_t>(len)};
        FIBER_TRY(auto written, fio.writev(fd, std::span<const iovec>{&iov, 1}, static_cast<off_t>(offset)));
        if (written != static_cast<int32_t>(len))
        {
            return tinyio::error_from_errc(std::errc::io_error);
        }
        offset += len;
    }
    return {};
}

BenchResult bench_tinyio_fiber(const Options& opts, const std::vector<std::byte>& block)
{
    const auto path = opts.path.string() + ".tinyio";
    prepare_file(path, opts.bytes);

    const int raw_fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (raw_fd < 0)
    {
        throw std::runtime_error("open tinyio file failed: " + std::string(std::strerror(errno)));
    }

    tinyio::Fd fd{raw_fd};
    tinyio::IoOptions io_opts;
    tinyio::IO io(0, io_opts);
    
    // We want to run the write inside a fiber, and wait for it to finish
    bool finished = false;
    std::optional<tinyio::Result<void>> result;
    
    const auto start = Clock::now();
    io.spawn([&](tinyio::IO& fio) {
        result = tinyio_fiber_write_all(fio, fd, block, opts.bytes);
        finished = true;
    });
    
    while (!finished) {
        io.tick();
    }
    const auto end = Clock::now();
    
    if (!result.value())
    {
        throw std::runtime_error("tinyio write failed: " + result.value().error().message());
    }

    const double seconds = std::chrono::duration<double>(end - start).count();
    return {"tinyio", seconds, static_cast<double>(opts.bytes) / (1024.0 * 1024.0) / seconds};
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        const auto opts = parse_args(argc, argv);
        std::vector<std::byte> block(opts.block, std::byte{0x42});
        
        tinyio::ALOG::set_level(tinyio::ALOG::Level::Disabled);

        std::cout << "Benchmarking disk write:\n"
                  << "  Bytes: " << opts.bytes << '\n'
                  << "  Block: " << opts.block << '\n';

        print_result(bench_tinyio_fiber(opts, block));
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
