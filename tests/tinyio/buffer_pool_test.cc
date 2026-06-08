#include "tinyio/buffer_pool.hpp"

#include <gtest/gtest.h>

using namespace tinyio;

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

class SingleBucketPoolTest : public ::testing::Test
{
protected:
    static constexpr size_t kSlotSize  = 4096;
    static constexpr size_t kSlotCount = 4;

    FixedBufferPool pool_{std::vector<BucketConfig>{{kSlotSize, kSlotCount}}};
};

// Configs are given in REVERSE order to exercise the sort-on-construction path.
// After sorting: bucket 0 = 4096 (3 slots), bucket 1 = 8192 (2 slots).
class MultiBucketPoolTest : public ::testing::Test
{
protected:
    FixedBufferPool pool_{std::vector<BucketConfig>{{8192, 2}, {4096, 3}}};
};

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST(FixedBufferPoolConstruction, EmptyConfigProducesEmptyPool)
{
    FixedBufferPool pool{std::vector<BucketConfig>{}};
    EXPECT_EQ(pool.bucket_count(), 0u);
    EXPECT_EQ(pool.total_capacity(), 0u);
}

TEST(FixedBufferPoolConstruction, MisalignedSizeThrows)
{
    // slot_size must be a multiple of 4096 for io_uring + O_DIRECT compatibility.
    EXPECT_THROW((FixedBufferPool{std::vector<BucketConfig>{{1024, 4}}}), std::invalid_argument);
}

TEST(FixedBufferPoolConstruction, TakeFromEmptyPoolReturnsENOMEM)
{
    FixedBufferPool pool{std::vector<BucketConfig>{}};
    auto buf = pool.take(4096);
    ASSERT_FALSE(buf.has_value());
    EXPECT_EQ(buf.error().code, ENOMEM);
}

// ---------------------------------------------------------------------------
// Single-bucket: basic take / state / capacity
// ---------------------------------------------------------------------------

TEST_F(SingleBucketPoolTest, TakeReturnsValidBuffer)
{
    auto buf = pool_.take(kSlotSize);
    ASSERT_TRUE(buf.has_value());
    EXPECT_TRUE(buf->valid());
    EXPECT_NE(buf->ptr(), nullptr);
}

TEST_F(SingleBucketPoolTest, BufferSizeMatchesBucketSlotSize)
{
    auto buf = pool_.take(kSlotSize);
    ASSERT_TRUE(buf.has_value());
    EXPECT_EQ(buf->size(), kSlotSize);
    EXPECT_EQ(buf->data().size(), kSlotSize);
}

TEST_F(SingleBucketPoolTest, TakeDecrementsAvailableSlots)
{
    EXPECT_EQ(pool_.available_in_bucket(0), kSlotCount);

    auto b0 = pool_.take(kSlotSize);
    EXPECT_EQ(pool_.available_in_bucket(0), kSlotCount - 1);

    auto b1 = pool_.take(kSlotSize);
    EXPECT_EQ(pool_.available_in_bucket(0), kSlotCount - 2);
}

TEST_F(SingleBucketPoolTest, ExhaustedBucketReturnsEAGAIN)
{
    // Drain all slots.
    std::vector<FixedBuffer> held;
    for (size_t i = 0; i < kSlotCount; ++i)
    {
        auto buf = pool_.take(kSlotSize);
        ASSERT_TRUE(buf.has_value()) << "unexpected failure at slot " << i;
        held.push_back(std::move(*buf));
    }

    // One more take must fail — the bucket does not spill to another bucket.
    auto extra = pool_.take(kSlotSize);
    ASSERT_FALSE(extra.has_value());
    EXPECT_EQ(extra.error().code, EAGAIN);
}

// ---------------------------------------------------------------------------
// Single-bucket: release / RAII
// ---------------------------------------------------------------------------

TEST_F(SingleBucketPoolTest, DestructorReleasesSlotBackToPool)
{
    EXPECT_EQ(pool_.available_in_bucket(0), kSlotCount);
    {
        auto buf = pool_.take(kSlotSize);
        ASSERT_TRUE(buf.has_value());
        EXPECT_EQ(pool_.available_in_bucket(0), kSlotCount - 1);
    }
    // buf destroyed here — slot must be back.
    EXPECT_EQ(pool_.available_in_bucket(0), kSlotCount);
}

TEST_F(SingleBucketPoolTest, ExplicitReleaseIsIdempotent)
{
    auto buf = pool_.take(kSlotSize);
    ASSERT_TRUE(buf.has_value());

    buf->release();
    EXPECT_FALSE(buf->valid());
    EXPECT_EQ(pool_.available_in_bucket(0), kSlotCount);

    // Second release must be a no-op — no double-return to pool.
    buf->release();
    EXPECT_EQ(pool_.available_in_bucket(0), kSlotCount);
}

