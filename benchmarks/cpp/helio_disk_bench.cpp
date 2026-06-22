
#include <chrono>
#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>

#include "io/file.h"
#include "base/init.h"
#include "util/fibers/pool.h"

using Clock = std::chrono::steady_clock;

const std::string FLAGS_path = "/tmp/helio-disk-bench.dat";
const std::uint64_t FLAGS_bytes = 1024ull * 1024ull * 1024ull;
const std::uint64_t FLAGS_block = 64ull * 1024ull;

void prepare_file(const std::filesystem::path& path, std::uint64_t bytes)
{
    const int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    if (fd < 0) {
        throw std::runtime_error("open failed");
    }

    if (::posix_fallocate(fd, 0, static_cast<off_t>(bytes)) != 0) {
        ::close(fd);
        throw std::runtime_error("posix_fallocate failed");
    }
    ::close(fd);
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

int helio_write_all(io::WriteFile* file, const uint8_t* block, std::uint64_t block_size, std::uint64_t bytes)
{
    std::uint64_t offset = 0;
    while (offset < bytes)
    {
        const auto len = std::min<std::uint64_t>(block_size, bytes - offset);
        auto ec = file->Write(block, len);
        if (ec) return -1;
        offset += len;
    }
    return 0;
}

BenchResult bench_helio(const uint8_t* block, std::uint64_t block_size)
{
    const auto path = FLAGS_path + ".helio";
    prepare_file(path, FLAGS_bytes);

    io::WriteFile::Options opts;
    opts.direct = true;
    auto res = io::OpenWrite(path, opts);
    if (!res) {
        throw std::runtime_error("OpenWrite failed");
    }
    io::WriteFile* file = res.value();

    const auto start = Clock::now();
    
    if (helio_write_all(file, block, block_size, FLAGS_bytes) != 0) {
        throw std::runtime_error("helio write failed");
    }
    file->Close();

    const auto end = Clock::now();

    const double seconds = std::chrono::duration<double>(end - start).count();
    delete file;
    return {"helio", seconds, static_cast<double>(FLAGS_bytes) / (1024.0 * 1024.0) / seconds};
}

int main(int argc, char** argv)
{
    MainInitGuard guard(&argc, &argv);
    void* ptr = nullptr;
    try
    {
        if (::posix_memalign(&ptr, 4096, FLAGS_block) != 0) {
            throw std::runtime_error("posix_memalign failed");
        }
        std::memset(ptr, 0x42, FLAGS_block);
        const uint8_t* block = static_cast<const uint8_t*>(ptr);

        std::unique_ptr<util::ProactorPool> pp;
        pp.reset(util::fb2::Pool::IOUring(256, 1));
        pp->Run();
        
        std::cout << "Benchmarking disk write (O_DIRECT):\n"
                  << "  Bytes: " << FLAGS_bytes << '\n'
                  << "  Block: " << FLAGS_block << '\n';

        pp->Await([&](auto*) {
            print_result(bench_helio(block, FLAGS_block));
        });

        pp->Stop();
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
