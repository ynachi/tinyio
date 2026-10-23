#pragma once

#include <cassert>
#include <cstddef>
#include <optional>
#include <vector>

#include "runtime.hpp"

namespace tinyio {

// A wait list is just an intrusive list of fibers waiting on a condition
using FiberWaitQueue = IntrusiveList<detail::fiber::Fiber, WaitQueueTag, false>;

/// @brief Mirrors std::latch. A one-shot countdown: N fibers do work, one waiter proceeds
/// when all are done.
///
/// A Latch is a synchronization primitive that allows one or more fibers to wait until
/// a set of operations being performed by other fibers completes.
///
/// @note Not thread-safe. Must be used within a single io runtime instance.
///
/// @code
/// tinyio::Latch latch(io, 2);
/// io.spawn([&](tinyio::IO& io) {
///     // do work
///     latch.count_down();
/// });
/// io.spawn([&](tinyio::IO& io) {
///     // do work
///     latch.count_down();
/// });
/// latch.wait(); // suspends until both fibers call count_down
/// @endcode
struct Latch {
  explicit Latch(IO& io, const std::ptrdiff_t count) : io_(io), count_(count) {}

  /// Called by worker fibers. When count reaches 0, all waiters are pushed ready.
  void CountDown(const std::ptrdiff_t n = 1) noexcept {
    count_ -= n;
    if (count_ <= 0) {
      DrainWaiters();
    }
  }

  [[nodiscard]] Result<void> Wait() noexcept {
    auto* self = io_.RunningFiber();
    assert(self != nullptr && "wait() called outside a fiber");
    if (count_ > 0) {
      waiters_.PushBack(self);
      io_.ParkRunning();
      if (io_.Stopping()) {
        return Fail(ECANCELED, "latch.wait");
      }
    }
    return {};
  }

  ~Latch() {
    if (!waiters_.Empty()) {
      ALOG_FATAL("Latch destroyed with fibers waiting");
      std::abort();
    }
  }

 private:
  void DrainWaiters() noexcept {
    while (!waiters_.Empty()) {
      io_.Unpark(waiters_.PopFront());
    }
  }

  IO& io_;
  std::ptrdiff_t count_;
  FiberWaitQueue waiters_;
};

/// @brief A counting gate. Limits how many fibers may be in a critical section
/// concurrently.
///
/// Semaphores are often used to restrict the number of concurrent accesses to a shared
/// resource, or to throttle the execution of fibers.
///
/// @code
/// tinyio::Semaphore sem(io, 2); // At most 2 fibers can acquire concurrently
///
/// io.spawn([&sem](tinyio::IO& io) {
///     sem.acquire();
///     // critical section limited to 2 concurrent fibers
///     sem.release();
/// });
/// @endcode
struct Semaphore {
  explicit Semaphore(IO& io, const std::ptrdiff_t initial) : io_(io), count_(initial) {}

  // Decrements count. Suspends if count == 0.
  [[nodiscard]] Result<void> Acquire() noexcept {
    auto* self = io_.RunningFiber();
    assert(self != nullptr && "acquire() called outside a fiber");
    if (count_ > 0) {
      --count_;
    } else {
      waiters_.PushBack(self);
      io_.ParkRunning();
      if (io_.Stopping()) {
        return Fail(ECANCELED, "semaphore.acquire");
      }
    }
    return {};
  }

  // Increments count. Wakes one waiter if any.
  void Release() noexcept {
    if (!waiters_.Empty()) {
      io_.Unpark(waiters_.PopFront());
    } else {
      ++count_;
    }
  }

  // Non-blocking attempt. Returns false instead of suspending.
  [[nodiscard]] bool TryAcquire() noexcept {
    if (count_ > 0) {
      --count_;
      return true;
    }
    return false;
  }

  ~Semaphore() {
    if (!waiters_.Empty()) {
      ALOG_FATAL("Semaphore destroyed with fibers waiting");
      std::abort();
    }
  }

