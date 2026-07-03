#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "fastlog/fastlog.hpp"
#include "partition_test_fixture.hpp"
#include "tinyio/runtime.hpp"

using namespace fastlog;

TEST_F(PartitionTest, BasicCommitDecreasesUnflushed) {
  const auto payload1 = MakePayload(2000, std::byte{0x11});
  const auto payload2 = MakePayload(2000, std::byte{0x22});
  const auto payload3 = MakePayload(2000, std::byte{0x33});

  // Initially 0 bytes unflushed
  EXPECT_EQ(GetAddrMgr().UnflushedBytes(), 0);

  // Append records, unflushed bytes should increase
  partition_->Append(payload1);
  partition_->Append(payload2);
  partition_->Append(payload3);  // Spills over 4096 bytes

  EXPECT_GT(GetAddrMgr().UnflushedBytes(), 4096);

  const auto spawn_res = io_->Spawn([&](tinyio::IO&) noexcept {
    const auto commit_res = partition_->Commit(true);
    EXPECT_TRUE(commit_res.has_value());
  });
  ASSERT_TRUE(spawn_res.has_value());
  io_->RunUntilDone();

  // Commit rounds down to the nearest page boundary. So 4096 bytes should be flushed.
  // We started at 0, so committed should be 4096.
  // The unflushed bytes should be total_appended - 4096.
  EXPECT_EQ(GetAddrMgr().GetCommitted(), 4096);

  // The 3rd record starts at 4096 and takes 2016 bytes (aligned)
  EXPECT_EQ(GetAddrMgr().GetTail(), 4096 + 2016);
  EXPECT_EQ(GetAddrMgr().UnflushedBytes(), 2016);
}

TEST_F(PartitionTest, SegmentRotation) {
  const auto payload = MakePayload(4064, std::byte{0x44});
  constexpr size_t records_to_write = (k_segment_size / 4096) + 10;

  const auto spawn_res = io_->Spawn([&](tinyio::IO&) noexcept {
    for (size_t i = 0; i < records_to_write; ++i) {
      auto append_res = partition_->Append(payload);
      EXPECT_TRUE(append_res.has_value()) << "Append failed at iteration " << i;

      // Commit incrementally to free up memory capacity and write to disk
      auto commit_res = partition_->Commit(true);
      EXPECT_TRUE(commit_res.has_value());

      // fastlog doesn't currently auto-evict, so we must manually advance head
      GetAddrMgr().AdvanceHead(GetAddrMgr().GetCommitted());
    }
  });
  ASSERT_TRUE(spawn_res.has_value());
  io_->RunUntilDone();

  EXPECT_GT(GetAddrMgr().GetTail(), k_segment_size);

  const auto seg0 = settings_.dir / "00000000.log";
  const auto seg1 = settings_.dir / "00000001.log";

  EXPECT_TRUE(std::filesystem::exists(seg0));
  EXPECT_EQ(std::filesystem::file_size(seg0), k_segment_size);

  EXPECT_TRUE(std::filesystem::exists(seg1));
  EXPECT_GT(std::filesystem::file_size(seg1), 0);
}
