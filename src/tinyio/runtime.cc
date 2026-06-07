#include "tinyio/runtime.hpp"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <format>

#include <sys/eventfd.h>

#include "tinyio/logger.hpp"

namespace tinyio
{
IO::IO(const size_t id, const IoOptions& opts, const std::vector<BucketConfig> pool_configs)
    : id_(id), opts_(opts), buffer_pool_(pool_configs)
{
    if (auto res = initialize(); !res.has_value())
    {
        auto err = res.error();
        ALOG_ERROR("failed to initialize io worker: [{}] {}", err.op, err.error_code().message());
        std::abort();
    }

    ALOG_INFO("initialized io worker {}", id_);
}

IO::~IO() noexcept
{
    // Free any FiberContexts that were enqueued cross-thread but never drained.
    // schedule_fiber() calls ctx.release() before enqueue, so raw ownership
    // lives in the queue until tick() links it into owned fibers.
    // TODO: implement, I am not sure at all

    if (has_live_fibers())
    {
        // TODO: can we have the number of live fibers in O(1) ?
        ALOG_WARN("Destroying IO with live fiber(s); call run_blocking/request_stop or drain the IO first");
    }

    if (ring_.ring_fd > 0 && buffer_pool_.is_registered())
    {
        if (auto res = unregister_buffers(); !res.has_value())
        {
            auto err = res.error();
            ALOG_ERROR("Failed to unregister buffers: [{}] {}", err.op, err.error_code().message());
        }
    }

    if (ring_.ring_fd > 0)
    {
        io_uring_queue_exit(&ring_);
        ring_.ring_fd = -1;
    }

    if (wake_fd_ >= 0)
    {
        ::close(wake_fd_);
        wake_fd_ = -1;
    }
}

Result<void> IO::initialize() noexcept
{
    io_uring_params params{};

    // disable the ring first
    params.flags |= kUringBaseFlags | opts_.additional_uring_flags | IORING_SETUP_R_DISABLED;

    params.cq_entries = opts_.sq_entries;

    // Safely configure SQPOLL if requested
    if (params.flags & IORING_SETUP_SQPOLL)
    {
        params.sq_thread_idle = opts_.sq_thread_idle_ms;
        if (opts_.sq_thread_cpu >= 0)
        {
            params.flags |= IORING_SETUP_SQ_AFF;
            params.sq_thread_cpu = opts_.sq_thread_cpu;
        }
    }

    // io uring requirement, cq_entries * 2
    if (const int rc = io_uring_queue_init_params(opts_.sq_entries * 2, &ring_, &params); rc < 0)
    {
        return fail(-rc, "io_uring_queue_init_params");
    }

    wake_fd_ = eventfd(0, EFD_CLOEXEC);
    if (wake_fd_ < 0)
    {
        io_uring_queue_exit(&ring_);
        return fail(errno, "eventfd");
    }

    // register pool if not empty
    if (buffer_pool_.bucket_count() != 0)
    {
        TRY(register_buffers(buffer_pool_));
        ALOG_INFO("the buffer pool have been registered to io_uring");
    }

    ALOG_INFO("Initialized IO context with {} submission entries (SQPOLL: {})", opts_.sq_entries,
              (opts_.additional_uring_flags & IORING_SETUP_SQPOLL) ? "enabled" : "disabled");

    return {};
}

Result<void> IO::register_buffers(FixedBufferPool& pool) noexcept
{
    if (pool.is_registered())
    {
        return fail(EALREADY, "register_buffers: pool already registered");
    }

    if (const int ret = io_uring_register_buffers(&ring_, pool.iovecs_ptr(), pool.total_capacity()); ret < 0)
    {
        return fail(-ret, "io_uring_register_buffers");
    }

    pool.set_registered();

    return {};
}

Result<void> IO::unregister_buffers() noexcept
{
    if (const int ret = io_uring_unregister_buffers(&ring_); ret < 0)
    {
        return fail(-ret, "io_uring_unregister_buffers");
    }
    return {};
}

//
// Fiber management
//
void IO::push_ready(detail::fiber::Fiber* fiber) noexcept
{
    if (fiber == nullptr || fiber->state == detail::fiber::FiberState::done)
    {
        return;
    }

    // Ask the specific hook if it is already in a list
    auto* hook = static_cast<IntrusiveHook<ReadyQueueTag>*>(fiber);
    if (hook->is_linked())
    {
        return;
    }

    fiber->state = detail::fiber::FiberState::ready;
    ready_queue_.push_back(fiber);
}

void IO::remove_ready(detail::fiber::Fiber* fiber) noexcept
{
    if (fiber != nullptr)
    {
        // We don't even need to check if it's in the queue here!
        // IntrusiveList::remove() already safely checks `is_linked()` internally
        // and does nothing if the fiber isn't in the list.
        ready_queue_.remove(fiber);
    }
}

[[nodiscard]] detail::fiber::Fiber* IO::pop_ready() noexcept
{
    return ready_queue_.pop_front();
}

void IO::destroy_fiber(detail::fiber::Fiber* fiber) noexcept
{
    if (fiber == nullptr)
    {
        return;
    }

    ready_queue_.remove(fiber);
    fiber->task = nullptr;

    detail::tools_destroy_fiber(fiber->tools);

    // Enforce the High-Water Mark!
    if (idle_fibers_.size() >= opts_.max_idle_fibers)
    {
        // only remove from all_fibers_ when we are returning memory to the OS!
        all_fibers_.remove(fiber);

        //  unmap the stack memory to prevent leaks
        detail::stack::release_stack(fiber->stack);
        delete fiber;
    }
    else
    {
        // Cache it for later. It remains in all_fibers_!
        idle_fibers_.push_back(fiber);
    }

    --live_fibers_;
}

void IO::destroy_all_fibers() noexcept
{
    // Iterate the master list. We are guaranteed to catch every reused/idle fiber.
    while (detail::fiber::Fiber* fiber = all_fibers_.pop_front())
    {
        ready_queue_.remove(fiber);
        idle_fibers_.remove(fiber);

        detail::tools_destroy_fiber(fiber->tools);
        detail::stack::release_stack(fiber->stack);
        delete fiber;
    }
}

Result<FiberId> IO::spawn_task(std::move_only_function<void(IO&) noexcept> task) noexcept
{
    if (!is_owner_thread())
    {
        return fail(EPERM, "spawn");
    }

    auto* fiber = idle_fibers_.pop_front();

    if (fiber == nullptr)
    {
        fiber = new (std::nothrow) detail::fiber::Fiber();

        // if fiber is null again
        if (fiber == nullptr)
        {
            return fail(ENOMEM, "spawn");
        }

        const auto stack = TRY(detail::stack::allocate_stack(opts_.stack_size, opts_.guard_size));

        fiber->stack = stack;

        // Only push to all_fibers_ when it's newly allocated
        all_fibers_.push_front(fiber);
    }

    ++live_fibers_;

    fiber->runtime = this;

    //  Reset state  for all fibers
    fiber->id = next_fiber_id_++;  // give every execution a unique ID for logging!
    fiber->task = std::move(task);
    fiber->state = detail::fiber::FiberState::ready;
    detail::tools_create_fiber(fiber->tools);

// Reset CPU registers
#if defined(__x86_64__)
    auto* sp = reinterpret_cast<std::uintptr_t*>(fiber->stack.high);
    *--sp = 0U;
    fiber->context.rsp = reinterpret_cast<std::uint64_t>(sp);
    fiber->context.rip = reinterpret_cast<std::uint64_t>(&tinyio_fiber_start);
    fiber->context.r12 = reinterpret_cast<std::uint64_t>(fiber);
#elif defined(__aarch64__)
    fiber->context.sp = reinterpret_cast<std::uint64_t>(fiber->stack.high);
    fiber->context.pc = reinterpret_cast<std::uint64_t>(&tinyio_fiber_start);
    fiber->context.x19 = reinterpret_cast<std::uint64_t>(fiber);
#else
    #error "unsupported architecture"
#endif

    // push to active list
    push_ready(fiber);

    return fiber->id;
}

void IO::resume_fiber(detail::fiber::Fiber* fiber) noexcept
{
    // Mark this fiber as the currently running one
    current_fiber_ = fiber;

    fiber->state = detail::fiber::FiberState::running;

    detail::tools_before_switch(scheduler_tools_, &fiber->tools, &fiber->stack);
    // Jump into the fiber's execution!
    tinyio_ctx_switch(&scheduler_context_, &fiber->context);
    detail::tools_after_switch(scheduler_tools_);

    // Clear it when we jump back to the scheduler
    current_fiber_ = nullptr;
}

void IO::suspend_fiber(detail::fiber::Fiber* fiber) noexcept
{
    detail::tools_before_switch(fiber->tools, &scheduler_tools_, nullptr);

    tinyio_ctx_switch(&fiber->context, &scheduler_context_);

    detail::tools_after_switch(fiber->tools);
}

[[nodiscard]] Result<void> IO::yield() noexcept
{
    // Check 1: Did the user try to cheat and call this from a different OS thread?
    // Check 2: Did the user call this from main() instead of inside a fiber?
    if (!is_owner_thread() || current_fiber_ == nullptr)
    {
        return fail(EPERM, "yield");
    }

    assert(current_fiber_->runtime == this);
    assert(current_fiber_->state == detail::fiber::FiberState::running);

    push_ready(current_fiber_);
    suspend_fiber(current_fiber_);

    return {};
}

bool IO::has_live_fibers() const noexcept
{
    return live_fibers_ > 0;
}

[[noreturn]] void IO::finish_current_fiber(detail::fiber::Fiber* fiber) noexcept
{
    fiber->state = detail::fiber::FiberState::done;

    detail::tools_before_switch(fiber->tools, &scheduler_tools_, nullptr);
    tinyio_ctx_switch(&fiber->context, &scheduler_context_);

    std::terminate();
}

//
// IO Uring methods
//

extern "C" [[noreturn]] void tinyio_fiber_entry(void* fiber_ptr) noexcept
{
    auto* fiber = static_cast<detail::fiber::Fiber*>(fiber_ptr);
    if (fiber == nullptr || fiber->runtime == nullptr)
    {
        std::terminate();
    }

    auto& rt = *fiber->runtime;

    tinyio::detail::tools_after_switch(rt.scheduler_tools_);
    fiber->task(rt);
    rt.finish_current_fiber(fiber);
}

Result<void> IO::submit_or_wait_for() noexcept
{
    // Only block if we have no local work to do.
    if (io_uring_cq_ready(&ring_) > 0 || !ready_queue_.empty())
    {
        int ret;
        if (opts_.additional_uring_flags & IORING_SETUP_DEFER_TASKRUN)
        {
            ret = io_uring_submit_and_get_events(&ring_);
        }
        else
        {
            ret = io_uring_submit(&ring_);
        }

        if (ret < 0 && ret != -EINTR)
        {
            return fail(-ret, "submit_or_wait_for.io_uring_submit");
        }
    }
    else
    {
        // Thundering herd mitigation
        is_sleeping_.store(true, std::memory_order_seq_cst);

        // Double check all sources of work before sleeping
        if (io_uring_cq_ready(&ring_) == 0 && ready_queue_.empty())
        {
            if (const auto ret = io_uring_submit_and_wait(&ring_, 1); ret < 0 && ret != -EINTR)
            {
                return fail(-ret, "submit_or_wait_for.io_uring_submit_and_wait");
            }
        }

        is_sleeping_.store(false, std::memory_order_relaxed);
    }

    return {};
}

Result<io_uring_sqe*> IO::get_sqe() noexcept
{
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr)
    {
        // SQ is full. Try one submit to clear space.
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
    }

