// tinyio random-read disk benchmark -- deliberately mirrors yalantinglibs'
// coro_file_bench (src/coro_io/benchmark/coro_file_bench.cpp) so the three
// runtimes are directly comparable.
//
// Same methodology:
//   * 4 KiB blocks, O_DIRECT, full content verification
//   * scenarios (file_count, depth) = (1,1) (1,32) (1,128) (32,32) (128,128)
//   * depth = number of concurrent fibers => queue depth
//   * block index pattern: (iteration * 977 + worker_id * 131) % blocks
//   * IOPS / mean / p50 / p99 are medians over `--repeats` runs
//   * warmup pass before each measured pass
//
// The content pattern is copied byte-for-byte from coro_io's
// file_io_fixture.hpp so every runtime reads literally the same bytes.
//
// The data file is created with mkstemp() in the CURRENT WORKING DIRECTORY --
// run this from a directory on the device you want to measure (the remote's
// /tmp is tmpfs, i.e. RAM).

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <sys/resource.h>
#include <tinyio/error.hpp>
#include <tinyio/fd.hpp>
#include <tinyio/fiber_sync.hpp>
#include <tinyio/runtime.hpp>

namespace {
using Clock = std::chrono::steady_clock;

constexpr std::size_t kBlockSize = 4096;

// ---- identical to coro_io's file_io_test::pattern -------------------------
std::uint64_t pattern(std::uint64_t block, std::size_t word) {
  return (block + 1) * 0x9e3779b97f4a7c15ULL ^ (word + 1) * 0xd1b54a32d192ed03ULL;
}

class AlignedBuffer {
 public:
  explicit AlignedBuffer(std::size_t size = kBlockSize) : size_(size) {
    if (::posix_memalign(&data_, kBlockSize, size) != 0) {
      throw std::bad_alloc();
    }
    std::memset(data_, 0, size);
  }
  ~AlignedBuffer() { std::free(data_); }
  AlignedBuffer(const AlignedBuffer&) = delete;
  AlignedBuffer& operator=(const AlignedBuffer&) = delete;
  AlignedBuffer(AlignedBuffer&& other) noexcept : data_(other.data_), size_(other.size_) {
    other.data_ = nullptr;
    other.size_ = 0;
  }
  AlignedBuffer& operator=(AlignedBuffer&& other) noexcept {
    if (this != &other) {
      std::free(data_);
      data_ = other.data_;
      size_ = other.size_;
      other.data_ = nullptr;
      other.size_ = 0;
    }
    return *this;
  }
  char* data() { return static_cast<char*>(data_); }
  std::size_t size() const { return size_; }

 private:
  void* data_ = nullptr;
  std::size_t size_;
};

void fill(char* buffer, std::uint64_t first_block, std::size_t size) {
  for (std::size_t offset = 0; offset < size; offset += sizeof(std::uint64_t)) {
    auto value = pattern(first_block + offset / kBlockSize,
                         (offset % kBlockSize) / sizeof(std::uint64_t));
    std::memcpy(buffer + offset, &value, sizeof(value));
  }
}

bool verify(const char* buffer, std::uint64_t first_block, std::size_t size) {
  for (std::size_t offset = 0; offset < size; offset += sizeof(std::uint64_t)) {
    std::uint64_t value;
    std::memcpy(&value, buffer + offset, sizeof(value));
    if (value != pattern(first_block + offset / kBlockSize,
                         (offset % kBlockSize) / sizeof(std::uint64_t))) {
      return false;
    }
  }
  return true;
}

// ---- data file -----------------------------------------------------------
class DataFile {
 public:
  explicit DataFile(std::size_t blocks, const std::string& existing = {})
      : blocks_(blocks), external_(!existing.empty()) {
    if (external_) {
      path_ = existing;
      fd_ = ::open(path_.c_str(), O_RDONLY | O_DIRECT | O_CLOEXEC);
      if (fd_ < 0) {
        throw std::system_error(errno, std::system_category(), "open existing O_DIRECT");
      }
      return;
    }
    char name[] = "tinyio-disk-data-XXXXXX";
    fd_ = ::mkstemp(name);
    if (fd_ < 0) {
      throw std::system_error(errno, std::system_category(), "mkstemp");
    }
    path_ = name;
    try {
      AlignedBuffer buffer(256 * kBlockSize);
      for (std::size_t block = 0; block < blocks;) {
        std::size_t count = std::min<std::size_t>(256, blocks - block);
        std::size_t length = count * kBlockSize;
        fill(buffer.data(), block, length);
        std::size_t done = 0;
        while (done < length) {
          ssize_t result = ::write(fd_, buffer.data() + done, length - done);
          if (result < 0 && errno == EINTR) continue;
          if (result <= 0) {
            throw std::system_error(result < 0 ? errno : EIO, std::system_category(),
                                    "write");
          }
          done += static_cast<std::size_t>(result);
        }
        block += count;
      }
      if (::fsync(fd_) < 0) {
        throw std::system_error(errno, std::system_category(), "fsync");
      }
    } catch (...) {
      ::close(fd_);
      ::unlink(path_.c_str());
      throw;
    }
  }
  ~DataFile() {
    ::close(fd_);
    if (!external_) {
      ::unlink(path_.c_str());
    }
  }
  DataFile(const DataFile&) = delete;
  DataFile& operator=(const DataFile&) = delete;
  const std::string& path() const { return path_; }
  std::size_t blocks() const { return blocks_; }

