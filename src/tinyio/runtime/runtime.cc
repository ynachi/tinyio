#include "tinyio/runtime/runtime.hpp"

#include <pthread.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <stop_token>
#include <system_error>
#include <utility>
#include <vector>

#include "tinyio/logging.hpp"

namespace tinyio {
IO::IO(const size_t id, const IoOptions& opts)
    : id_(id), opts_(opts), post_queue_(opts.post_queue_capacity) {
    if (opts.buffer_pool) {
        buffer_pool_.emplace(opts.buffer_pool->slot_size, opts.buffer_pool->slots);
        buffer_pool_->SetOwner(this);
    }

    if (auto res = Initialize(); !res) {
        // The destructor does not run when a constructor throws: release what
        // Initialize() created before it failed.
        if (wake_fd_ >= 0) {
            ::close(wake_fd_);
            wake_fd_ = -1;
        }
        if (ring_.ring_fd > 0) {
            io_uring_queue_exit(&ring_);
            ring_.ring_fd = -1;
        }
        const auto err = res.error();
        throw std::system_error(err.code,
                                std::format("tinyio::IO init failed in {}", err.Context()));
    }
}

IO::~IO() noexcept {
    const bool was_stopping = stopping_.exchange(true, std::memory_order_release);

    // Discard any tasks that were posted but not yet spawned.
    // Their destructors release any captured resources before we tear down the ring.
    {
        ELOGFMT(INFO, "discarding ready tasks, worker {}", id_);
        std::move_only_function<void(IO&) noexcept> fn;
        while (post_queue_.TryDequeue(fn)) {
        }
    }

    // Quiesce in-flight work before releasing kernel resources.
    //
    // If the ring was activated and there is still live work (fibers waiting
    // on io_uring CQEs, ready fibers not yet resumed), we must cancel and drain
    // here so the kernel does not reference freed Operation* pointers after
    // io_uring_queue_exit().
    //
    // This MUST run on the owner thread: IORING_SETUP_SINGLE_ISSUER forbids
    // cross-thread submission.  Destroying the IO from another thread while
    // the ring has live work is a kernel UAF and causes abort.
    const bool has_live_work = live_fibers_ > 0 || inflight_ops_ > 0 || !ready_queue_.Empty();

    if (is_activated_ && !was_stopping && has_live_work) {
        if (!IsOwnerThread()) {
            ELOGFMT(ERROR,
                    "IO #{} destroyed from a non-owner thread with {} live fiber(s) "
                    "and {} op(s) in-flight — aborting to prevent kernel UAF",
                    id_, live_fibers_, inflight_ops_);
            std::abort();
        }

        ELOGFMT(WARN, "IO #{} destroyed with {} live fiber(s); issuing cancels and draining", id_,
                live_fibers_);
        CancelAndDrain();
    }

    // Only a runtime that never ran gets here with live fibers (spawned, never
    // started): a drain aborts on fibers that are still parked.
    if (live_fibers_ > 0) {
        ELOGFMT(WARN, "IO #{} teardown: {} fiber(s) never ran", id_, live_fibers_);
    }

    // The constructor throws when setup fails, so the ring and the eventfd
    // always exist here.
    io_uring_queue_exit(&ring_);
    ELOGFMT(INFO, "exited io-uring ring, worker {}", id_);
    {
        // Take the same lock Wake() uses: a poster may still be inside write().
        std::lock_guard<std::mutex> lock(wake_mutex_);
        ::close(wake_fd_);
        wake_fd_ = -1;
    }

    // Detach the pool before the fibers go away: a fiber parked on a slot
    // points at memory that is about to be freed, so the pool must not try to
    // unpark it afterwards.
    if (buffer_pool_) {
        buffer_pool_->Detach();
    }

    // Release all fiber stacks and Fiber objects (both live and idle).
    DestroyAllFibers();
}

Result<void> IO::Initialize() noexcept {
    io_uring_params params{};

    owner_tid_ = std::this_thread::get_id();

    // disable the ring first (kUringBaseFlags already includes IORING_SETUP_R_DISABLED)
    params.flags |= k_uring_base_flags | opts_.additional_uring_flags;

    params.cq_entries = opts_.sq_entries * 2;

    // Safely configure SQPOLL if requested
    if (params.flags & IORING_SETUP_SQPOLL) {
        params.sq_thread_idle = opts_.sq_thread_idle_ms;
        if (opts_.sq_thread_cpu >= 0) {
            params.flags |= IORING_SETUP_SQ_AFF;
            params.sq_thread_cpu = static_cast<std::uint32_t>(opts_.sq_thread_cpu);
        }
    }

    // The kernel rejects this pair with a bare EINVAL; say why instead.
    if ((params.flags & IORING_SETUP_SQPOLL) && (params.flags & IORING_SETUP_DEFER_TASKRUN)) {
        return Error::FailErrno(EINVAL,
                                "IORING_SETUP_SQPOLL is incompatible with DEFER_TASKRUN; "
                                "clear kUringDefaultAdditionalFlag bits when enabling SQPOLL");
    }

    if (const int rc = io_uring_queue_init_params(opts_.sq_entries, &ring_, &params); rc < 0) {
        return Error::FailErrno(-rc, "io_uring_queue_init_params");
    }

    wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_fd_ < 0) {
        return Error::FailErrno(errno, "eventfd");
    }

