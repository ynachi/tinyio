#include <cassert>
#include <cerrno>
#include <exception>
#include <memory>
#include <new>
#include <utility>

#include "tinyio/runtime/runtime.hpp"

namespace tinyio {
//
// Fiber management
//
void IO::PushReady(detail::fiber::Fiber* fiber) noexcept {
    assert(fiber != nullptr && fiber->state != detail::fiber::FiberState::done);
    // Ready means "in the ready queue": a fiber is queued at most once, however
    // many wakeups it got.
    if (fiber->state == detail::fiber::FiberState::ready) {
        return;
    }
    fiber->state = detail::fiber::FiberState::ready;
    ready_queue_.Push(fiber);
}

detail::fiber::Fiber* IO::PopReady() noexcept {
    detail::fiber::Fiber* fiber = nullptr;
    (void)ready_queue_.Pop(fiber);
    return fiber;
}

void IO::RemoveFromAllFibers(detail::fiber::Fiber* fiber) noexcept {
    // Swap-and-pop: move the last fiber into the hole.
    detail::fiber::Fiber* last = all_fibers_.back();
    all_fibers_[fiber->all_fibers_index] = last;
    last->all_fibers_index = fiber->all_fibers_index;
    all_fibers_.pop_back();
}

void IO::DestroyFiber(detail::fiber::Fiber* fiber) noexcept {
    // A fiber finishes while it runs: it is out of the ready queue, and its
    // last operation completed, which took it out of the timer wheel.
    assert(fiber->state == detail::fiber::FiberState::done && !fiber->timer_armed);
    fiber->task = nullptr;

    detail::ToolsDestroyFiber(fiber->tools);

    if (idle_fibers_.size() >= opts_.max_idle_fibers) {
        // Over the cache limit: give the stack back to the OS.
        RemoveFromAllFibers(fiber);
        detail::stack::ReleaseStack(fiber->stack);
        delete fiber;
    } else {
        // Keep it, stack included, for the next spawn. It stays in all_fibers_.
        idle_fibers_.push_back(fiber);
    }

    --live_fibers_;
}

void IO::DestroyAllFibers() noexcept {
    // all_fibers_ owns every fiber, idle ones included, so the other
    // containers only need to forget their pointers.
    for (detail::fiber::Fiber* fiber : all_fibers_) {
        detail::ToolsDestroyFiber(fiber->tools);
        detail::stack::ReleaseStack(fiber->stack);
        delete fiber;
    }
    all_fibers_.clear();
    idle_fibers_.clear();
    ready_queue_.Clear();
}

Result<FiberId> IO::SpawnTask(std::move_only_function<void(IO&) noexcept> task) noexcept {
    if (!IsOwnerThread()) {
        return Error::FailErrno(EPERM, "spawn");
    }

    detail::fiber::Fiber* fiber = nullptr;
    if (!idle_fibers_.empty()) {
        fiber = idle_fibers_.back();
        idle_fibers_.pop_back();
    } else {
        // unique_ptr frees the Fiber if the stack allocation fails, and the
        // fiber joins all_fibers_ only once both allocations succeeded.
        std::unique_ptr<detail::fiber::Fiber> owned(new (std::nothrow) detail::fiber::Fiber());
        if (!owned) {
            return Error::FailErrno(ENOMEM, "spawn");
        }
        TRY(const stack, detail::stack::AllocateStack(opts_.stack_size, opts_.guard_size));
        owned->runtime = this;
        owned->stack = stack;
        owned->all_fibers_index = all_fibers_.size();
        all_fibers_.push_back(owned.get());
        fiber = owned.release();
    }

    ++live_fibers_;

    // Every execution gets a fresh id, a recycled fiber included.
    fiber->id = next_fiber_id_++;
    fiber->task = std::move(task);
    detail::ToolsCreateFiber(fiber->tools, fiber->stack);
    detail::context::Make(fiber->context, fiber->stack.high, &IO::FiberEntry, fiber);

    // A new or recycled fiber is in no queue yet.
    fiber->state = detail::fiber::FiberState::ready;
    ready_queue_.Push(fiber);

    return fiber->id;
}

TIO_NO_SANITIZE_ADDRESS void IO::ResumeFiber(detail::fiber::Fiber* fiber) noexcept {
    current_fiber_ = fiber;
    fiber->state = detail::fiber::FiberState::running;

    detail::ToolsBeforeSwitch(scheduler_tools_, &fiber->tools, &fiber->stack);
    detail::context::Switch(scheduler_context_, fiber->context);
    detail::ToolsAfterSwitch(scheduler_tools_);

    // Back on the scheduler.
    current_fiber_ = nullptr;
}

TIO_NO_SANITIZE_ADDRESS void IO::SuspendFiber(detail::fiber::Fiber* fiber) noexcept {
    detail::ToolsBeforeSwitch(fiber->tools, &scheduler_tools_, nullptr);
    detail::context::Switch(fiber->context, scheduler_context_);
    detail::ToolsAfterSwitch(fiber->tools, &scheduler_tools_);
}

[[nodiscard]] Result<void> IO::Yield() noexcept {
    CheckCanSubmit("yield");

    if (stopping_) {
        return Error::FailErrno(ECANCELED, "yield");
    }

    PushReady(current_fiber_);
    SuspendFiber(current_fiber_);

    return {};
}

bool IO::HasLiveFibers() const noexcept { return live_fibers_ > 0; }

[[noreturn]] TIO_NO_SANITIZE_ADDRESS void IO::FinishCurrentFiber(
    detail::fiber::Fiber* fiber) noexcept {
    fiber->state = detail::fiber::FiberState::done;

    detail::ToolsBeforeSwitch(fiber->tools, &scheduler_tools_, nullptr, true);
    // Last switch out. The stack pointer saved into fiber->context is never
    // resumed: a recycled fiber gets a fresh frame from context::Make().
    detail::context::Switch(fiber->context, scheduler_context_);

    std::terminate();
}

// First code a new fiber runs, on its own stack, reached through the
// trampoline in context.cc. ResumeFiber() saved the scheduler's context in
// the switch that got us here, so there is nothing to record.
[[noreturn]] void IO::FiberEntry(void* arg) noexcept {
    auto* fiber = static_cast<detail::fiber::Fiber*>(arg);
    assert(fiber != nullptr && fiber->runtime != nullptr);

    // Complete the ASAN fiber switch ResumeFiber() started and capture the
    // scheduler's stack bounds on the very first switch out of it. A brand-new
    // fiber has no fake stack to restore: fiber->tools.asan_fake_stack is still
    // the nullptr ToolsCreateFiber() set.
    detail::ToolsAfterSwitch(fiber->tools, &fiber->runtime->scheduler_tools_);
    fiber->task(*fiber->runtime);
    fiber->runtime->FinishCurrentFiber(fiber);
}

}  // namespace tinyio
