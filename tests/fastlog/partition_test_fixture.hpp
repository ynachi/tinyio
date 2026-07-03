#pragma once

#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "fastlog/fastlog.hpp"
#include "tinyio/runtime.hpp"

namespace fastlog {

// ── Helpers ──

inline std::span<const std::byte> AsBytes(const std::string_view sv) {
  return {reinterpret_cast<const std::byte*>(sv.data()), sv.size()};
}

inline std::vector<std::byte> MakePayload(const std::size_t size,
                                          const std::byte fill = std::byte{0xAB}) {
  return std::vector(size, fill);
}

// ── Fixture ──

class PartitionTest : public ::testing::Test {
 protected:
  // Page: 4KB, Memory: 64KB (16 pages), Segment: 1MB
  static constexpr uint k_page_bits = 12;
  static constexpr uint k_memory_bits = 16;
  static constexpr uint k_segment_bits = 20;
  static constexpr size_t k_page_size = 1ULL << k_page_bits;
  static constexpr size_t k_memory_size = 1ULL << k_memory_bits;
  static constexpr size_t k_segment_size = 1ULL << k_segment_bits;
  static constexpr size_t k_resident_pages = 1ULL << (k_memory_bits - k_page_bits);

  tinyio::IoOptions io_opts_;
  std::unique_ptr<tinyio::IO> io_;
  FastLogSettings settings_;
  std::unique_ptr<Partition> partition_;

  AddressManager& GetAddrMgr() const { return partition_->am_; }
  PageArena& GetArena() const { return partition_->arena_; }

  void SetUp() override {
    // Recreate io_ for each test to ensure a clean state.
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__) || \
    (defined(__has_feature) && (__has_feature(thread_sanitizer) || __has_feature(address_sanitizer)))
    // Set a 2MB fiber stack to prevent overflows under TSAN/ASAN with GTest macros.
    io_opts_.stack_size = 2 * 1024 * 1024;
#endif
    io_ = std::make_unique<tinyio::IO>(
        0, io_opts_,
        std::vector<tinyio::BucketConfig>{{.size = k_memory_size, .count = 1}});

    settings_.dir = std::filesystem::temp_directory_path() / "fastlog_partition_test";
    std::filesystem::remove_all(settings_.dir);
    std::filesystem::create_directories(settings_.dir);

    settings_.page_bits = k_page_bits;
    settings_.memory_bits = k_memory_bits;
    settings_.segment_bits = k_segment_bits;
    settings_.mutable_frac = 0.5;
    settings_.Validate();

    partition_ = std::make_unique<Partition>(settings_, *io_);
  }

  void TearDown() override {
    if (partition_) {
      partition_.reset();
    }
    if (io_) {
      io_->RunUntilDone();
      io_.reset();
    }
    std::filesystem::remove_all(settings_.dir);
  }

  // Read the raw record bytes at a logical address (useful for checking RAM writes)
  const std::byte* RecordPtr(const LogAddress addr) const {
    const auto& am = GetAddrMgr();
    const auto slot = am.SlotOf(addr);
    const auto offset = am.OffsetInPageOf(addr);
    const size_t start_offset = slot * k_page_size + offset;
    return GetArena().Buffer().Ptr() + start_offset;
  }

  const RecordHeader* HeaderAt(const LogAddress addr) const {
    return reinterpret_cast<const RecordHeader*>(RecordPtr(addr));
  }

  std::span<const std::byte> PayloadAt(const LogAddress addr, const size_t len) const {
    return {RecordPtr(addr) + k_header_size, len};
  }

  uint32_t ComputeExpectedCrc(const LogAddress addr, const uint32_t payload_size) const {
    const auto* ptr = RecordPtr(addr);
    const auto* crc_start = reinterpret_cast<const char*>(ptr + sizeof(uint32_t));
    const std::size_t crc_length = k_header_size - sizeof(uint32_t) + payload_size;
    return static_cast<uint32_t>(
        absl::ComputeCrc32c(absl::string_view(crc_start, crc_length)));
  }
};

}  // namespace fastlog