    // its a fatal error if its null again
    if (sqe == nullptr)
    {
        return fail(ENOSPC, "get_sqe");
    }
    return sqe;
}

void IO::wake() const noexcept
{
    // Only write to the eventfd if the thread is actually parked in the kernel
    if (!is_sleeping_.load(std::memory_order_seq_cst))
    {
        return;
    }

    constexpr uint64_t one = 1;
    for (;;)
    {
        const ssize_t n = ::write(wake_fd_, &one, sizeof(one));
        if (n == sizeof(one) || (n == -1 && errno == EAGAIN))
        {
            return;
        }
        if (n == -1 && errno == EINTR)
        {
            continue;
        }
        ALOG_WARN("failed to wake io context with eventfd: {}", std::strerror(errno));
        return;
    }
}

Result<void> IO::arm_wake_read() noexcept
{
    io_uring_sqe* sqe = TRY(get_sqe());
    io_uring_prep_read(sqe, wake_fd_, &wake_value_, sizeof(wake_value_), 0);
    io_uring_sqe_set_data64(sqe, kWakeupSentinel);
    io_uring_submit(&ring_);
    return {};
}

Result<void> IO::activate()
{
    if (is_activated_)
    {
        ALOG_DEBUG("IO is already activated, this is a noop");
        return {};
    }

    if (const int ret = io_uring_register(ring_.ring_fd, IORING_REGISTER_ENABLE_RINGS, nullptr, 0); ret < 0)
    {
        return fail(-ret, "io_uring_register");
    }

    // we need to submit a first read, and this is a good place
    arm_wake_read();
    is_activated_ = true;

    // set thread id
    owner_tid_ = std::this_thread::get_id();

    return {};
}

