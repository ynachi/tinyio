#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "tinyio/runtime/runtime.hpp"

namespace tinyio {
/// @brief A one-shot baton: one Wait(), released by one Post().
///
/// Closer to absl::Notification than to folly::Baton: there is no Reset(), a
/// posted baton stays posted forever, and a fresh wait is a fresh object.
/// Removing Reset() removes the hardest part — the window where a Post()
/// lands between "work done" and "reset" and is wiped out.
///
/// State machine:
///
///     Empty ──Wait()──▶ Waiting ──Post()──▶ Posted   (terminal)
///       └───────────────Post()──────────────▶ Posted   (terminal)
///
/// - Post() on Posted does nothing, so posting is idempotent.
/// - Wait() on Posted returns at once; repeated waits after the post are
///   harmless and allowed.
/// - A second concurrent Wait() is a hard failure: one-shot means one
///   waiter, and a second one could only be dropped silently.
///
/// The re-check rule (how to sleep on a Baton):
///
///     Baton* wake_ = nullptr;              // the published sleeping baton
///     void Wake() noexcept {               // called by producers
///         if (wake_ != nullptr) {
///             std::exchange(wake_, nullptr)->Post();
///         }
///     }
///     while (!shutting_down_) {
///         if (!HasWork()) {                // (1) re-check the predicate FIRST
///             Baton wake{io_};             // (2) fresh, one-shot
///             wake_ = &wake;               // (3) publish
///             wake.Wait();                 //     sleep
///             continue;                    //     loop back to the re-check
///         }
///         // ... do the work (may suspend in I/O) ...
///     }
///
/// A Wake() that fires while the sleeper is away doing I/O finds
/// wake_ == nullptr and is dropped. That is NOT a lost wakeup: the producer
/// advances the predicate — the sticky state — before calling Wake(), so the
/// sleeper's re-check at (1) sees the work and never sleeps. Steps (1)–(3)
/// need no lock: the runtime is cooperative, no other fiber runs between the
/// check and the publish.
///
/// Lifetime rules:
/// 1. Clear the published pointer before (or inside) Post(), as
///    std::exchange does in Wake() above. Once the waiter resumes, its stack
///    frame and the baton on it are gone; a stale wake_ would point at a
///    dead stack frame.
/// 2. Post() must not touch the baton after Unpark(). In tinyio this holds
///    by construction — Unpark() only queues the fiber ready, it does not
///    run until the poster yields — but it is the bug class folly's Baton
///    had to defend against across threads, so it is stated here as a
///    contract on every caller.
///
/// @note Not thread-safe. Must be used within a single io runtime instance.
///
/// @code
/// tinyio::Baton baton(io);
/// (void)io.Spawn([&](tinyio::IO&) noexcept { baton.Post(); });
/// (void)baton.Wait(); // suspends until the Post()
/// @endcode
struct Baton {
    explicit Baton(IO& io) noexcept : io_(io) {}

    Baton(const Baton&) = delete;
    Baton& operator=(const Baton&) = delete;

    /// Suspends the calling fiber until Post(). Returns at once when the
    /// baton is already posted.
    [[nodiscard]] Result<void> Wait() noexcept {
        assert(io_.RunningFiber() != nullptr && "Wait() called outside a fiber");
        switch (state_) {
            case State::posted:
                return {};
            case State::waiting:
                // The first waiter is parked inside this call, so only a
                // second fiber can get here. One-shot means one waiter; a
                // second one would have to be silently dropped.
                ELOGFMT(FATAL, "Baton: second concurrent Wait() is not allowed (one-shot)");
                std::abort();
            case State::empty:
                waiter_ = io_.RunningFiber();
                state_ = State::waiting;
                io_.ParkRunning(detail::fiber::WaitKind::baton);
                return {};
        }
        return {};
    }

    /// True once Post() has been called. Never suspends.
    [[nodiscard]] bool TryWait() const noexcept { return state_ == State::posted; }

    /// Releases the waiter, or makes any future Wait() return at once.
    /// Idempotent.
    ///
    /// The state flips to posted BEFORE the waiter is queued, so a resumed
    /// waiter always observes Posted. The waiter's fiber only runs once the
    /// poster yields, and the poster clears any published pointer BEFORE
    /// calling Post() — after Unpark() the baton may live on the waiter's
    /// dying stack frame and must not be touched again.
    void Post() noexcept {
        if (state_ == State::posted) {
            return;
        }
        state_ = State::posted;
        if (waiter_ != nullptr) {
            io_.Unpark(std::exchange(waiter_, nullptr));
        }
    }

