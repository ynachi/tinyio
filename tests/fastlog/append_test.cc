#include <array>
#include <cstring>
#include <vector>

#include <absl/crc/crc32c.h>
#include <gtest/gtest.h>

#include "fastlog/fastlog.hpp"
#include "tinyio/runtime.hpp"

#include "partition_test_fixture.hpp"

using namespace fastlog;

// ---------------------------------------------------------------------------
// Record layout: header fields, payload content, CRC integrity
// ---------------------------------------------------------------------------

TEST_F(PartitionTest, RecordLayout) {
  const auto payload = AsBytes("hello, fastlog!");

  const auto result = partition_->Append(payload);
  ASSERT_TRUE(result.has_value());

  const LogAddress addr = *result;
  EXPECT_EQ(addr, 0) << "First append starts at address 0";

  // Header
  const auto* hdr = HeaderAt(addr);
  EXPECT_EQ(hdr->op, static_cast<uint8_t>(RecordOp::Upsert));
  EXPECT_EQ(hdr->flags, 0);
  EXPECT_EQ(hdr->payload_size, payload.size());

  // Payload
  const auto stored = PayloadAt(addr, payload.size());
  EXPECT_EQ(std::memcmp(stored.data(), payload.data(), payload.size()), 0);

  // CRC32c
  EXPECT_EQ(hdr->crc32c, ComputeExpectedCrc(addr, hdr->payload_size));

  // Tail advanced by AlignUpTo8(header + payload)
  const auto expected_size = AlignUpTo8(k_header_size + payload.size());
  EXPECT_EQ(GetAddrMgr().GetTail(), expected_size);
}

// ---------------------------------------------------------------------------
// Sequential appends: monotonic addresses, 8-byte alignment, independent data
// ---------------------------------------------------------------------------

TEST_F(PartitionTest, SequentialAppends) {
  const std::array<std::string, 4> payloads = {"alpha", "bravo-record", "charlie!"};
  LogAddress prev_addr = 0;

  for (size_t i = 0; i < payloads.size(); ++i) {
    auto result = partition_->Append(AsBytes(payloads[i]));
    ASSERT_TRUE(result.has_value()) << "Append #" << i;

    const LogAddress addr = *result;

    // 8-byte aligned
    EXPECT_EQ(addr % 8, 0) << "Address " << addr << " not 8-byte aligned";

    // Monotonically increasing
    if (i > 0) {
      EXPECT_GT(addr, prev_addr);
    }

    // Payload integrity
    auto stored = PayloadAt(addr, payloads[i].size());
    EXPECT_EQ(std::memcmp(stored.data(), payloads[i].data(), payloads[i].size()), 0)
        << "Payload mismatch at append #" << i;

    // Header correctness
    const auto* hdr = HeaderAt(addr);
    EXPECT_EQ(hdr->payload_size, payloads[i].size());
    EXPECT_EQ(hdr->crc32c, ComputeExpectedCrc(addr, hdr->payload_size));

    prev_addr = addr;
  }
}

// ---------------------------------------------------------------------------
// Empty payload: header-only record with valid CRC
// ---------------------------------------------------------------------------

TEST_F(PartitionTest, EmptyPayload) {
  constexpr std::span<const std::byte> empty;

  const auto result = partition_->Append(empty);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 0);

  const auto* hdr = HeaderAt(*result);
  EXPECT_EQ(hdr->op, static_cast<uint8_t>(RecordOp::Upsert));
  EXPECT_EQ(hdr->flags, 0);
  EXPECT_EQ(hdr->payload_size, 0);
  EXPECT_EQ(hdr->crc32c, ComputeExpectedCrc(*result, 0));

  // AlignUpTo8(16 + 0) = 16
  EXPECT_EQ(GetAddrMgr().GetTail(), k_header_size);
}

// ---------------------------------------------------------------------------
// Page boundary: record that doesn't fit pads to next page
// ---------------------------------------------------------------------------

TEST_F(PartitionTest, PageBoundaryPadding) {
  // Fill most of page 0: payload=4064 → record=AlignUpTo8(4080)=4080
  // Leaves 4096 - 4080 = 16 bytes remaining in page 0.
  auto big = MakePayload(4064);
  const auto r1 = partition_->Append(big);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(*r1, 0);
  EXPECT_EQ(GetAddrMgr().GetTail(), 4080);

  // Next record (even 1 byte) needs AlignUpTo8(17) = 24 > 16 remaining → pads to page 1
  auto small = MakePayload(1, std::byte{0xCD});
  const auto r2 = partition_->Append(small);
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(*r2, k_page_size) << "Should pad to start of next page";

  // Verify record on new page
  const auto* hdr = HeaderAt(*r2);
  EXPECT_EQ(hdr->payload_size, 1);
  const auto stored = PayloadAt(*r2, 1);
  EXPECT_EQ(stored[0], std::byte{0xCD});
  EXPECT_EQ(hdr->crc32c, ComputeExpectedCrc(*r2, 1));
}