Result<std::size_t> IO::drain_completions() noexcept
{
    io_uring_cqe* cqe;
    unsigned head;
    unsigned count = 0;

    io_uring_for_each_cqe(&ring_, head, cqe)
    {
        // Grab the raw 64-bit value first
        const uint64_t raw_data = io_uring_cqe_get_data64(cqe);

        if (raw_data == kWakeupSentinel)
        {
            // It's a cross-thread wakeup!
            // If you used an eventfd, you might need to actually read() it here to clear it.
            // If it's just a raw NOP SQE from another thread, we just ignore it
            // because its arrival already did its job (waking us from submit_or_wait_for).
            // Todo: use io uring ring msg, if yes update the comment above
            arm_wake_read();
        }
        else if (raw_data == kFiberCancelSentinel)
        {
            --inflight_ops_;
        }
        else if (raw_data != 0)
        {
            // It's a valid Fiber pointer! Now it's safe to cast.
            auto* op = static_cast<detail::fiber::Operation*>(io_uring_cqe_get_data(cqe));

            op->cqe_res = cqe->res;
            op->cqe_flags = cqe->flags;

            if (op->fiber)
            {
                push_ready(op->fiber);
                --inflight_ops_;
            }
        }

        count++;
    }

    if (count > 0)
    {
        io_uring_cq_advance(&ring_, count);
    }

    return count;
}

