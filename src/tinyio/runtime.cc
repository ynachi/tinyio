#include "tinyio/runtime.hpp"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <format>

#include <sys/eventfd.h>

namespace tinyio
{
Result<void> Runtime::initialize() noexcept
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

Result<void> Runtime::register_buffers(FixedBufferPool& pool) noexcept
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

//
// Fiber management
//
void Runtime::push_ready(detail::fiber::Fiber* fiber) noexcept
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

void Runtime::remove_ready(detail::fiber::Fiber* fiber) noexcept
{
    if (fiber != nullptr)
    {
        // We don't even need to check if it's in the queue here!
        // IntrusiveList::remove() already safely checks `is_linked()` internally
        // and does nothing if the fiber isn't in the list.
        ready_queue_.remove(fiber);
    }
}

[[nodiscard]] detail::fiber::Fiber* Runtime::pop_ready() noexcept
{
    // pop_front() automatically resets the hook to an unlinked state!
    return ready_queue_.pop_front();
}

void Runtime::destroy_fiber(detail::fiber::Fiber* fiber) noexcept
{
    if (fiber == nullptr)
    {
        return;
    }

    ready_queue_.remove(fiber);
    all_fibers_.remove(fiber);
    fiber->task = nullptr;  // Clean user memory

    // Enforce the High-Water Mark!
    if (idle_fibers_.size() >= opts_.max_idle_fibers)
    {
        all_fibers_.remove(fiber);
        // The pool is full, return memory to the OS
        delete fiber;
    }
    else
    {
        // Cache it for later
        idle_fibers_.push_back(fiber);
    }

    --live_fibers_;
}

void Runtime::destroy_all_fibers() noexcept
{
    // Clean up active fibers
    while (detail::fiber::Fiber* fiber = all_fibers_.pop_front())
    {
        ready_queue_.remove(fiber);
        delete fiber;
    }

    // Clean up cached/idle fibers
    while (const detail::fiber::Fiber* fiber = idle_fibers_.pop_front())
    {
        delete fiber;
    }
}

Result<FiberId> Runtime::spawn_task(std::move_only_function<void(Runtime&) noexcept> task) noexcept
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

void Runtime::resume_fiber(detail::fiber::Fiber* fiber) noexcept
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

void Runtime::suspend_fiber(detail::fiber::Fiber* fiber) noexcept
{
    detail::tools_before_switch(fiber->tools, &scheduler_tools_, nullptr);

    tinyio_ctx_switch(&fiber->context, &scheduler_context_);

    detail::tools_after_switch(fiber->tools);
}

[[nodiscard]] Result<void> Runtime::yield() noexcept
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

void Runtime::tick() noexcept
{
    if (auto ret = submit_or_wait_for(); !ret)
    {
        // Log/handle failure if needed
    }

    io_uring_cqe* cqe;
    unsigned head;
    unsigned count = 0;

    io_uring_for_each_cqe(&ring_, head, cqe)
    {
        if (void* ptr = io_uring_cqe_get_data(cqe))
        {
            auto* op = static_cast<detail::fiber::Operation*>(ptr);
            
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

    size_t processed = 0;
    const size_t max_batch = 128; // To prevent networking starvation
    
    while (processed < max_batch)
    {
        auto* fiber = pop_ready();
        if (!fiber)
        {
            break;
        }

        resume_fiber(fiber);

        if (fiber->state == detail::fiber::FiberState::done)
        {
            destroy_fiber(fiber);
        }

        processed++;
    }
}

void Runtime::run() noexcept
{
    is_running_ = true;
    inside_run_ = true;
    owner_tid_ = std::this_thread::get_id();

    while (inside_run_ && has_live_fibers())
    {
        tick();
    }

    is_running_ = false;
    inside_run_ = false;
}

bool Runtime::has_live_fibers() const noexcept
{
    return live_fibers_ > 0;
}

[[noreturn]] void Runtime::finish_current_fiber(detail::fiber::Fiber* fiber) noexcept
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

Result<void> Runtime::submit_or_wait_for() noexcept
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

Result<io_uring_sqe*> Runtime::get_sqe() noexcept
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

void Runtime::wake() const noexcept
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

Result<void> Runtime::arm_wake_read() noexcept
{
    io_uring_sqe* sqe = TRY(get_sqe());
    io_uring_prep_read(sqe, wake_fd_, &wake_value_, sizeof(wake_value_), 0);
    io_uring_sqe_set_data64(sqe, kWakeupSentinel);
    io_uring_submit(&ring_);
    return {};
}

Result<void> Runtime::activate()
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

    return {};
}

Result<std::size_t> Runtime::drain_completions() noexcept
{
    io_uring_cqe* cqe;
    unsigned head;
    unsigned count = 0;

    io_uring_for_each_cqe(&ring_, head, cqe)
    {
        // If user_data was 0, ptr will be nullptr and the if() fails.
        if (void* ptr = io_uring_cqe_get_data(cqe); ptr != nullptr)
        {
            auto* op = static_cast<detail::fiber::Operation*>(ptr);

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

    //
    // ======================= IO Methods ==========================================
    //

    Result<uint32_t> Runtime::read(Fd & fd, const std::span<std::byte> buf, const off_t offset) noexcept
    {
        return syscall("read",
                       [&](io_uring_sqe* sqe) { io_uring_prep_read(sqe, fd.fd, buf.data(), buf.size(), offset); });
    }

    Result<uint32_t> Runtime::read_fixed(Fd & fd, FixedBuffer & buf, size_t len, off_t offset) noexcept
    {
        return syscall("read_fixed", [&](io_uring_sqe* sqe)
                       { io_uring_prep_read_fixed(sqe, fd.fd, buf.ptr(), len, offset, buf.index()); });
    }

    Result<uint32_t> Runtime::read_fixed(Fd & fd, FixedBuffer & buf, off_t offset) noexcept
    {
        return read_fixed(fd, buf, buf.size(), offset);
    }

    Result<uint32_t> Runtime::writev(Fd & fd, std::span<const iovec> iovecs, off_t offset) noexcept
    {
        return syscall("writev", [&](io_uring_sqe* sqe)
                       { io_uring_prep_writev(sqe, fd.fd, iovecs.data(), iovecs.size(), offset); });
    }

    Result<void> Runtime::fsync(Fd & fd, bool datasync) noexcept
    {
        return syscall<void>(
            "fsync", [&](io_uring_sqe* sqe) { io_uring_prep_fsync(sqe, fd.fd, datasync ? IORING_FSYNC_DATASYNC : 0); });
    }

    Result<Fd> Runtime::open(const std::filesystem::path& path, int flags, mode_t mode) noexcept
    {
        return syscall<Fd>("open",
                           [&](io_uring_sqe* sqe) { io_uring_prep_openat(sqe, AT_FDCWD, path.c_str(), flags, mode); });
    }

    Result<void> Runtime::close(Fd && fd) noexcept
    {
        // Nullify so Fd destructor doesn't close it twice
        int raw_fd = fd.Release();
        return syscall<void>("close", [&](io_uring_sqe* sqe) { io_uring_prep_close(sqe, raw_fd); });
    }

    Result<void> Runtime::fallocate(Fd & fd, int mode, off_t offset, off_t len) noexcept
    {
        return syscall<void>("fallocate",
                             [&](io_uring_sqe* sqe) { io_uring_prep_fallocate(sqe, fd.fd, mode, offset, len); });
    }

    Result<void> Runtime::ftruncate(Fd & fd, off_t len) noexcept
    {
        return syscall<void>("ftruncate", [&](io_uring_sqe* sqe) { io_uring_prep_ftruncate(sqe, fd.fd, len); });
    }

    Result<Fd> Runtime::accept(Fd & server_fd, SocketAddress & client_addr, int flags) noexcept
    {
        return syscall<Fd>("accept",
                           [&](io_uring_sqe* sqe)
                           {
                               client_addr.addrlen = sizeof(sockaddr_storage);
                               io_uring_prep_accept(sqe, server_fd.fd, client_addr.GetMutable(), &client_addr.addrlen,
                                                    flags);
                           });
    }

    Result<Fd> Runtime::accept(Fd & server_fd, int flags) noexcept
    {
        return syscall<Fd>(
            "accept", [&](io_uring_sqe* sqe) { io_uring_prep_accept(sqe, server_fd.fd, nullptr, nullptr, flags); });
    }

    Result<uint32_t> Runtime::write(Fd & fd, std::span<const std::byte> buf, off_t offset) noexcept
    {
        return syscall("write",
                       [&](io_uring_sqe* sqe) { io_uring_prep_write(sqe, fd.fd, buf.data(), buf.size(), offset); });
    }

    Result<uint32_t> Runtime::write_fixed(Fd & fd, const FixedBuffer& buf, size_t len, off_t offset) noexcept
    {
        return syscall("write_fixed", [&](io_uring_sqe* sqe)
                       { io_uring_prep_write_fixed(sqe, fd.fd, buf.ptr(), len, offset, buf.index()); });
    }

    Result<uint32_t> Runtime::write_fixed(Fd & fd, const FixedBuffer& buf, off_t offset) noexcept
    {
        return write_fixed(fd, buf, buf.size(), offset);
    }

    Result<uint32_t> Runtime::readv(Fd & fd, std::span<const iovec> iovecs, off_t offset) noexcept
    {
        return syscall(
            "readv", [&](io_uring_sqe* sqe) { io_uring_prep_readv(sqe, fd.fd, iovecs.data(), iovecs.size(), offset); });
    }

    Result<void> Runtime::remove(const std::filesystem::path& path) noexcept
    {
        return syscall<void>("remove",
                             [&](io_uring_sqe* sqe) { io_uring_prep_unlinkat(sqe, AT_FDCWD, path.c_str(), 0); });
    }

    Result<void> Runtime::rename(const std::filesystem::path& from, const std::filesystem::path& to) noexcept
    {
        return syscall<void>("rename", [&](io_uring_sqe* sqe)
                             { io_uring_prep_renameat(sqe, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), 0); });
    }

    Result<uint32_t> Runtime::poll(Fd & fd, unsigned poll_mask) noexcept
    {
        return syscall("poll", [&](io_uring_sqe* sqe) { io_uring_prep_poll_add(sqe, fd.fd, poll_mask); });
    }

    Result<void> Runtime::connect(Fd & fd, const SocketAddress& addr) noexcept
    {
        return syscall<void>("connect",
                             [&](io_uring_sqe* sqe) { io_uring_prep_connect(sqe, fd.fd, addr.Get(), addr.addrlen); });
    }

    Result<void> Runtime::sleep_for_ns(std::chrono::nanoseconds duration) noexcept
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