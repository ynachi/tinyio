#pragma once

#include "tinyio/runtime.hpp"
#include <cassert>
#include <cstddef>

namespace tinyio
{

// A wait list is just an intrusive list of fibers waiting on a condition
using FiberWaitQueue = IntrusiveList<detail::fiber::Fiber, WaitQueueTag>;

/// @brief Mirrors std::latch. A one-shot countdown: N fibers do work, one waiter proceeds when all are done.
struct FiberLatch {
    explicit FiberLatch(IO& io, std::ptrdiff_t count)
        : io_(io), count_(count) {}

    // Called by worker fibers. When count reaches 0, all waiters are pushed ready.
    void count_down(std::ptrdiff_t n = 1) noexcept
    {
        count_ -= n;
        if (count_ <= 0)
        {
            drain_waiters();
        }
    }

    // Suspends the calling fiber until count == 0. Returns immediately if already 0.
    void wait() noexcept
    {
        assert(io_.current_fiber_ != nullptr && "wait() called outside a fiber");
        if (count_ > 0)
        {
            waiters_.push_back(io_.current_fiber_);
            io_.suspend_fiber(io_.current_fiber_);
        }
    }

private:
    void drain_waiters() noexcept
    {
        while (!waiters_.empty())
        {
            auto* waiting_fiber = waiters_.pop_front();
            io_.push_ready(waiting_fiber);
        }
    }

    IO& io_;
    std::ptrdiff_t count_;
    FiberWaitQueue waiters_;
};

/// @brief A counting gate. Limits how many fibers may be in a critical section concurrently.
struct FiberSemaphore {
    explicit FiberSemaphore(IO& io, const std::ptrdiff_t initial)
        : io_(io), count_(initial) {}

    // Decrements count. Suspends if count == 0.
    void acquire() noexcept
    {
        assert(io_.current_fiber_ != nullptr && "acquire() called outside a fiber");
        if (count_ > 0)
        {
            --count_;
        }
        else
        {
            waiters_.push_back(io_.current_fiber_);
            io_.suspend_fiber(io_.current_fiber_);
        }
    }

    // Increments count. Wakes one waiter if any.
    void release() noexcept
    {
        if (!waiters_.empty())
        {
            auto* waiting_fiber = waiters_.pop_front();
            io_.push_ready(waiting_fiber);
        }
        else
        {
            ++count_;
        }
    }

    // Non-blocking attempt. Returns false instead of suspending.
    [[nodiscard]] bool try_acquire() noexcept
    {
        if (count_ > 0)
        {
            --count_;
            return true;
        }
        return false;
    }

private:
    IO& io_;
    std::ptrdiff_t count_;
    FiberWaitQueue waiters_;
};

/// @brief Mutual exclusion for shared state across fibers
struct FiberMutex {
    explicit FiberMutex(IO& io) : io_(io), held_(false) {}

    void lock() noexcept
    {
        assert(io_.current_fiber_ != nullptr && "lock() called outside a fiber");
        if (!held_)
        {
            held_ = true;
        }
        else
        {
            waiters_.push_back(io_.current_fiber_);
            io_.suspend_fiber(io_.current_fiber_);
        }
    }

    void unlock() noexcept
    {
        if (!waiters_.empty())
        {
            auto* waiting_fiber = waiters_.pop_front();
            io_.push_ready(waiting_fiber);
            // We do not set held_ = false, because ownership transfers directly to the waiting fiber.
        }
        else
        {
            held_ = false;
        }
    }

    [[nodiscard]] bool try_lock() noexcept
    {
        if (!held_)
        {
            held_ = true;
            return true;
        }
        return false;
    }

    // RAII guard
    struct Guard {
        explicit Guard(FiberMutex& m) : m_(m) {}
        ~Guard() { m_.unlock(); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    private:
        FiberMutex& m_;
    };

    [[nodiscard]] Guard scoped_lock() noexcept
    {
        lock();
        return Guard{*this};
    }

private:
    IO& io_;
    bool held_;
    FiberWaitQueue waiters_;
};

/// @brief Ties fiber lifetime to a lexical scope. Ensures that spawned child fibers cannot outlive the group.
struct TaskGroup {
    explicit TaskGroup(IO& io) : io_(io) {}

    // Spawns a child fiber tracked by this group.
    void spawn(std::move_only_function<void(IO&) noexcept> task) noexcept
    {
        auto res = io_.spawn([this, t = std::move(task)](IO& io) mutable noexcept {
            t(io);
            this->on_task_done();
        });
        if (res.has_value())
        {
            ++active_tasks_;
        }
    }

    // Suspends calling fiber until all spawned children have finished.
    void wait() noexcept
    {
        assert(io_.current_fiber_ != nullptr && "wait() called outside a fiber");
        if (active_tasks_ > 0)
        {
            waiters_.push_back(io_.current_fiber_);
            io_.suspend_fiber(io_.current_fiber_);
        }
    }

    ~TaskGroup()
    {
        assert(active_tasks_ == 0 && "TaskGroup destroyed with live tasks; call wait() first");
    }

private:
    void on_task_done() noexcept
    {
        --active_tasks_;
        if (active_tasks_ == 0)
        {
            while (!waiters_.empty())
            {
                auto* waiting_fiber = waiters_.pop_front();
                io_.push_ready(waiting_fiber);
            }
        }
    }

    IO& io_;
    std::ptrdiff_t active_tasks_ = 0;
    FiberWaitQueue waiters_;
};

} // namespace tinyio
