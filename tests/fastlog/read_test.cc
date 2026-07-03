#include <array>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "fastlog/fastlog.hpp"
#include "tinyio/runtime.hpp"

using namespace fastlog;

#include "partition_test_fixture.hpp"

using namespace fastlog;

TEST_F(PartitionTest, ReadFromRam) {
  const auto payload = AsBytes("hello_read_test");
  const auto append_res = partition_->Append(payload);
  ASSERT_TRUE(append_res.has_value());
  const LogAddress addr = *append_res;

  std::vector<std::byte> read_buf(payload.size());
  auto read_res = partition_->Read(addr, read_buf);
  ASSERT_TRUE(read_res.has_value());

  EXPECT_EQ(std::memcmp(read_buf.data(), payload.data(), payload.size()), 0);
}

TEST_F(PartitionTest, ReadOutOfBounds) {
  const auto payload = AsBytes("hello");
  const auto append_res = partition_->Append(payload);
  ASSERT_TRUE(append_res.has_value());

  std::vector<std::byte> read_buf(payload.size());

  // Try to read before begin or after tail
  auto read_res = partition_->Read(9999999, read_buf);
  EXPECT_FALSE(read_res.has_value());
  EXPECT_EQ(read_res.error().ErrorCode().value(), ENOENT);
}

TEST_F(PartitionTest, ReadFromDisk) {
  const auto payload1 = MakePayload(2000, std::byte{0x11});
  const auto payload2 = MakePayload(2000, std::byte{0x22});
  const auto payload3 = MakePayload(2000, std::byte{0x33});

  auto spawn_res = io_->Spawn([&](tinyio::IO&) noexcept {
    // Append records until we cross a page boundary
    const auto addr1 = *partition_->Append(payload1);
    *partition_->Append(payload2);
    *partition_->Append(payload3);  // This will spill to page 1

    // Commit to flush page 0 to disk
    auto commit_res = partition_->Commit(true);
    EXPECT_TRUE(commit_res.has_value()) << "Commit failed";
    if (!commit_res.has_value()) return;

    // Manually advance the head past page 0 to force eviction of page 0
    GetAddrMgr().AdvanceHead(4096);

    // Now Read addr1 should fetch from disk!
    std::vector<std::byte> read_buf(payload1.size());
    auto read_res = partition_->Read(addr1, read_buf);
    EXPECT_TRUE(read_res.has_value())
        << "Read from disk failed: " << read_res.error().Message();
    if (!read_res.has_value()) return;

    EXPECT_EQ(std::memcmp(read_buf.data(), payload1.data(), payload1.size()), 0);
  });

  ASSERT_TRUE(spawn_res.has_value());
  io_->RunUntilDone();
}