Result<void> IO::tick() noexcept
{
    // Flush SQEs and/or wait for CQEs.
    // If ready_queue_ is empty, this safely drops the CPU to 0% and
    // sleeps the thread until the kernel wakes us via a CQE.
    if (auto ret = submit_or_wait_for(); !ret)
    {
        auto err = ret.error();
        ALOG_ERROR("failed to submit and wait for io: [{}] {}", err.op, err.error_code().message());
    }

    auto count = TRY_LOG_ERR(drain_completions(), "current tick run failed to drain completions");

    ALOG_DEBUG("drained {} io completions", count);

    size_t processed = 0;
    while (processed < opts_.batch_max_size)
    {
        auto* fiber = pop_ready();
        if (!fiber)
        {
            break;  // empty queue
        }

        // Jump into the user's code
        resume_fiber(fiber);

        // State Machine Cleanup
        if (fiber->state == detail::fiber::FiberState::done)
        {
            destroy_fiber(fiber);
        }

        processed++;
    }

    return {};
}

//
// ======================= Helpers ==========================================
//
void IO::pin_to_cpu() const
{
    const auto size = opts_.worker_cpu_affinity.size();

    if (empty(opts_.worker_cpu_affinity))
    {
        return;
    }

    int physical_core_id = opts_.worker_cpu_affinity[id_ % size];

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(physical_core_id, &cpuset);

    if (const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset); rc != 0)
    {
        ALOG_INFO("Warning: Failed to pin to physical CPU {}: {}", physical_core_id,
                  std::generic_category().message(rc));
    }
}