 private:
  IO& io_;
  std::ptrdiff_t count_;
  FiberWaitQueue waiters_;
};

/// @brief Mutual exclusion for shared state across fibers
///
/// A Mutex ensures that only one fiber at a time can access a critical section of code
/// or shared data. The `scoped_lock` method provides a convenient RAII mechanism to
/// automatically release the lock.
///
/// @code
/// tinyio::Mutex mutex(io);
/// int shared_counter = 0;
///
/// io.spawn([&](tinyio::IO& io) {
///     auto guard = mutex.scoped_lock();
///     shared_counter++; // safely incremented
/// });
/// @endcode
struct Mutex {
  explicit Mutex(IO& io) : io_(io) {}

  [[nodiscard]] Result<void> Lock() noexcept {
    auto* self = io_.RunningFiber();
    assert(self != nullptr && "lock() called outside a fiber");
    if (!held_) {
      held_ = true;
      return {};
    }
    waiters_.PushBack(self);
    io_.ParkRunning();
    if (io_.Stopping()) {
      return Fail(ECANCELED, "mutex.lock");
    }
    return {};
  }

  void Unlock() noexcept {
    if (!waiters_.Empty()) {
      // Ownership transfers directly to the woken fiber, so held_
      // intentionally stays true.
      io_.Unpark(waiters_.PopFront());
    } else {
      held_ = false;
    }
  }

  [[nodiscard]] bool TryLock() noexcept {
    if (!held_) {
      held_ = true;
      return true;
    }
    return false;
  }

  // RAII guard
  struct Guard {
    explicit Guard(Mutex* m) : m_(m) {}
    ~Guard() {
      if (m_) m_->Unlock();
    }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    Guard(Guard&& other) noexcept : m_(other.m_) { other.m_ = nullptr; }
    Guard& operator=(Guard&& other) noexcept {
      if (this != &other) {
        if (m_) m_->Unlock();
        m_ = other.m_;
        other.m_ = nullptr;
      }
      return *this;
    }

   private:
    Mutex* m_;
  };

  [[nodiscard]] Result<Guard> ScopedLock() noexcept {
    if (auto res = Lock(); !res) {
      return std::unexpected(res.error());
    }
    return Guard{this};
  }

  ~Mutex() {
    if (!waiters_.Empty()) {
      ALOG_FATAL("Mutex destroyed with fibers waiting");
      std::abort();
    }
  }

 private:
  IO& io_;
  bool held_{false};
  FiberWaitQueue waiters_;
};

/// @brief Ties fiber lifetime to a lexical scope. Ensures that spawned child fibers
/// cannot outlive the group.
///
template <typename T = void>
struct TaskGroup;

/// A TaskGroup allows you to spawn multiple fibers and wait for all of them to complete.
/// This prevents "runaway" fibers that outlive the scope where they were created.
/// The destructor asserts that no tasks are still active, enforcing that `wait()` is
/// called before the group is destroyed.
///
/// @code
/// {
///     tinyio::TaskGroup group(io);
///     group.spawn([](tinyio::IO& io) {
///         // Task 1
///     });
///     group.spawn([](tinyio::IO& io) {
///         // Task 2
///     });
///     group.wait(); // Suspends until Task 1 and Task 2 are finished
/// }
/// @endcode
template <>
struct TaskGroup<void> {
  explicit TaskGroup(IO& io) : io_(io) {}

  // Spawns a child fiber tracked by this group.
  template <class Fn>
    requires std::constructible_from<std::move_only_function<void(IO&) noexcept>, Fn>
  [[nodiscard]] Result<void> Spawn(Fn&& task) noexcept {
    auto res = io_.Spawn([this, t = std::forward<Fn>(task)](IO& io) mutable noexcept {
      t(io);
      this->OnTaskDone();
    });
    if (res.has_value()) {
      ++active_tasks_;
      return {};
    }
    return std::unexpected(res.error());
  }