// ---------------------------------------------------------------------------
// Record that exactly fits remaining page space: no padding
// ---------------------------------------------------------------------------

TEST_F(PartitionTest, ExactPageFit) {
  // First record: payload=4048 → record=AlignUpTo8(4064)=4064. Leaves 32 bytes.
  auto first = MakePayload(4048);
  const auto r1 = partition_->Append(first);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(GetAddrMgr().GetTail(), 4064);

  // Second record: payload=8 → record=AlignUpTo8(24)=24. Fits in remaining 32 bytes.
  auto second = MakePayload(8, std::byte{0xEE});
  const auto r2 = partition_->Append(second);
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(*r2, 4064) << "Should NOT pad — fits in remaining space";
  EXPECT_EQ(GetAddrMgr().GetTail(), 4088);

  // Verify second record
  const auto* hdr = HeaderAt(*r2);
  EXPECT_EQ(hdr->payload_size, 8);
  EXPECT_EQ(hdr->crc32c, ComputeExpectedCrc(*r2, 8));
}

// ---------------------------------------------------------------------------
// Capacity exhaustion: fill all pages, then verify rejection
// ---------------------------------------------------------------------------

TEST_F(PartitionTest, CapacityExhaustion) {
  // payload=4064 → record=4080. One record per page (16 bytes wasted).
  // 16 pages → exactly 16 records.
  auto payload = MakePayload(4064);

  size_t count = 0;
  while (partition_->Append(payload).has_value()) {
    count++;
  }
  EXPECT_EQ(count, k_resident_pages);

  // Even a small record is rejected (only 16 bytes free, need 24 minimum)
  auto tiny = MakePayload(1);
  EXPECT_FALSE(partition_->Append(tiny).has_value());
}

// Multiple pages: records spanning all slots with verified integrity
// ---------------------------------------------------------------------------

TEST_F(PartitionTest, MultiPageIntegrity) {
  // Write one record per page across all 16 pages, each with distinct content.
  std::vector<LogAddress> addrs;
  for (size_t i = 0; i < k_resident_pages; ++i) {
    auto payload = MakePayload(100, static_cast<std::byte>(i));
    auto result = partition_->Append(payload);
    ASSERT_TRUE(result.has_value()) << "Failed at page " << i;
    addrs.push_back(*result);
  }

  // Verify every record independently
  for (size_t i = 0; i < addrs.size(); ++i) {
    const auto* hdr = HeaderAt(addrs[i]);
    EXPECT_EQ(hdr->payload_size, 100) << "Page " << i;
    EXPECT_EQ(hdr->op, static_cast<uint8_t>(RecordOp::Upsert));
    EXPECT_EQ(hdr->crc32c, ComputeExpectedCrc(addrs[i], 100))
        << "CRC mismatch page " << i;

    auto stored = PayloadAt(addrs[i], 100);
    auto expected = MakePayload(100, static_cast<std::byte>(i));
    EXPECT_EQ(std::memcmp(stored.data(), expected.data(), 100), 0)
        << "Data mismatch page " << i;
  }
}

// ---------------------------------------------------------------------------
// Delete operation: tombstone record with valid layout
// ---------------------------------------------------------------------------

TEST_F(PartitionTest, DeleteRecord) {
  const auto key = AsBytes("test_key_to_delete");

  const auto result = partition_->Delete(key);
  ASSERT_TRUE(result.has_value());

  const LogAddress addr = *result;

  // Header
  const auto* hdr = HeaderAt(addr);
  EXPECT_EQ(hdr->op, static_cast<uint8_t>(RecordOp::Delete));
  EXPECT_EQ(hdr->flags, 0);
  EXPECT_EQ(hdr->payload_size, key.size());

  // Payload (contains the key of the deleted record)
  const auto stored = PayloadAt(addr, key.size());
  EXPECT_EQ(std::memcmp(stored.data(), key.data(), key.size()), 0);

  // CRC32c
  EXPECT_EQ(hdr->crc32c, ComputeExpectedCrc(addr, hdr->payload_size));
}