void IO::run(const std::stop_token& st) noexcept
{
    is_running_ = true;

    pin_to_cpu();

    activate();

    std::stop_callback wake_on_stop{st, [this] { wake(); }};
    while (!st.stop_requested())
    {
        tick();
    }

    stopping_ = true;
    ALOG_INFO("Worker {} quiescing...", id_);

    // Issue cancels for waiting fibers
    bool issued_cancels = false;
    all_fibers_.for_each(
        [&](detail::fiber::Fiber& fiber) noexcept
        {
            if (fiber.state != detail::fiber::FiberState::waiting)
            {
                return;
            }

            auto res = get_sqe();
            if (!res)
            {
                auto err = res.error();
                ALOG_ERROR("failed to allocate cancel SQE: [{}] {}", err.op, err.error_code().message());
                return;
            }

            auto* sqe = res.value();
            io_uring_prep_cancel(sqe, &fiber.operation, 0);
            io_uring_sqe_set_data64(sqe, kFiberCancelSentinel);
            ++inflight_ops_;
            issued_cancels = true;
        });

    if (issued_cancels)
    {
        io_uring_submit(&ring_);
    }

    // Drain completions and ready fibers
    while (inflight_ops_ > 0 || !ready_queue_.empty())
    {
        tick();
    }

    is_running_ = false;

    if (live_fibers_ > 0)
    {
        ALOG_WARN("Worker {} stopped with {} live fibers (stuck in app logic?)", id_, live_fibers_);
    }
}

void IO::run_until_done() noexcept
{
    is_running_ = true;

    pin_to_cpu();

    if (auto res = activate(); !res)
    {
        const auto err = res.error();
        ALOG_ERROR("failed to activate io worker: [{}] {}", err.op, err.error_code().message());
        is_running_ = false;
        return;
    }

    while (has_live_fibers() || inflight_ops_ > 0 || !ready_queue_.empty())
    {
        if (auto res = tick(); !res)
        {
            const auto err = res.error();
            ALOG_ERROR("io tick failed: [{}] {}", err.op, err.error_code().message());
            break;
        }
    }

    is_running_ = false;
}
//
// ======================= IO Methods ==========================================
//

Result<uint32_t> IO::read(Fd& fd, const std::span<std::byte> buf, const off_t offset) noexcept
{
    return syscall("read", [&](io_uring_sqe* sqe) { io_uring_prep_read(sqe, fd.fd, buf.data(), buf.size(), offset); });
}

Result<uint32_t> IO::read_fixed(Fd& fd, FixedBuffer& buf, size_t len, off_t offset) noexcept
{
    return syscall("read_fixed", [&](io_uring_sqe* sqe)
                   { io_uring_prep_read_fixed(sqe, fd.fd, buf.ptr(), len, offset, buf.index()); });
}

Result<uint32_t> IO::read_fixed(Fd& fd, FixedBuffer& buf, off_t offset) noexcept
{
    return read_fixed(fd, buf, buf.size(), offset);
}

Result<uint32_t> IO::writev(Fd& fd, std::span<const iovec> iovecs, off_t offset) noexcept
{
    return syscall("writev",
                   [&](io_uring_sqe* sqe) { io_uring_prep_writev(sqe, fd.fd, iovecs.data(), iovecs.size(), offset); });
}

Result<void> IO::fsync(Fd& fd, bool datasync) noexcept
{
    return syscall<void>(
        "fsync", [&](io_uring_sqe* sqe) { io_uring_prep_fsync(sqe, fd.fd, datasync ? IORING_FSYNC_DATASYNC : 0); });
}

Result<Fd> IO::open(const std::filesystem::path& path, int flags, mode_t mode) noexcept
{
    return syscall<Fd>("open",
                       [&](io_uring_sqe* sqe) { io_uring_prep_openat(sqe, AT_FDCWD, path.c_str(), flags, mode); });
}

Result<void> IO::close(Fd&& fd) noexcept
{
    // Nullify so Fd destructor doesn't close it twice
    int raw_fd = fd.Release();
    return syscall<void>("close", [&](io_uring_sqe* sqe) { io_uring_prep_close(sqe, raw_fd); });
}