  // Suspends calling fiber until all spawned children have finished.
  [[nodiscard]] Result<void> Wait() noexcept {
    auto* self = io_.RunningFiber();
    assert(self != nullptr && "wait() called outside a fiber");
    if (active_tasks_ > 0) {
      waiters_.PushBack(self);
      io_.ParkRunning();
      if (io_.Stopping()) {
        return Fail(ECANCELED, "taskgroup.wait");
      }
    }
    return {};
  }

  ~TaskGroup() {
    // Hard check, a live task would
    // later invoke on_task_done() through a dangling `this` — a UAF inside
    // a fiber callback is strictly worse than aborting here.
    if (active_tasks_ != 0) {
      ALOG_FATAL("TaskGroup destroyed with {} live task(s); call wait() first",
                 active_tasks_);
      std::abort();
    }
  }

 private:
  void OnTaskDone() noexcept {
    --active_tasks_;
    if (active_tasks_ == 0) {
      while (!waiters_.Empty()) {
        io_.Unpark(waiters_.PopFront());
      }
    }
  }

  IO& io_;
  std::ptrdiff_t active_tasks_ = 0;
  FiberWaitQueue waiters_;
};

// Deduction guide for TaskGroup (deduces to TaskGroup<void>)
TaskGroup(IO&) -> TaskGroup<void>;

/// @brief An extension of TaskGroup that collects a Result<T> per task into a pre-sized
/// vector.
///
/// Tasks are assigned slots in spawn order (slot 0, 1, 2, ...). Unspawned slots
/// remain std::nullopt. A spawn failure pre-populates the slot with the error so
/// that results() is the single source of truth after wait().
///
/// Requires: T is nothrow-move-constructible (enforced at instantiation time).
///
/// @code
/// tinyio::TaskGroup<int> group(io, 2);
/// group.spawn([](tinyio::IO& io) -> tinyio::Result<int> {
///     return 42;
/// });
/// group.spawn([](tinyio::IO& io) -> tinyio::Result<int> {
///     return 100;
/// });
/// group.wait();
///
/// for (const auto& res : group.results()) {
///     if (res && res->has_value()) {
///         // res->value() contains 42, then 100
///     }
/// }
/// @endcode
template <typename T>
struct TaskGroup {
  static_assert(std::is_nothrow_move_constructible_v<T>,
                "TaskGroup<T>: T must be nothrow-move-constructible");

  explicit TaskGroup(IO& io, std::size_t capacity) : tg_(io), results_(capacity) {}

  // Spawns the next child fiber; its result lands in the next available slot.
  // If the runtime cannot allocate a fiber, the slot is pre-populated with the
  // error and the error is also returned (for early-abort if desired).
  template <class Fn>
    requires std::constructible_from<std::move_only_function<Result<T>(IO&) noexcept>, Fn>
  Result<void> Spawn(Fn&& task) noexcept {
    assert(next_ < results_.size() && "TaskGroup: more spawns than capacity");
    std::size_t index = next_++;
    auto res = tg_.Spawn([this, index, t = std::forward<Fn>(task)](
                             IO& io) mutable noexcept { results_[index] = t(io); });
    if (!res) {
      results_[index] = std::unexpected(res.error());
    }
    return res;
  }

  // Suspends the calling fiber until all spawned children have finished.
  [[nodiscard]] Result<void> Wait() noexcept { return tg_.Wait(); }

  // Returns all result slots. Check each entry: nullopt = never spawned,
  // has_value() = task succeeded, !has_value() = task returned an error.
  std::vector<std::optional<Result<T>>>& Results() noexcept { return results_; }
  const std::vector<std::optional<Result<T>>>& Results() const noexcept {
    return results_;
  }

 private:
  TaskGroup<> tg_;
  std::vector<std::optional<Result<T>>> results_;
  std::size_t next_ = 0;
};

}  // namespace tinyio