 private:
  std::string path_;
  int fd_ = -1;
  std::size_t blocks_;
  bool external_ = false;
};

// ---- benchmark -----------------------------------------------------------
struct Config {
  std::size_t operations = 20000;
  std::size_t repeats = 3;
  std::size_t file_mib = 64;
  // 0 = keep tinyio's default (SINGLE_ISSUER|DEFER_TASKRUN|TASKRUN_FLAG)
  unsigned flags = 0;
  // When set, read this existing file instead of creating one. Removes the
  // "where did the file land on the NVMe" variable from the comparison.
  std::string file_path;
  // -1 = all scenarios; 0..4 selects one of the five (QD1/32/128, indep 32/128).
  int scenario = -1;
  // 0 = tinyio's submit-first loop, 1 = kio's resume-first pipeline.
  int loop_style = 0;
};

struct Worker {
  AlignedBuffer buffer;
  std::vector<double> latencies;
  std::size_t errors = 0;
  std::size_t short_reads = 0;
  std::size_t corrupt = 0;
  void reset() {
    latencies.clear();
    errors = short_reads = corrupt = 0;
  }
};

struct Measurement {
  double iops;
  double mean;
  double p50;
  double p99;
  std::size_t errors;
  std::size_t short_reads;
  std::size_t corrupt;
};

void run_phase(const std::vector<tinyio::Fd>& files, std::vector<Worker>& workers,
               std::size_t total_ops, std::size_t blocks, unsigned flags = 0,
               int loop_style = 0) {
  const std::size_t depth = workers.size();
  // A tinyio::IO owns its ring and is single-use: one IO per phase.
  tinyio::IoOptions opts;
  if (flags != 0) {
    opts.additional_uring_flags = flags;
  }
  if (loop_style == 1) {
    opts.loop_style = tinyio::IoOptions::LoopStyle::kResumeFirst;
  }
  tinyio::IO io(0, opts, {});
  (void)io.Spawn([&](tinyio::IO& fio) noexcept {
    tinyio::TaskGroup<void> group(fio);
    for (std::size_t index = 0; index < depth; ++index) {
      std::size_t count = total_ops / depth + (index < total_ops % depth ? 1 : 0);
      const std::size_t file_index = index % files.size();
      // NOTE: `index` MUST be captured by value. The fiber body runs long
      // after the loop iteration that created it has ended, so a by-reference
      // capture would dangle and the fiber would read/write a bogus Worker.
      Worker* const worker = &workers[index];
      (void)group.Spawn([&, worker, index, file_index, count](tinyio::IO& io2) noexcept {
        for (std::size_t iteration = 0; iteration < count; ++iteration) {
          std::size_t block = (iteration * 977 + index * 131) % blocks;
          auto start = Clock::now();
          auto result = io2.Read(
              files[file_index],
              std::span{reinterpret_cast<std::byte*>(worker->buffer.data()), kBlockSize},
              static_cast<off_t>(block * kBlockSize));
          worker->latencies.push_back(
              std::chrono::duration<double, std::micro>(Clock::now() - start).count());
          if (!result.has_value()) {
            ++worker->errors;
            return;
          }
          if (*result != kBlockSize) {
            ++worker->short_reads;
            return;
          }
          if (!verify(worker->buffer.data(), block, kBlockSize)) {
            ++worker->corrupt;
          }
        }
      });
    }
    (void)group.Wait();
  });
  io.RunUntilDone();
}

Measurement measure(const Config& config, const DataFile& data, std::size_t file_count,
                    std::size_t depth) {
  std::vector<tinyio::Fd> files;
  files.reserve(file_count);
  for (std::size_t index = 0; index < file_count; ++index) {
    int fd = ::open(data.path().c_str(), O_RDONLY | O_DIRECT | O_CLOEXEC);
    if (fd < 0) {
      throw std::system_error(errno, std::system_category(), "open O_DIRECT");
    }
    files.emplace_back(fd);
  }

  std::vector<Worker> workers;
  workers.reserve(depth);
  for (std::size_t index = 0; index < depth; ++index) {
    workers.emplace_back();
  }

  tinyio::IoOptions opts;
  (void)opts;

  // warmup
  run_phase(files, workers, depth * 8, data.blocks(), config.flags, config.loop_style);
  for (auto& worker : workers) {
    if (worker.errors || worker.short_reads || worker.corrupt) {
      throw std::runtime_error("warmup verification failed");
    }
    worker.reset();
  }

  // measured
  struct rusage ru0{};
  getrusage(RUSAGE_SELF, &ru0);
  auto start = Clock::now();
  run_phase(files, workers, config.operations, data.blocks(), config.flags,
            config.loop_style);
  struct rusage ru1{};
  getrusage(RUSAGE_SELF, &ru1);
  std::printf("RUSAGE qd=%zu user=%.3f sys=%.3f vctx=%ld ivctx=%ld\n", depth,
              static_cast<double>(ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec) +
                  1e-6 * static_cast<double>(ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec),
              static_cast<double>(ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec) +
                  1e-6 * static_cast<double>(ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec),
              (long)(ru1.ru_nvcsw - ru0.ru_nvcsw), (long)(ru1.ru_nivcsw - ru0.ru_nivcsw));
  std::fflush(stdout);

  double seconds = std::chrono::duration<double>(Clock::now() - start).count();

  Measurement measurement{};
  std::vector<double> latencies;
  latencies.reserve(config.operations);
  for (const auto& worker : workers) {
    latencies.insert(latencies.end(), worker.latencies.begin(), worker.latencies.end());
    measurement.errors += worker.errors;
    measurement.short_reads += worker.short_reads;
    measurement.corrupt += worker.corrupt;
  }
  if (latencies.size() != config.operations) {
    throw std::runtime_error("incorrect measurement count: " +
                             std::to_string(latencies.size()));
  }
  std::sort(latencies.begin(), latencies.end());
  measurement.iops = static_cast<double>(config.operations) / seconds;
  measurement.mean = std::accumulate(latencies.begin(), latencies.end(), 0.0) /
                     static_cast<double>(latencies.size());
  measurement.p50 = latencies[(latencies.size() - 1) / 2];
  measurement.p99 = latencies[static_cast<std::size_t>(
      0.99 * static_cast<double>(latencies.size() - 1))];
  return measurement;
}

double median(const std::vector<Measurement>& samples, double Measurement::* field) {
  std::vector<double> values;
  for (const auto& sample : samples) {
    values.push_back(sample.*field);
  }
  std::sort(values.begin(), values.end());
  return (values[(values.size() - 1) / 2] + values[values.size() / 2]) / 2;
}

bool print_summary(const char* scenario, const char* backend, std::size_t depth,
                   const std::vector<Measurement>& samples) {
  std::size_t errors = 0, short_reads = 0, corrupt = 0;
  for (const auto& sample : samples) {
    errors += sample.errors;
    short_reads += sample.short_reads;
    corrupt += sample.corrupt;
  }
  std::printf("%-12s %-12s %4zu %12.0f %12.2f %12.2f %12.2f %8zu %8zu %8zu\n", scenario,
              backend, depth, median(samples, &Measurement::iops),
              median(samples, &Measurement::mean), median(samples, &Measurement::p50),
              median(samples, &Measurement::p99), errors, short_reads, corrupt);
  std::fflush(stdout);
  return errors == 0 && short_reads == 0 && corrupt == 0;
}

std::size_t number(std::string_view text) {
  std::size_t value = 0;
  auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
    throw std::invalid_argument("expected a positive integer");
  }
  return value;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Config config;
    for (int index = 1; index < argc; ++index) {
      std::string_view option(argv[index]);
      if (option == "--help") {
        std::puts(
            "tinyio_disk_read_bench [--operations N] [--repeats N] [--file-mib N]\n"
            "Mirrors coro_io's coro_file_bench: 4 KiB O_DIRECT, QD 1/32/128.\n"
            "The data file is created in the current working directory.");
        return 0;
      }
      if (++index == argc) {
        throw std::invalid_argument("missing option value");
      }
      if (option == "--operations") {
        config.operations = number(argv[index]);
      } else if (option == "--repeats") {
        config.repeats = number(argv[index]);
      } else if (option == "--file-mib") {
        config.file_mib = number(argv[index]);
      } else if (option == "--uring-flags") {
        config.flags = static_cast<unsigned>(number(argv[index]));
      } else if (option == "--file") {
        config.file_path = argv[index];
      } else if (option == "--scenario") {
        config.scenario = static_cast<int>(number(argv[index]));
      } else if (option == "--loop") {
        config.loop_style = static_cast<int>(number(argv[index]));
      } else {
        throw std::invalid_argument("unknown option");
      }
    }
    if (config.operations < 128 || config.operations > 2000000 || !config.repeats ||
        config.repeats > 20 || !config.file_mib || config.file_mib > 1024) {
      throw std::invalid_argument(
          "operations: 128..2000000, repeats: 1..20, file-mib: 1..1024");
    }

