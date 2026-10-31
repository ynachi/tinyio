#include "tinyio/detail/ring_buffer.hpp"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

using tinyio::detail::RingBuffer;

namespace {

// Drains a ring into a vector via repeated Pop().
std::vector<int> Drain(RingBuffer<int>& ring) {
    std::vector<int> result;
    int value = 0;
    while (ring.Pop(value)) {
        result.push_back(value);
    }
    return result;
}

}  // namespace

TEST(RingBufferTest, EmptyRingInvariants) {
    RingBuffer<int> ring{4};
    int value = -1;

    EXPECT_TRUE(ring.Empty());
    EXPECT_EQ(ring.Size(), 0u);
    EXPECT_FALSE(ring.Pop(value));
    EXPECT_EQ(value, -1);  // untouched on failure
}

TEST(RingBufferTest, PushPopIsFifo) {
    RingBuffer<int> ring{8};
    for (int i = 0; i < 5; ++i) {
        ring.Push(i);
    }

    EXPECT_EQ(ring.Size(), 5u);
    EXPECT_EQ(Drain(ring), (std::vector{0, 1, 2, 3, 4}));
    EXPECT_TRUE(ring.Empty());
}

// Interleaved pushes and pops move head_ and tail_ around the array many
// times without ever growing it.
TEST(RingBufferTest, WrapAroundKeepsOrder) {
    RingBuffer<int> ring{4};
    int next_in = 0;
    int next_out = 0;

    for (int round = 0; round < 10; ++round) {
        ring.Push(next_in++);
        ring.Push(next_in++);
        ring.Push(next_in++);
        for (int i = 0; i < 3; ++i) {
            int value = -1;
            ASSERT_TRUE(ring.Pop(value));
            EXPECT_EQ(value, next_out++);
        }
    }
    EXPECT_TRUE(ring.Empty());
}

// Grow while wrapped: the two-piece copy must keep logical order.
TEST(RingBufferTest, GrowWhileWrappedKeepsOrder) {
    RingBuffer<int> ring{4};
    ring.Push(0);
    ring.Push(1);
    ring.Push(2);
    int value = 0;
    ASSERT_TRUE(ring.Pop(value));
    ASSERT_TRUE(ring.Pop(value));
    // head_ = 2: the next pushes wrap around to slots 0 and 1.
    for (int i = 3; i < 12; ++i) {
        ring.Push(i);  // grows at size 4, then at size 8
    }

    EXPECT_EQ(ring.Size(), 10u);
    EXPECT_EQ(Drain(ring), (std::vector{2, 3, 4, 5, 6, 7, 8, 9, 10, 11}));
}

TEST(RingBufferTest, ClearEmptiesAndRingStaysUsable) {
    RingBuffer<int> ring{4};
    ring.Push(1);
    ring.Push(2);

    ring.Clear();
    EXPECT_TRUE(ring.Empty());

    ring.Push(3);
    EXPECT_EQ(Drain(ring), (std::vector{3}));
}

TEST(RingBufferTest, WorksWithPointers) {
    int a = 0;
    int b = 0;
    int c = 0;
    RingBuffer<int*> ring{2};
    ring.Push(&a);
    ring.Push(&b);
    ring.Push(&c);  // grows

    int* out = nullptr;
    ASSERT_TRUE(ring.Pop(out));
    EXPECT_EQ(out, &a);
    ASSERT_TRUE(ring.Pop(out));
    EXPECT_EQ(out, &b);
    ASSERT_TRUE(ring.Pop(out));
    EXPECT_EQ(out, &c);
    EXPECT_FALSE(ring.Pop(out));
}

TEST(RingBufferDeathTest, RejectsNonPowerOfTwoCapacity) {
    // No message match: easylog may write the fatal record to a sink other
    // than stderr. Dying at all is the contract.
    EXPECT_DEATH(RingBuffer<int>{3}, "");
}
