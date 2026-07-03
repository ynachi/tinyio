#include <gtest/gtest.h>

#include "fastlog/fastlog.hpp"

using namespace fastlog;

class AddressManagerTest : public ::testing::Test {
 protected:
  FastLogSettings settings_;
  AddressManager* am_ = nullptr;

  void SetUp() override {
    // Use small, easy-to-reason-about numbers for testing
    // Page: 4KB (12 bits)
    // Memory: 64KB (16 bits) -> 16 pages
    // Segment: 1MB (20 bits)
    settings_.page_bits = 12;
    settings_.memory_bits = 16;
    settings_.segment_bits = 20;
    settings_.mutable_frac = 0.5;

    // We must validate to ensure no exceptions are thrown during setup
    settings_.Validate();

    am_ = new AddressManager(settings_, 0);
  }

  void TearDown() override { delete am_; }
};

TEST_F(AddressManagerTest, Initialization) {
  EXPECT_EQ(am_->GetBegin(), 0);
  EXPECT_EQ(am_->GetHead(), 0);
  EXPECT_EQ(am_->GetTail(), 0);
  EXPECT_EQ(am_->GetCommitted(), 0);
  EXPECT_EQ(am_->MemoryCapacity(), 65536);  // 2^16
  EXPECT_EQ(am_->ResidentSize(), 0);
  EXPECT_EQ(am_->UnflushedBytes(), 0);
}

TEST_F(AddressManagerTest, AddressMath) {
  // 4KB page size = 4096
  constexpr LogAddress addr1 = 4096 + 100;  // Page 1, Offset 100
  EXPECT_EQ(am_->PageNumberOf(addr1), 1);
  EXPECT_EQ(am_->OffsetInPageOf(addr1), 100);
  EXPECT_EQ(am_->SlotOf(addr1), 1);  // 16 pages total, slot 1

  constexpr LogAddress addr2 = (16 * 4096) + 500;  // Page 16, wraps to slot 0
  EXPECT_EQ(am_->PageNumberOf(addr2), 16);
  EXPECT_EQ(am_->SlotOf(addr2), 0);
  EXPECT_EQ(am_->OffsetInPageOf(addr2), 500);

  // 1MB segment size = 1048576 (256 pages)
  constexpr LogAddress addr3 = (257 * 4096) + 50;  // Segment 1, Page in segment 1
  EXPECT_EQ(am_->SegmentNumberOf(addr3), 1);
  EXPECT_EQ(am_->PageNumberInSegmentOf(addr3), 1);
  EXPECT_EQ(am_->OffsetInSegmentOf(addr3), 4096 + 50);
}

TEST_F(AddressManagerTest, Mutators) {
  EXPECT_TRUE(am_->HasCapacityFor(1000));

  am_->AdvanceTail(1000);
  EXPECT_EQ(am_->GetTail(), 1000);
  EXPECT_EQ(am_->ResidentSize(), 1000);
  EXPECT_EQ(am_->UnflushedBytes(), 1000);

  am_->AdvanceCommitted(500);
  EXPECT_EQ(am_->GetCommitted(), 500);
  EXPECT_EQ(am_->UnflushedBytes(), 500);  // 1000 - 500

  am_->AdvanceHead(500);
  EXPECT_EQ(am_->GetHead(), 500);
  EXPECT_EQ(am_->ResidentSize(), 500);  // tail(1000) - head(500)

  am_->AdvanceBegin(250);
  EXPECT_EQ(am_->GetBegin(), 250);
}

TEST_F(AddressManagerTest, CapacityCheck) {
  // Capacity is 64KB (65536 bytes)
  am_->AdvanceTail(65500);
  EXPECT_TRUE(am_->HasCapacityFor(36));
  EXPECT_FALSE(am_->HasCapacityFor(37));

  // Free up space by moving head
  am_->AdvanceHead(4096);

  // Now has 4096 + 36 = 4132 bytes free in total.
  // BUT if a record is > 36 bytes, it will pad and waste those 36 bytes!
  // So the maximum record it can fit is 4132 - 36 = 4096 bytes.
  EXPECT_TRUE(am_->HasCapacityFor(37));
  EXPECT_TRUE(am_->HasCapacityFor(4096));
  EXPECT_FALSE(am_->HasCapacityFor(4097));
}
