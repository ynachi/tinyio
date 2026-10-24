#include "tinyio/runtime/buffer_pool.hpp"

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <set>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "tinyio/error.hpp"
#include "tinyio/runtime/runtime.hpp"

using namespace tinyio;

namespace {

IoOptions WithPool(const size_t slot_size, const uint32_t slots) {
  IoOptions opts;
  opts.buffer_pool = BufferPoolConfig{.slot_size = slot_size, .slots = slots};
  return opts;
}

// A runtime with a single 4 KiB slot: every handoff is observable.
class FixedBufferPoolFiberTest : public ::testing::Test {
 protected:
  IO io_{0, WithPool(4096, 1)};
};

// easylog writes the fatal record to the console (stdout), while a death
// test captures stderr. Redirecting stdout to stderr inside the child puts
// the message into the captured output, so the test can assert on it.
void RedirectStdoutToStderr() { (void)::dup2(STDERR_FILENO, STDOUT_FILENO); }

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST(FixedBufferPoolTest, ConstructorRejectsBadSlotGeometry) {
  // not a power of two
  EXPECT_THROW(FixedBufferPool(3000, 4), std::invalid_argument);
  // below one page
  EXPECT_THROW(FixedBufferPool(2048, 4), std::invalid_argument);
  // 2 GiB slots: cannot be sliced into 1 GiB registration chunks
  EXPECT_THROW(FixedBufferPool(size_t{1} << 31U, 4), std::invalid_argument);
  // no slots
  EXPECT_THROW(FixedBufferPool(4096, 0), std::invalid_argument);
}

TEST(FixedBufferPoolTest, GeometryIsTwoMiBAligned) {
  FixedBufferPool pool(4096, 8);

  EXPECT_EQ(reinterpret_cast<uintptr_t>(pool.Base()) % FixedBufferPool::kHugePageSize,
            0u);
  EXPECT_EQ(pool.AllocatedBytes() % FixedBufferPool::kHugePageSize, 0u);
  EXPECT_GE(pool.AllocatedBytes(), pool.TotalBytes());
  EXPECT_EQ(pool.TotalBytes(), 4096u * 8u);
  EXPECT_EQ(pool.SlotSize(), 4096u);
  EXPECT_EQ(pool.Capacity(), 8u);
}

// ---------------------------------------------------------------------------
// Registration math
// ---------------------------------------------------------------------------

TEST(FixedBufferPoolTest, SlotMathTwoRegions) {
  // 1 MiB slots x 1537 = ~1.5 GiB of virtual address space. The mapping is
  // lazy and nothing is registered here, so no page is ever touched.
  FixedBufferPool pool(1 << 20, 1537);

  std::vector<iovec> table;
  const uint16_t base_idx = pool.AppendRegions(table);

  EXPECT_EQ(table.size(), 2u);
  EXPECT_EQ(base_idx, 0);
  EXPECT_EQ(table[0].iov_len, FixedBufferPool::kOneGiB);
  EXPECT_EQ(table[1].iov_len, pool.TotalBytes() - FixedBufferPool::kOneGiB);

  // Branchless region math: region 0 holds slots [0, 1023], region 1 the
  // remaining 513.
  EXPECT_EQ(pool.RegistrationIndex(pool.Base()), 0);
  EXPECT_EQ(pool.RegistrationIndex(pool.Base() + size_t{0} * pool.SlotSize()), 0);
  EXPECT_EQ(pool.RegistrationIndex(pool.Base() + size_t{1023} * pool.SlotSize()), 0);
  EXPECT_EQ(pool.RegistrationIndex(pool.Base() + size_t{1024} * pool.SlotSize()), 1);
  EXPECT_EQ(pool.RegistrationIndex(pool.Base() + size_t{1536} * pool.SlotSize()), 1);
}

TEST(FixedBufferPoolTest, ThreeRegionAppendAndBoundaryIndexes) {
  // 64 KiB x 40960 slots = 2.5 GiB: three registration chunks of 1 GiB +
  // 1 GiB + 0.5 GiB. Pages are never touched (lazy mapping).
  FixedBufferPool pool(65536, 40960);

  std::vector<iovec> table{
      {nullptr, 1},
      {nullptr, 1},
  };  // two foreign entries
  const uint16_t first = pool.AppendRegions(table);

  EXPECT_EQ(first, 2u);
  ASSERT_EQ(table.size(), 5u);
  EXPECT_EQ(table[2].iov_len, FixedBufferPool::kOneGiB);
  EXPECT_EQ(table[3].iov_len, FixedBufferPool::kOneGiB);
  EXPECT_EQ(table[4].iov_len, pool.TotalBytes() - 2 * FixedBufferPool::kOneGiB);

  // Chunk-boundary bytes resolve to the right table index.
  EXPECT_EQ(pool.RegistrationIndex(pool.Base()), 2u);
  EXPECT_EQ(pool.RegistrationIndex(pool.Base() + FixedBufferPool::kOneGiB - 1), 2u);
  EXPECT_EQ(pool.RegistrationIndex(pool.Base() + FixedBufferPool::kOneGiB), 3u);
  EXPECT_EQ(pool.RegistrationIndex(pool.Base() + 2 * FixedBufferPool::kOneGiB), 4u);
  EXPECT_EQ(pool.RegistrationIndex(pool.Base() + pool.TotalBytes() - 1), 4u);

  // No slot may straddle a 1 GiB registration boundary: the slot size
  // (64 KiB = 2^16) evenly divides 1 GiB (2^30), so every chunk boundary
  // falls exactly on a slot boundary.
  static_assert((FixedBufferPool::kOneGiB % 65536) == 0);
  for (const uint32_t ord : {0u, 16383u, 16384u, 32767u, 32768u, 40959u}) {
    const size_t off = size_t{ord} * 65536;
    EXPECT_EQ(off >> 30U, (off + 65535) >> 30U)
        << "slot " << ord << " straddles a chunk boundary";
  }
}

// ---------------------------------------------------------------------------
// Take / release
// ---------------------------------------------------------------------------

TEST(FixedBufferPoolTest, TakeReturnsValidBuffer) {
  FixedBufferPool pool(4096, 4);

  auto buf = pool.Take();
  ASSERT_TRUE(buf.has_value());
  EXPECT_TRUE(buf->Valid());
  EXPECT_NE(buf->Ptr(), nullptr);
  EXPECT_EQ(buf->Size(), 4096u);
  EXPECT_EQ(buf->Data().size(), 4096u);
  EXPECT_EQ(pool.Available(), 3u);
}

TEST(FixedBufferPoolTest, LifoWarmthOrder) {
  FixedBufferPool pool(4096, 4);

  // Seeded slots-1..0, so pops come out 0, 1, 2, 3.
  auto a = pool.Take();
  auto b = pool.Take();
  auto c = pool.Take();
  auto d = pool.Take();
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  ASSERT_TRUE(c.has_value());
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(a->Ordinal(), 0u);
  EXPECT_EQ(b->Ordinal(), 1u);
  EXPECT_EQ(c->Ordinal(), 2u);
  EXPECT_EQ(d->Ordinal(), 3u);

  // Release 1 then 3: the LIFO top is now 3, then 1 (recently returned slots
  // are the hot ones).
  b->Release();
  d->Release();
  EXPECT_EQ(pool.Available(), 2u);

  auto e = pool.Take();
  auto f = pool.Take();
  ASSERT_TRUE(e.has_value());
  ASSERT_TRUE(f.has_value());
  EXPECT_EQ(e->Ordinal(), 3u);
  EXPECT_EQ(f->Ordinal(), 1u);
}

TEST(FixedBufferPoolTest, OrdinalsAreUniqueAndDense) {
  FixedBufferPool pool(4096, 8);

  std::vector<FixedBuffer> leases;
  for (uint32_t i = 0; i < 8; ++i) {
    auto buf = pool.Take();
    ASSERT_TRUE(buf.has_value());
    leases.push_back(std::move(*buf));
  }

  std::set<uint32_t> ordinals;
  for (const auto& lease : leases) {
    EXPECT_TRUE(lease.Valid());
    ordinals.insert(lease.Ordinal());
    EXPECT_EQ(lease.Ptr(), pool.Base() + size_t{lease.Ordinal()} * pool.SlotSize());
    EXPECT_EQ(lease.Size(), 4096u);
  }
  EXPECT_EQ(ordinals.size(), 8u);

  // Exhausted: the next non-blocking take fails with EAGAIN.
  auto extra = pool.Take();
  ASSERT_FALSE(extra.has_value());
  EXPECT_EQ(extra.error().value(), EAGAIN);
  EXPECT_EQ(pool.Available(), 0u);
}

TEST(FixedBufferPoolTest, DestructorReleasesSlotBackToPool) {
  FixedBufferPool pool(4096, 4);
  EXPECT_EQ(pool.Available(), 4u);
  {
    auto buf = pool.Take();
    ASSERT_TRUE(buf.has_value());
    EXPECT_EQ(pool.Available(), 3u);
  }
  // buf destroyed here -- the slot must be back.
  EXPECT_EQ(pool.Available(), 4u);
}

TEST(FixedBufferPoolTest, ExplicitReleaseIsIdempotent) {
  FixedBufferPool pool(4096, 4);

  auto buf = pool.Take();
  ASSERT_TRUE(buf.has_value());

  buf->Release();
  EXPECT_FALSE(buf->Valid());
  EXPECT_EQ(pool.Available(), 4u);

  // A second release must be a no-op -- no double return to the pool.
  buf->Release();
  EXPECT_EQ(pool.Available(), 4u);
}

// ---------------------------------------------------------------------------
// Move semantics
// ---------------------------------------------------------------------------

TEST(FixedBufferPoolTest, MoveConstructorTransfersOwnership) {
  FixedBufferPool pool(4096, 4);

  auto src = pool.Take();
  ASSERT_TRUE(src.has_value());
  EXPECT_EQ(pool.Available(), 3u);

  FixedBuffer dst = std::move(*src);

  EXPECT_TRUE(dst.Valid());
  EXPECT_FALSE(src->Valid());
  // Still one slot outstanding -- the move did not release it.
  EXPECT_EQ(pool.Available(), 3u);
}

TEST(FixedBufferPoolTest, MoveAssignmentReleasesExistingBuffer) {
  FixedBufferPool pool(4096, 4);

  auto buf1 = pool.Take();
  auto buf2 = pool.Take();
  ASSERT_TRUE(buf1.has_value());
  ASSERT_TRUE(buf2.has_value());
  EXPECT_EQ(pool.Available(), 2u);

  // Assign buf2 into buf1: buf1's old slot must be released before taking
  // buf2's.
  *buf1 = std::move(*buf2);

  EXPECT_TRUE(buf1->Valid());
  EXPECT_FALSE(buf2->Valid());
  // One slot was released (old buf1), one is still held (now in buf1).
  EXPECT_EQ(pool.Available(), 3u);
}

TEST(FixedBufferPoolTest, SelfMoveAssignmentIsNoOp) {
  FixedBufferPool pool(4096, 4);

  auto buf = pool.Take();
  ASSERT_TRUE(buf.has_value());

  auto* raw = &*buf;
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wself-move"
#else
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wself-move"
#endif
  *raw = std::move(*raw);
#if defined(__clang__)
#  pragma clang diagnostic pop
#else
#  pragma GCC diagnostic pop
#endif

  // The buffer must still be valid after the self-move.
  EXPECT_TRUE(buf->Valid());
  EXPECT_EQ(pool.Available(), 3u);
}

TEST(FixedBufferPoolDeathTest, LeaseBoundsChecksTerminate) {
  FixedBufferPool pool(4096, 1);
  auto buf = pool.Take();
  ASSERT_TRUE(buf.has_value());

  EXPECT_DEATH(
      {
        RedirectStdoutToStderr();
        (void)buf->First(4097);
      },
      "exceeds slot size");
  EXPECT_DEATH(
      {
        RedirectStdoutToStderr();
        (void)buf->Subspan(4000, 100);
      },
      "outside slot");
}

TEST(FixedBufferPoolDeathTest, RegistrationIndexOutsideMappingTerminates) {
  FixedBufferPool pool(4096, 1);
  EXPECT_DEATH(
      {
        RedirectStdoutToStderr();
        (void)pool.RegistrationIndex(pool.Base() + pool.TotalBytes());
      },
      "outside the pool mapping");
}

// The kernel resolves the buf_index against the registered table, so a lease
// from a pool this runtime does not know about names the wrong buffer. That
// is a programming error, not an I/O failure: it terminates.
TEST(FixedBufferPoolDeathTest, ReadFixedOnAnUnregisteredPoolTerminates) {
  IO io{0, WithPool(4096, 1)};

  // A pool that was never registered with this runtime.
  FixedBufferPool foreign{4096, 1};
  auto buf = foreign.Take();
  ASSERT_TRUE(buf.has_value());

  EXPECT_DEATH(
      {
        RedirectStdoutToStderr();
        (void)io.ReadFixed(UniqueFd{}, *buf, 16, 0);
      },
      "not registered with this runtime");
}

// ---------------------------------------------------------------------------
// Asynchronous acquisition (fiber driven)
// ---------------------------------------------------------------------------

// The holder takes the only slot, sleeps, then releases it: the release is
// what wakes waiter #1, whose own release chains the wake to #2, then #3.
TEST_F(FixedBufferPoolFiberTest, FifoWakeChain) {
  std::vector<int> order;

  (void)io_.Spawn([](IO& rt) noexcept {
    auto buf = rt.AcquireFixedBuffer();
    EXPECT_TRUE(buf.has_value());
    (void)rt.SleepForNs(std::chrono::milliseconds(10));
    buf->Release();
  });

  for (int id = 1; id <= 3; ++id) {
    (void)io_.Spawn([&order, id](IO& rt) noexcept {
      auto buf = rt.AcquireFixedBuffer();
      EXPECT_TRUE(buf.has_value());
      order.push_back(id);
      buf->Release();
    });
  }

  io_.RunUntilDone();

  // Completions come out in spawn order despite the single slot.
  EXPECT_EQ(order, (std::vector{1, 2, 3}));
}

// The releaser must not steal the slot it just handed to the woken waiter:
// the slot is handed over, not put back on the free list.
TEST_F(FixedBufferPoolFiberTest, ReleasedSlotIsHandedToTheWaiter) {
  bool thief_blocked = false;
  bool waiter_ok = false;

  (void)io_.Spawn([&thief_blocked](IO& rt) noexcept {
    auto buf = rt.AcquireFixedBuffer();
    EXPECT_TRUE(buf.has_value());
    (void)rt.SleepForNs(std::chrono::milliseconds(10));
    buf->Release();
    // The slot went to the parked waiter, so the free list is empty.
    auto stolen = rt.TakeFixedBuffer();
    thief_blocked = !stolen.has_value();
  });

  (void)io_.Spawn([&waiter_ok](IO& rt) noexcept {
    auto buf = rt.AcquireFixedBuffer();
    waiter_ok = buf.has_value();
  });

  io_.RunUntilDone();

  EXPECT_TRUE(thief_blocked);
  EXPECT_TRUE(waiter_ok);
}

// Closing a pool wakes every parked fiber with operation_canceled. The
// runtime never closes its own pool, so this drives a standalone one.
TEST_F(FixedBufferPoolFiberTest, CloseCancelsWaiters) {
  FixedBufferPool pool(4096, 1);
  bool holder_ok = false;
  bool w1_canceled = false;
  bool w1_code_ok = false;
  bool w2_canceled = false;
  bool w2_code_ok = false;

  (void)io_.Spawn([&](IO& rt) noexcept {
    auto buf = pool.Acquire(rt);
    holder_ok = buf.has_value();
    (void)rt.SleepForNs(std::chrono::milliseconds(5));
  });

  (void)io_.Spawn([&](IO& rt) noexcept {
    auto buf = pool.Acquire(rt);
    w1_canceled = !buf.has_value();
    if (w1_canceled) {
      w1_code_ok = buf.error() == std::errc::operation_canceled;
    }
  });

  (void)io_.Spawn([&](IO& rt) noexcept {
    auto buf = pool.Acquire(rt);
    w2_canceled = !buf.has_value();
    if (w2_canceled) {
      w2_code_ok = buf.error() == std::errc::operation_canceled;
    }
  });

  (void)io_.Spawn([&pool](IO& rt) noexcept {
    (void)rt.SleepForNs(std::chrono::milliseconds(2));
    pool.Close();
  });

  io_.RunUntilDone();

  EXPECT_TRUE(holder_ok);
  EXPECT_TRUE(w1_canceled);
  EXPECT_TRUE(w1_code_ok);
  EXPECT_TRUE(w2_canceled);
  EXPECT_TRUE(w2_code_ok);
}

// A slot handed to a parked waiter is the waiter's, even when the pool is
// closed before the waiter gets to run: it must neither be lost in the
// handoff queue nor turn into operation_canceled.
TEST_F(FixedBufferPoolFiberTest, SlotHandedOverBeforeCloseReachesTheWaiter) {
  FixedBufferPool pool(4096, 1);
  bool waiter_got_slot = false;

  (void)io_.Spawn([&pool](IO& rt) noexcept {
    auto buf = pool.Acquire(rt);
    EXPECT_TRUE(buf.has_value());
    (void)rt.SleepForNs(std::chrono::milliseconds(5));
    buf->Release();  // handed to the parked waiter, which has not run yet
    pool.Close();
  });

  (void)io_.Spawn([&](IO& rt) noexcept {
    auto buf = pool.Acquire(rt);
    waiter_got_slot = buf.has_value();
  });

  io_.RunUntilDone();

  EXPECT_TRUE(waiter_got_slot);
  EXPECT_EQ(pool.Available(), 1u);
}

// A closed pool hands out nothing, not even to a fiber that asks later.
TEST_F(FixedBufferPoolFiberTest, ClosedPoolHandsOutNothing) {
  FixedBufferPool pool(4096, 2);
  pool.Close();

  auto buf = pool.Take();
  ASSERT_FALSE(buf.has_value());
  EXPECT_EQ(buf.error() == std::errc::operation_canceled, true);
  EXPECT_TRUE(pool.Closed());
}

// ---------------------------------------------------------------------------
// Runtime level: one runtime, one pool
// ---------------------------------------------------------------------------

TEST(FixedBufferPoolRuntimeTest, NoPoolUnlessConfigured) {
  IO io{0, IoOptions{}};
  EXPECT_EQ(io.BufferPool(), nullptr);
}

TEST(FixedBufferPoolRuntimeTest, PoolFollowsTheConfig) {
  IO io{0, WithPool(8192, 3)};

  const auto* pool = io.BufferPool();
  ASSERT_NE(pool, nullptr);
  EXPECT_EQ(pool->SlotSize(), 8192u);
  EXPECT_EQ(pool->Capacity(), 3u);

  auto buf = io.TakeFixedBuffer();
  ASSERT_TRUE(buf.has_value());
  EXPECT_EQ(buf->Size(), 8192u);
  EXPECT_EQ(buf->Pool(), pool);
  EXPECT_EQ(pool->Available(), 2u);
}

TEST(FixedBufferPoolRuntimeTest, ExhaustedPoolReturnsEagain) {
  IO io{0, WithPool(4096, 1)};

  auto held = io.TakeFixedBuffer();
  ASSERT_TRUE(held.has_value());

  auto extra = io.TakeFixedBuffer();
  ASSERT_FALSE(extra.has_value());
  EXPECT_EQ(extra.error().value(), EAGAIN);
}

// Asking a runtime without a pool for a fixed buffer is a programming error.
TEST(FixedBufferPoolDeathTest, TakeWithoutAPoolTerminates) {
  IO io{0, IoOptions{}};

  EXPECT_DEATH(
      {
        RedirectStdoutToStderr();
        (void)io.TakeFixedBuffer();
      },
      "no buffer pool configured");
}

// ---------------------------------------------------------------------------
// Runtime teardown with a fiber parked on a slot
// ---------------------------------------------------------------------------

// kio's model: the runtime never wakes a fiber parked on its pool. One still
// parked once the runtime has quiesced waits for a lease nobody will ever
// release (here, one that escaped its fiber), and the runtime aborts instead
// of hanging or handing the waiter a made-up operation_canceled.
TEST(FixedBufferPoolDeathTest, ParkedWaiterAtQuiesceAborts) {
  const auto park_forever = [] {
    IO io{0, WithPool(4096, 1)};
    std::optional<FixedBuffer> escaped;

    (void)io.Spawn([&escaped](IO& rt) noexcept {
      auto buf = rt.AcquireFixedBuffer();
      if (buf.has_value()) {
        escaped.emplace(std::move(*buf));
      }
    });
    (void)io.Spawn([](IO& rt) noexcept { (void)rt.AcquireFixedBuffer(); });

    io.RunUntilDone();
  };

  EXPECT_DEATH(
      {
        RedirectStdoutToStderr();
        park_forever();
      },
      "parked fiber");
}