    // One iovec per <= 1 GiB chunk, not per slot: the kernel's registered
    // buffer table is far smaller than the number of slots a pool holds. The
    // kernel pins the pages, which counts against RLIMIT_MEMLOCK unless the
    // process has CAP_IPC_LOCK.
    if (buffer_pool_) {
        std::vector<iovec> table;
        buffer_pool_->AppendRegions(table);
        if (const int ret = io_uring_register_buffers(&ring_, table.data(),
                                                      static_cast<unsigned int>(table.size()));
            ret < 0) {
            ELOGFMT(ERROR, "io_uring_register_buffers failed: {} ({} regions, {} bytes pinned){}",
                    std::strerror(-ret), table.size(), buffer_pool_->AllocatedBytes(),
                    ret == -ENOMEM ? "; check `ulimit -l` (RLIMIT_MEMLOCK) or LimitMEMLOCK= "
                                     "in the unit file"
                                   : "");
            return Error::FailErrno(-ret, "io_uring_register_buffers");
        }
        ELOGFMT(INFO, "registered a buffer pool of {} x {} bytes", buffer_pool_->Capacity(),
                buffer_pool_->SlotSize());
    }

    ELOGFMT(INFO, "Initialized IO context with {} submission entries (SQPOLL: {})",
            opts_.sq_entries,
            (opts_.additional_uring_flags & IORING_SETUP_SQPOLL) ? "enabled" : "disabled");

    return {};
}

//
// Shutdown
//

void IO::CancelAndDrain() noexcept {
    // Cancel every in-flight request with ONE SQE.
    //
    // IORING_ASYNC_CANCEL_ALL | IORING_ASYNC_CANCEL_ANY matches every request
    // in the ring, whatever its user_data: at this point every request in the
    // ring belongs to the runtime, and every one of them has to die. The
    // wake-read is in the ring too; DrainCompletions() re-arms it, so
    // cancelling it costs one extra completion and nothing else.
    //
    // Sync-parked fibers are never woken by the runtime. They might be waiting
    // for the IO fibers to finish (e.g., TaskGroup::Wait, Latch::Wait), and
    // waking them early would let them destroy synchronization primitives
    // while the IO fibers are still unwinding, causing Use-After-Free.
    if (inflight_ops_ > 0) {
        if (auto sqe = GetSqe(); sqe) {
            io_uring_prep_cancel64(*sqe, 0, IORING_ASYNC_CANCEL_ALL | IORING_ASYNC_CANCEL_ANY);
            io_uring_sqe_set_data64(*sqe, k_fiber_cancel_sentinel);
            ++inflight_ops_;
            io_uring_submit(&ring_);
        } else {
            ELOGFMT(ERROR, "cancel SQE alloc failed: [{}] {}", sqe.error().Context(),
                    sqe.error().Message());
        }
    }

    // The cancelled IO fibers wake with ECANCELED and unwind, which lets them
    // wake their sync-parked dependents naturally.
    DriveToQuiescence("cancel_and_drain");
    AbortIfParkedFibers();
}

void IO::DriveToQuiescence(const char* who) noexcept {
    // This loop decides when to stop, so a tick must not sleep once the
    // runtime is quiesced: it would wait for a completion that can never come.
    drain_to_completion_ = true;
    int consecutive_errors = 0;
    while (inflight_ops_ > 0 || !ready_queue_.Empty() || !post_queue_.Empty()) {
        // Giving up must not return: operations would still be in flight when
        // the ring is torn down, and the kernel would then reference Operation
        // objects that live inside the fibers about to be freed.
        if (!TickCounted(consecutive_errors, who)) {
            ELOGFMT(FATAL,
                    "IO #{} {}: {} operation(s) still in flight after {} failed ticks; "
                    "tearing the ring down now would be a kernel use-after-free",
                    id_, who, inflight_ops_, consecutive_errors);
            std::abort();
        }
    }
    drain_to_completion_ = false;
}

