#include <photon/photon.h>
#include <photon/thread/thread.h>
#include <photon/fs/localfs.h>
#include <photon/common/alog.h>

#include <chrono>
#include <iostream>
#include <vector>
#include <span>
#include <string>
#include <stdexcept>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;

struct Options
{
    std::filesystem::path path = "/tmp/photon-disk-bench.dat";
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

int photon_write_all(photon::fs::IFile* file, std::span<const std::byte> block, std::uint64_t bytes)
{
    std::uint64_t offset = 0;
    while (offset < bytes)
    {
        const auto len = std::min<std::uint64_t>(block.size(), bytes - offset);
        ssize_t written = file->pwrite(block.data(), len, offset);
        if (written != static_cast<ssize_t>(len))
        {
            return -1;
        }
        offset += len;
    }
    return 0;
}

BenchResult bench_photon(const Options& opts, std::span<const std::byte> block)
{
    const auto path = opts.path.string() + ".photon";
    prepare_file(path, opts.bytes);

    photon::init(photon::INIT_EVENT_IOURING, photon::INIT_IO_LIBAIO);
    DEFER(photon::fini());

    photon::fs::IFileSystem* fs = photon::fs::new_localfs_adaptor();
    if (!fs) {
        throw std::runtime_error("failed to create localfs adaptor");
    }
    DEFER(delete fs);

    photon::fs::IFile* file = fs->open(path.c_str(), O_WRONLY | O_CLOEXEC | O_DIRECT, 0644);
    if (!file) {
        throw std::runtime_error("open photon file failed");
    }
    DEFER(delete file);

    const auto start = Clock::now();
    
    if (photon_write_all(file, block, opts.bytes) != 0) {
        throw std::runtime_error("photon write failed");
    }

    const auto end = Clock::now();

    const double seconds = std::chrono::duration<double>(end - start).count();
    return {"photon", seconds, static_cast<double>(opts.bytes) / (1024.0 * 1024.0) / seconds};
}
} // namespace

int main(int argc, char** argv)
{
    void* ptr = nullptr;
    try
    {
        const auto opts = parse_args(argc, argv);
        
        if (::posix_memalign(&ptr, 4096, opts.block) != 0) {
            throw std::runtime_error("posix_memalign failed");
        }
        std::memset(ptr, 0x42, opts.block);
        std::span<const std::byte> block(static_cast<const std::byte*>(ptr), opts.block);
        
        set_log_output_level(ALOG_FATAL);

        std::cout << "Benchmarking disk write (O_DIRECT):\n"
                  << "  Bytes: " << opts.bytes << '\n'
                  << "  Block: " << opts.block << '\n';

        print_result(bench_photon(opts, block));
        std::free(ptr);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << '\n';
        if (ptr) std::free(ptr);
        return 1;
    }
    return 0;
}