Result<void> IO::fallocate(Fd& fd, int mode, off_t offset, off_t len) noexcept
{
    return syscall<void>("fallocate",
                         [&](io_uring_sqe* sqe) { io_uring_prep_fallocate(sqe, fd.fd, mode, offset, len); });
}

Result<void> IO::ftruncate(Fd& fd, off_t len) noexcept
{
    return syscall<void>("ftruncate", [&](io_uring_sqe* sqe) { io_uring_prep_ftruncate(sqe, fd.fd, len); });
}

Result<Fd> IO::accept(Fd& server_fd, SocketAddress& client_addr, int flags) noexcept
{
    return syscall<Fd>("accept",
                       [&](io_uring_sqe* sqe)
                       {
                           client_addr.addrlen = sizeof(sockaddr_storage);
                           io_uring_prep_accept(sqe, server_fd.fd, client_addr.GetMutable(), &client_addr.addrlen,
                                                flags);
                       });
}

Result<Fd> IO::accept(Fd& server_fd, int flags) noexcept
{
    return syscall<Fd>("accept",
                       [&](io_uring_sqe* sqe) { io_uring_prep_accept(sqe, server_fd.fd, nullptr, nullptr, flags); });
}

Result<uint32_t> IO::write(Fd& fd, std::span<const std::byte> buf, off_t offset) noexcept
{
    return syscall("write",
                   [&](io_uring_sqe* sqe) { io_uring_prep_write(sqe, fd.fd, buf.data(), buf.size(), offset); });
}

Result<uint32_t> IO::write_fixed(Fd& fd, const FixedBuffer& buf, size_t len, off_t offset) noexcept
{
    return syscall("write_fixed", [&](io_uring_sqe* sqe)
                   { io_uring_prep_write_fixed(sqe, fd.fd, buf.ptr(), len, offset, buf.index()); });
}

Result<uint32_t> IO::write_fixed(Fd& fd, const FixedBuffer& buf, off_t offset) noexcept
{
    return write_fixed(fd, buf, buf.size(), offset);
}

Result<uint32_t> IO::readv(Fd& fd, std::span<const iovec> iovecs, off_t offset) noexcept
{
    return syscall("readv",
                   [&](io_uring_sqe* sqe) { io_uring_prep_readv(sqe, fd.fd, iovecs.data(), iovecs.size(), offset); });
}

Result<void> IO::remove(const std::filesystem::path& path) noexcept
{
    return syscall<void>("remove", [&](io_uring_sqe* sqe) { io_uring_prep_unlinkat(sqe, AT_FDCWD, path.c_str(), 0); });
}

Result<void> IO::rename(const std::filesystem::path& from, const std::filesystem::path& to) noexcept
{
    return syscall<void>("rename", [&](io_uring_sqe* sqe)
                         { io_uring_prep_renameat(sqe, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), 0); });
}

Result<uint32_t> IO::poll(Fd& fd, unsigned poll_mask) noexcept
{
    return syscall("poll", [&](io_uring_sqe* sqe) { io_uring_prep_poll_add(sqe, fd.fd, poll_mask); });
}

Result<void> IO::connect(Fd& fd, const SocketAddress& addr) noexcept
{
    return syscall<void>("connect",
                         [&](io_uring_sqe* sqe) { io_uring_prep_connect(sqe, fd.fd, addr.Get(), addr.addrlen); });
}

Result<void> IO::sleep_for_ns(std::chrono::nanoseconds duration) noexcept
{
    if (duration.count() <= 0)
    {
        return {};
    }

    auto r = syscall<void>("sleep_for",
                           [&](io_uring_sqe* sqe)
                           {
                               const auto ns = duration.count();
                               auto& ts = current_fiber_->operation.timeout_ts;
                               ts.tv_sec = ns / 1'000'000'000LL;
                               ts.tv_nsec = ns % 1'000'000'000LL;
                               io_uring_prep_timeout(sqe, &ts, 0U, 0U);
                           });

    if (!r && r.error().code == ETIME)
    {
        return {};  // ETIME is the expected success code for timeout
    }

    return r;
}
}  // namespace tinyio