    DataFile data(config.file_mib * 1024 * 1024 / kBlockSize, config.file_path);
    tinyio::log::SetLevel(tinyio::log::Level::Disabled);
    std::printf(
        "tinyio disk bench, operations/run=%zu, repeats=%zu, file=%zu MiB, cwd=%s, "
        "uring_flags=%u\n",
        config.operations, config.repeats, config.file_mib,
        std::filesystem::current_path().c_str(),
        config.flags != 0 ? config.flags : tinyio::k_uring_default_additional_flag);
    std::puts("IOPS/latency: per-run medians; errors/short/corrupt: totals across runs.");
    std::printf("%-12s %-12s %4s %12s %12s %12s %12s %8s %8s %8s\n", "Scenario",
                "Backend", "QD", "IOPS", "Mean(us)", "P50(us)", "P99(us)", "Errors",
                "Short", "Corrupt");

    bool passed = true;
    std::size_t scenario_index = 0;
    for (auto [file_count, depth] : std::array<std::pair<std::size_t, std::size_t>, 5>{
             {{1, 1}, {1, 32}, {1, 128}, {32, 32}, {128, 128}}}) {
      if (config.scenario >= 0 && static_cast<int>(scenario_index) != config.scenario) {
        ++scenario_index;
        continue;
      }
      ++scenario_index;
      std::vector<Measurement> samples;
      for (std::size_t repeat = 0; repeat < config.repeats; ++repeat) {
        samples.push_back(measure(config, data, file_count, depth));
      }
      const char* scenario = file_count == 1 ? "shared" : "independent";
      char backend[24];
      if (config.flags != 0) {
        std::snprintf(backend, sizeof(backend), "tinyio-f%u", config.flags);
      } else {
        std::snprintf(backend, sizeof(backend), "tinyio");
      }
      passed &= print_summary(scenario, backend, depth, samples);
    }
    return passed ? 0 : 1;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