    ~Baton() {
        if (state_ == State::waiting) {
            ELOGFMT(FATAL, "Baton destroyed with its waiter still parked");
            std::abort();
        }
    }

private:
    enum class State : std::uint8_t { empty, waiting, posted };

    IO& io_;
    State state_ = State::empty;
    detail::fiber::Fiber* waiter_ = nullptr;
};

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
/// (void)io.Spawn([&](tinyio::IO&) noexcept {
///     // do work
///     latch.CountDown();
/// });
/// (void)io.Spawn([&](tinyio::IO&) noexcept {
///     // do work
///     latch.CountDown();
/// });
/// (void)latch.Wait(); // suspends until both fibers call CountDown
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
        assert(io_.RunningFiber() != nullptr && "wait() called outside a fiber");
        if (count_ > 0) {
            io_.ParkOn(waiters_, detail::fiber::WaitKind::latch);
        }
        return {};
    }

    ~Latch() {
        if (!waiters_.Empty()) {
            ELOGFMT(FATAL, "Latch destroyed with fibers waiting");
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
/// (void)io.Spawn([&sem](tinyio::IO&) noexcept {
///     (void)sem.Acquire();
///     // critical section limited to 2 concurrent fibers
///     sem.Release();
/// });
/// @endcode
struct Semaphore {
    explicit Semaphore(IO& io, const std::ptrdiff_t initial) : io_(io), count_(initial) {}

    // Decrements count. Suspends if count == 0.
    [[nodiscard]] Result<void> Acquire() noexcept {
        assert(io_.RunningFiber() != nullptr && "acquire() called outside a fiber");
        if (count_ > 0) {
            --count_;
            return {};
        }
        io_.ParkOn(waiters_, detail::fiber::WaitKind::semaphore);
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
            ELOGFMT(FATAL, "Semaphore destroyed with fibers waiting");
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
/// (void)io.Spawn([&](tinyio::IO&) noexcept {
///     auto guard = mutex.ScopedLock().value();
///     shared_counter++; // safely incremented
/// });
/// @endcode
struct Mutex {
    explicit Mutex(IO& io) : io_(io) {}

    [[nodiscard]] Result<void> Lock() noexcept {
        assert(io_.RunningFiber() != nullptr && "lock() called outside a fiber");
        if (!held_) {
            held_ = true;
            return {};
        }
        io_.ParkOn(waiters_, detail::fiber::WaitKind::mutex);
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
            if (m_) {
                m_->Unlock();
            }
        }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        Guard(Guard&& other) noexcept : m_(other.m_) { other.m_ = nullptr; }
        Guard& operator=(Guard&& other) noexcept {
            if (this != &other) {
                if (m_) {
                    m_->Unlock();
                }
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
            ELOGFMT(FATAL, "Mutex destroyed with fibers waiting");
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
///     (void)group.Spawn([](tinyio::IO&) noexcept {
///         // Task 1
///     });
///     (void)group.Spawn([](tinyio::IO&) noexcept {
///         // Task 2
///     });
///     (void)group.Wait(); // Suspends until Task 1 and Task 2 are finished
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
        assert(io_.RunningFiber() != nullptr && "wait() called outside a fiber");
        if (active_tasks_ > 0) {
            io_.ParkOn(waiters_, detail::fiber::WaitKind::task_group);
        }
        return {};
    }

    ~TaskGroup() {
        // Hard check, a live task would
        // later invoke on_task_done() through a dangling `this` — a UAF inside
        // a fiber callback is strictly worse than aborting here.
        if (active_tasks_ != 0) {
            ELOGFMT(FATAL, "TaskGroup destroyed with {} live task(s); call wait() first",
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
/// (void)group.Spawn([](tinyio::IO&) noexcept -> tinyio::Result<int> {
///     return 42;
/// });
/// (void)group.Spawn([](tinyio::IO&) noexcept -> tinyio::Result<int> {
///     return 100;
/// });
/// (void)group.Wait();
///
/// for (const auto& res : group.Results()) {
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
        auto res = tg_.Spawn([this, index, t = std::forward<Fn>(task)](IO& io) mutable noexcept {
            results_[index] = t(io);
        });
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
    const std::vector<std::optional<Result<T>>>& Results() const noexcept { return results_; }

private:
    TaskGroup<> tg_;
    std::vector<std::optional<Result<T>>> results_;
    std::size_t next_ = 0;
};

}  // namespace tinyio
