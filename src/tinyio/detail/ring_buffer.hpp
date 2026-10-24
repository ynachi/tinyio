#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <type_traits>

#include "tinyio/logging.hpp"

namespace tinyio::detail {

/// \brief Growable FIFO queue of trivially copyable values (ported from kio).
///
/// A flat array used as a circle. Three ideas make it work:
///
/// 1. The capacity is always a power of two, so a logical index maps to a
///    slot with a bitwise AND instead of a modulo:
///      slot = index & (capacity - 1)
///
/// 2. head_ and tail_ only ever increase; the SLOT index wraps, the counters
///    never do. The size is always tail_ - head_, which unsigned arithmetic
///    keeps correct even past 2^64. Never compare head_ < tail_.
///
/// 3. When the ring is full, Push() doubles the array and copies the items
///    into it in logical order, so head_ restarts at 0.
///
///    cap = 8, head_ = 4, tail_ = 10 (size 6), wrapped:
///
///      slot:   0   1   2   3   4   5   6   7
///            [ I | J |   |   | E | F | G | H ]
///                    ^           ^
///             tail_ & 7 = 2   head_ & 7 = 4
///
/// The storage is allocated on the first Push(), so an idle queue (a Mutex
/// nobody ever waits on) costs no memory.
///
/// Single-threaded: owner-thread only, like the rest of the runtime.
template <typename T>
class RingBuffer {
  // Grow moves items with memcpy.
  static_assert(std::is_trivially_copyable_v<T>,
                "RingBuffer requires a trivially copyable type");

 public:
  static constexpr std::uint64_t k_default_initial_capacity = 64;
  /// 8 MiB of pointers. Growing past this is a runaway enqueue, not a load.
  static constexpr std::uint64_t k_max_capacity = 1ULL << 20U;

  explicit RingBuffer(
      const std::uint64_t initial_capacity = k_default_initial_capacity) noexcept
      : initial_cap_(initial_capacity) {
    if (initial_capacity == 0 || (initial_capacity & (initial_capacity - 1)) != 0) {
      ELOGFMT(FATAL, "RingBuffer capacity must be a power of two, got {}",
              initial_capacity);
      std::abort();
    }
  }

  RingBuffer(const RingBuffer&) = delete;
  RingBuffer& operator=(const RingBuffer&) = delete;

  /// Allocates only when the ring is full. An allocation failure inside this
  /// noexcept function terminates, like every other OOM in the runtime.
  void Push(const T& item) noexcept {
    if (Size() == cap_) {
      Grow();
    }
    slots_[tail_ & (cap_ - 1)] = item;
    ++tail_;
  }

  /// Pops the oldest item into @p out. Returns false when the ring is empty.
  [[nodiscard]] bool Pop(T& out) noexcept {
    if (Empty()) {
      return false;
    }
    out = slots_[head_ & (cap_ - 1)];
    ++head_;
    return true;
  }

  /// Removes every item equal to @p item and keeps the order of the others.
  /// O(n): meant for rare paths only (a fiber leaving a wait queue during
  /// teardown), never for the hot path.
  void Erase(const T& item) noexcept {
    std::uint64_t write = head_;
    for (std::uint64_t read = head_; read != tail_; ++read) {
      const T& current = slots_[read & (cap_ - 1)];
      if (!(current == item)) {
        slots_[write & (cap_ - 1)] = current;
        ++write;
      }
    }
    tail_ = write;
  }

  /// Drops every item and keeps the storage.
  void Clear() noexcept { head_ = tail_ = 0; }

  [[nodiscard]] bool Empty() const noexcept { return head_ == tail_; }

  [[nodiscard]] std::uint64_t Size() const noexcept { return tail_ - head_; }

 private:
  // Doubles the capacity (or allocates the first array) and copies the items
  // in logical order. A wrapped ring is split in two pieces, so the copy is
  // two memcpys:
  //
  //   old: [ I | J |   |   | E | F | G | H ]   new: [ E | F | G | H | I | J | ...
  //          <-2nd->         <----1st---->
  void Grow() noexcept {
    const std::uint64_t new_cap = cap_ == 0 ? initial_cap_ : cap_ * 2;
    if (new_cap > k_max_capacity) {
      ELOGFMT(FATAL, "RingBuffer refuses to grow past {} entries (runaway enqueue?)",
              k_max_capacity);
      std::abort();
    }

    // for_overwrite: no point zeroing slots that are written before read.
    auto new_slots = std::make_unique_for_overwrite<T[]>(new_cap);
    const std::uint64_t n = Size();
    if (n > 0) {
      const std::uint64_t first = std::min(n, cap_ - (head_ & (cap_ - 1)));
      std::memcpy(new_slots.get(), &slots_[head_ & (cap_ - 1)], first * sizeof(T));
      std::memcpy(new_slots.get() + first, slots_.get(), (n - first) * sizeof(T));
    }

    slots_ = std::move(new_slots);
    cap_ = new_cap;
    head_ = 0;
    tail_ = n;
  }

  std::uint64_t head_ = 0;  ///< Monotonic read index
  std::uint64_t tail_ = 0;  ///< Monotonic write index
  std::uint64_t cap_ = 0;   ///< 0 until the first Push(), then a power of two
  std::uint64_t initial_cap_;
  std::unique_ptr<T[]> slots_;  // NOLINT(modernize-avoid-c-arrays)
};

}  // namespace tinyio::detail