// ---------------------------------------------------------------------------
// FixedBuffer move semantics
// ---------------------------------------------------------------------------

TEST_F(SingleBucketPoolTest, MoveConstructorTransfersOwnership)
{
    auto src = pool_.take(kSlotSize);
    ASSERT_TRUE(src.has_value());
    EXPECT_EQ(pool_.available_in_bucket(0), kSlotCount - 1);

    FixedBuffer dst = std::move(*src);

    EXPECT_TRUE(dst.valid());
    EXPECT_FALSE(src->valid());
    // Still one slot outstanding — the move did not release it.
    EXPECT_EQ(pool_.available_in_bucket(0), kSlotCount - 1);
}

TEST_F(SingleBucketPoolTest, MoveAssignmentReleasesExistingBuffer)
{
    auto buf1 = pool_.take(kSlotSize);
    auto buf2 = pool_.take(kSlotSize);
    ASSERT_TRUE(buf1.has_value());
    ASSERT_TRUE(buf2.has_value());
    EXPECT_EQ(pool_.available_in_bucket(0), kSlotCount - 2);

    // Assign buf2 into buf1: buf1's old slot must be released before taking buf2.
    *buf1 = std::move(*buf2);

    EXPECT_TRUE(buf1->valid());
    EXPECT_FALSE(buf2->valid());
    // One slot was released (old buf1), one is still held (now in buf1).
    EXPECT_EQ(pool_.available_in_bucket(0), kSlotCount - 1);
}

TEST_F(SingleBucketPoolTest, SelfMoveAssignmentIsNoOp)
{
    auto buf = pool_.take(kSlotSize);
    ASSERT_TRUE(buf.has_value());

    auto* raw = &*buf;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-move"
    *raw = std::move(*raw);
#pragma clang diagnostic pop

    // Buffer must still be valid after self-move.
    EXPECT_TRUE(buf->valid());
    EXPECT_EQ(pool_.available_in_bucket(0), kSlotCount - 1);
}

// ---------------------------------------------------------------------------
// Multi-bucket: sorting and bucket selection
// ---------------------------------------------------------------------------

TEST_F(MultiBucketPoolTest, ConstructorSortsBucketsBySlotSize)
{
    // Configs were {8192, 2}, {4096, 3} — must be reordered to ascending.
    EXPECT_EQ(pool_.bucket_count(), 2u);
    EXPECT_EQ(pool_.available_in_bucket(0), 3u);  // 4096-byte bucket
    EXPECT_EQ(pool_.available_in_bucket(1), 2u);  // 8192-byte bucket
    EXPECT_EQ(pool_.total_capacity(), 5u);
}

TEST_F(MultiBucketPoolTest, TakeUsesSmallestFittingBucket)
{
    // Exact match: 4096 should use bucket 0.
    auto buf = pool_.take(4096);
    ASSERT_TRUE(buf.has_value());
    EXPECT_EQ(buf->size(), 4096u);
    EXPECT_EQ(pool_.available_in_bucket(0), 2u);
    EXPECT_EQ(pool_.available_in_bucket(1), 2u);  // unchanged
}

TEST_F(MultiBucketPoolTest, TakeUsesLargerBucketWhenRequestBetweenSizes)
{
    // 5000 > 4096, lower_bound lands on the 8192-byte bucket.
    auto buf = pool_.take(5000);
    ASSERT_TRUE(buf.has_value());
    EXPECT_EQ(buf->size(), 8192u);
    EXPECT_EQ(pool_.available_in_bucket(0), 3u);  // unchanged
    EXPECT_EQ(pool_.available_in_bucket(1), 1u);
}

TEST_F(MultiBucketPoolTest, TakeFailsWhenRequestExceedsAllBuckets)
{
    auto buf = pool_.take(16384);
    ASSERT_FALSE(buf.has_value());
    EXPECT_EQ(buf.error().code, ENOMEM);
}

TEST_F(MultiBucketPoolTest, ExhaustedSmallBucketDoesNotSpillToLarger)
{
    // Drain the 4096-byte bucket completely.
    std::vector<FixedBuffer> held;
    for (size_t i = 0; i < 3; ++i)
    {
        auto buf = pool_.take(4096);
        ASSERT_TRUE(buf.has_value());
        held.push_back(std::move(*buf));
    }

    // Requesting 4096 again must fail — take() does not fall back to the 8192 bucket.
    auto extra = pool_.take(4096);
    ASSERT_FALSE(extra.has_value());
    EXPECT_EQ(extra.error().code, EAGAIN);

    // The 8192-byte bucket is untouched.
    EXPECT_EQ(pool_.available_in_bucket(1), 2u);
}