void IO::AbortIfParkedFibers() const noexcept {
    // Called once quiesced: nothing ready, nothing in flight. A fiber still
    // live is parked on a sync primitive or on the buffer pool, and nothing
    // can ever wake it.
    if (live_fibers_ == 0) {
        return;
    }

    // Name the culprit: every parked fiber records what it is waiting on, so
    // the report says "1 on Mutex (fiber 4)" instead of a count plus guesses.
    // Grouped by WaitKind, in enum order.
    constexpr auto k_kinds = static_cast<size_t>(detail::fiber::WaitKind::buffer_pool) + 1;
    std::array<std::string, k_kinds> ids;
    std::array<size_t, k_kinds> counts{};
    for (const detail::fiber::Fiber* fiber : all_fibers_) {
        if (fiber->state != detail::fiber::FiberState::waiting) {
            continue;
        }
        const auto kind = static_cast<size_t>(fiber->wait_kind);
        ids[kind] += (counts[kind]++ == 0 ? "" : ", ") + std::to_string(fiber->id);
    }

    std::string report;
    for (size_t kind = 0; kind < k_kinds; ++kind) {
        if (counts[kind] == 0) {
            continue;
        }
        report +=
            std::format("{}{} on {} (fiber{} {})", report.empty() ? "" : ", ", counts[kind],
                        detail::fiber::WaitKindName(static_cast<detail::fiber::WaitKind>(kind)),
                        counts[kind] == 1 ? "" : "s", ids[kind]);
    }

    ELOGFMT(FATAL, "IO #{} quiesced with {} parked fiber(s) and no one left to wake them: {}", id_,
            live_fibers_, report.empty() ? "unknown" : report);
    std::abort();
}

//
// Running
//

Result<void> IO::Activate() noexcept {
    // The thread that runs the loop owns the ring, whichever thread built it.
    owner_tid_ = std::this_thread::get_id();
    PinToCpu();

    if (is_activated_) {
        return {};
    }

    // Register the scheduler (this OS thread) with the sanitizer runtimes
    // BEFORE the first context switch. Under TSAN, every fiber-to-scheduler
    // switch targets scheduler_tools_.tsan_fiber; if it was never captured,
    // tools_before_switch() silently skips __tsan_switch_to_fiber and TSAN's
    // shadow state diverges from reality on the first suspend - SEGV.
    detail::ToolsCaptureCurrentThread(scheduler_tools_);

    if (const int ret =
            io_uring_register(static_cast<unsigned int>(ring_.ring_fd),
                              static_cast<unsigned int>(IORING_REGISTER_ENABLE_RINGS), nullptr, 0);
        ret < 0) {
        return Error::FailErrno(-ret, "io_uring_register");
    }

    // The eventfd read that lets Wake() interrupt a sleeping loop.
    if (auto res = ArmWakeRead(); !res) {
        ELOGFMT(WARN, "failed to arm eventfd read: [{}] {}", res.error().Context(),
                res.error().Message());
    }
    is_activated_ = true;

    return {};
}

void IO::PinToCpu() const {
    const auto& cpus = opts_.worker_cpu_affinity;
    if (cpus.empty()) {
        return;
    }

    const int cpu = cpus[id_ % cpus.size()];
    if (cpu < 0) {
        return;
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(static_cast<unsigned int>(cpu), &cpuset);

    if (const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
        rc != 0) {
        ELOGFMT(INFO, "Warning: Failed to pin to physical CPU {}: {}", cpu,
                std::generic_category().message(rc));
    }
}

void IO::Run(const std::stop_token& st) noexcept {
    if (auto res = Activate(); !res) {
        ELOGFMT(ERROR, "failed to activate io uring: [{}] {}", res.error().Context(),
                res.error().Message());
        return;
    }

    std::stop_callback wake_on_stop{st, [this] { Wake(); }};
    // A tick that keeps failing must not spin the loop forever: stop after a
    // run of consecutive failures and let the drain below empty the ring.
    int consecutive_errors = 0;
    while (!st.stop_requested()) {
        if (!TickCounted(consecutive_errors, "run")) {
            ELOGFMT(ERROR, "{} consecutive tick failures; stopping the loop to drain",
                    consecutive_errors);
            break;
        }
    }

    stopping_.store(true, std::memory_order_release);
    ELOGFMT(INFO, "Worker {} quiescing...", id_);
    CancelAndDrain();
}

void IO::RunUntilDone() noexcept {
    if (auto res = Activate(); !res) {
        ELOGFMT(ERROR, "failed to activate io worker: [{}] {}", res.error().Context(),
                res.error().Message());
        return;
    }

    // Run until quiesced: nothing ready, nothing in flight, nothing posted.
    // inflight_ops_ counts I/O ops and cancel SQEs only (the wake_fd_ read is
    // not counted).
    DriveToQuiescence("run_until_done");
    AbortIfParkedFibers();
}

}  // namespace tinyio
