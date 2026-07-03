#include <array>
#include <cstring>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "fastlog/fastlog.hpp"
#include "tinyio/runtime.hpp"

using namespace fastlog;

#include "partition_test_fixture.hpp"

using namespace fastlog;

TEST_F(PartitionTest, ConcurrentReads) {
  // We will append a bunch of records to disk, then spawn multiple fibers
  // to read them back concurrently.

  struct RecordInfo {
    LogAddress addr;
    std::byte fill_byte;
    size_t size;
  };

  std::vector<RecordInfo> records;

  const auto spawn_res = io_->Spawn([&](tinyio::IO&) noexcept {
    for (size_t i = 0; i < 20; ++i) {
      const auto fill = static_cast<std::byte>(i % 256);
      auto payload = MakePayload(2000, fill);
      auto append_res = partition_->Append(payload);
      EXPECT_TRUE(append_res.has_value());

      records.push_back({.addr = *append_res, .fill_byte = fill, .size = 2000});

      auto commit_res = partition_->Commit(true);
      EXPECT_TRUE(commit_res.has_value());
      GetAddrMgr().AdvanceHead(GetAddrMgr().GetCommitted());
    }
  });
  ASSERT_TRUE(spawn_res.has_value());
  io_->RunUntilDone();

  // All 20 records are now on disk. Now we spawn multiple fibers to read them!
  std::atomic success_count{0};

  // Use a fiber to spawn the other fibers, because TaskGroup must be used inside a
  // fiber
  const auto test_spawn_res = io_->Spawn([&](tinyio::IO& io) noexcept {
    tinyio::TaskGroup<bool> group(io, records.size());

    for (const auto& rec : records) {
      group.Spawn([&, rec](tinyio::IO&) noexcept -> tinyio::Result<bool> {
        std::vector<std::byte> read_buf(rec.size);
        if (auto read_res = partition_->Read(rec.addr, read_buf); !read_res.has_value()) {
          return std::unexpected(read_res.error());
        }

        // Check integrity
        for (const auto b : read_buf) {
          if (b != rec.fill_byte) {
            return tinyio::Fail(EINVAL, "Corrupted read");
          }
        }
        ++success_count;
        return true;
      });
    }

    const auto wait_res = group.Wait();
    EXPECT_TRUE(wait_res.has_value());
  });

  ASSERT_TRUE(test_spawn_res.has_value());
  io_->RunUntilDone();

  EXPECT_EQ(success_count.load(), records.size());
}

TEST_F(PartitionTest, InterleavedReadWrite) {
  // First, write some records so we have something to read from disk
  const auto initial_payload = MakePayload(1000, std::byte{0xAA});
  LogAddress read_target = 0;

  const auto init_spawn = io_->Spawn([&](tinyio::IO&) noexcept {
    read_target = *partition_->Append(initial_payload);
    const auto commit_res = partition_->Commit(true);
    EXPECT_TRUE(commit_res.has_value());
    GetAddrMgr().AdvanceHead(GetAddrMgr().GetCommitted());  // Force it to disk
  });
  ASSERT_TRUE(init_spawn.has_value());
  io_->RunUntilDone();

  std::atomic read_success{0};
  std::atomic write_success{0};

  const auto test_spawn_res = io_->Spawn([&](tinyio::IO& io) noexcept {
    tinyio::TaskGroup<bool> group(io, 20);

    // Spawn 10 readers
    for (int i = 0; i < 10; ++i) {
      group.Spawn([&](tinyio::IO&) noexcept -> tinyio::Result<bool> {
        std::vector<std::byte> read_buf(1000);
        auto read_res = partition_->Read(read_target, read_buf);
        if (read_res.has_value()) {
          ++read_success;
          return true;
        }
        return false;
      });
    }

    // Spawn 10 writers
    for (int i = 0; i < 10; ++i) {
      group.Spawn([&, i](tinyio::IO&) noexcept -> tinyio::Result<bool> {
        auto payload = MakePayload(1000, static_cast<std::byte>(i));
        auto append_res = partition_->Append(payload);
        if (append_res.has_value()) {
          ++write_success;
          return true;
        }
        return false;
      });
    }

    const auto wait_res = group.Wait();
    EXPECT_TRUE(wait_res.has_value());
  });

  ASSERT_TRUE(test_spawn_res.has_value());
  io_->RunUntilDone();

  EXPECT_EQ(read_success.load(), 10);
  EXPECT_EQ(write_success.load(), 10);
}
