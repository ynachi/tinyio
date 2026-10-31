#include <algorithm>
#include <cerrno>

#include "tinyio/runtime/runtime.hpp"

namespace tinyio {
const FixedBufferPool* IO::BufferPool(const size_t slot_size) const noexcept {
    return FindPool(slot_size);
}

FixedBufferPool* IO::FindPool(const size_t slot_size) noexcept {
    for (auto& pool : buffer_pools_) {
        if (pool->SlotSize() == slot_size) {
            return pool.get();
        }
    }
    return nullptr;
}

const FixedBufferPool* IO::FindPool(const size_t slot_size) const noexcept {
    for (const auto& pool : buffer_pools_) {
        if (pool->SlotSize() == slot_size) {
            return pool.get();
        }
    }
    return nullptr;
}

bool IO::OwnsPool(const FixedBufferPool* pool) const noexcept {
    for (const auto& owned : buffer_pools_) {
        if (owned.get() == pool) {
            return true;
        }
    }
    return false;
}

Result<FixedBuffer> IO::TakeFixedBuffer(const size_t slot_size) noexcept {
    CHECK(!buffer_pools_.empty())
        << "take_fixed_buffer: no buffer pool configured (IoOptions::buffer_pools)";
    // No fallback: a slot comes from the class of that exact size, or not at
    // all. A full class never borrows from another one.
    FixedBufferPool* pool = FindPool(slot_size);
    if (pool == nullptr) {
        return Error::FailErrno(ENOENT, "take_fixed_buffer: no buffer class of that size");
    }
    return pool->Take();
}

Result<FixedBuffer> IO::AcquireFixedBuffer(const size_t slot_size) noexcept {
    CHECK(!buffer_pools_.empty())
        << "acquire_fixed_buffer: no buffer pool configured (IoOptions::buffer_pools)";
    // No fallback: the fiber waits for a slot of that exact size. A full
    // class never borrows from another one.
    FixedBufferPool* pool = FindPool(slot_size);
    if (pool == nullptr) {
        return Error::FailErrno(ENOENT, "acquire_fixed_buffer: no buffer class of that size");
    }
    return pool->Acquire(*this);
}

Result<unsigned int> IO::FixedRange(const FixedBuffer& buf, const size_t len,
                                    const size_t buffer_offset,
                                    const char* op_name) const noexcept {
    // The slot has to come from one of this runtime's pools: the kernel
    // resolves the buf_index against the table registered with this ring, and
    // a foreign index would name the wrong buffer.
    CHECK(OwnsPool(buf.Pool())) << op_name << ": the buffer is not registered with this runtime";
    // The buffer offset has to land inside the slot, and the length must not
    // be zero: either is an EINVAL. A length that runs past the slot is
    // clamped to what is left of it, so the kernel never touches the next
    // slot.
    if (buffer_offset >= buf.Size() || len == 0) {
        return Error::FailErrno(EINVAL, op_name);
    }
    return static_cast<unsigned int>(std::min({len, buf.Size() - buffer_offset, MAX_RW_LEN}));
}

Result<unsigned int> IO::FixedRegionRange(const FixedRegion& region,
                                          const char* op_name) const noexcept {
    // Same ownership rule as FixedRange(): the region names a pool, and only
    // a pool of this runtime is registered with this ring.
    CHECK(OwnsPool(region.Pool())) << op_name << ": the buffer is not registered with this runtime";
    if (region.Size() == 0) {
        return Error::FailErrno(EINVAL, op_name);
    }
    return static_cast<unsigned int>(std::min(region.Size(), MAX_RW_LEN));
}

Result<void> IO::Close(const int fd) noexcept {
    return Syscall<void>("close", [&](io_uring_sqe* sqe) { io_uring_prep_close(sqe, fd); });
}

Result<void> IO::Close(UniqueFd fd) noexcept {
    if (!fd.Valid()) {
        return {};
    }

    return Syscall<void>("close",
                         [&](io_uring_sqe* sqe) { io_uring_prep_close(sqe, fd.Release()); });
}

//
// ======================= IO Methods ==========================================
//

Result<uint32_t> IO::Read(const UniqueFd& fd, const std::span<std::byte> buf, const off_t offset,
                          const std::chrono::nanoseconds timeout) noexcept {
    if (offset < -1) {
        return Error::FailErrno(EINVAL, "read");
    }
    // A span past the kernel's per-op cap would truncate through the unsigned
    // SQE length; clamp so the caller gets a short read instead of a wrong one.
    const auto len = static_cast<unsigned int>(std::min(buf.size(), MAX_RW_LEN));
    return Syscall(
        "read",
        [&](io_uring_sqe* sqe) {
            io_uring_prep_read(sqe, fd.Get(), buf.data(), len, static_cast<std::uint64_t>(offset));
        },
        timeout);
}

Result<void> IO::ReadExact(const UniqueFd& fd, std::span<std::byte> buf, off_t offset,
                           const std::chrono::nanoseconds timeout) noexcept {
    if (offset < -1) {
        return Error::FailErrno(EINVAL, "read_exact");
    }
    while (!buf.empty()) {
        TRY(const n, Read(fd, buf, offset, timeout));
        if (n == 0) {
            return Error::FailErrno(EIO, "read_exact");
        }
        buf = buf.subspan(n);
        if (offset >= 0) {
            offset += static_cast<off_t>(n);
        }
    }
    return {};
}

Result<uint32_t> IO::Write(const UniqueFd& fd, const std::span<const std::byte> buf,
                           const off_t offset, const std::chrono::nanoseconds timeout) noexcept {
    if (offset < -1) {
        return Error::FailErrno(EINVAL, "write");
    }
    // Same clamp as Read: a partial write beats a truncated length.
    const auto len = static_cast<unsigned int>(std::min(buf.size(), MAX_RW_LEN));
    return Syscall(
        "write",
        [&](io_uring_sqe* sqe) {
            io_uring_prep_write(sqe, fd.Get(), buf.data(), len, static_cast<std::uint64_t>(offset));
        },
        timeout);
}

Result<void> IO::WriteAll(const UniqueFd& fd, std::span<const std::byte> buf, off_t offset,
                          const std::chrono::nanoseconds timeout) noexcept {
    if (offset < -1) {
        return Error::FailErrno(EINVAL, "write_all");
    }
    while (!buf.empty()) {
        TRY(const n, Write(fd, buf, offset, timeout));
        if (n == 0) {
            return Error::FailErrno(EIO, "write_all");
        }
        buf = buf.subspan(n);
        if (offset >= 0) {
            offset += static_cast<off_t>(n);
        }
    }
    return {};
}

Result<uint32_t> IO::ReadFixed(const UniqueFd& fd, FixedBuffer& buf, const size_t len,
                               const off_t file_offset, const size_t buffer_offset,
                               const std::chrono::nanoseconds timeout) noexcept {
    if (file_offset < -1) {
        return Error::FailErrno(EINVAL, "read_fixed");
    }
    TRY(const safe_len, FixedRange(buf, len, buffer_offset, "read_fixed"));
    auto* start_addr = buf.Ptr() + buffer_offset;
    return Syscall(
        "read_fixed",
        [&](io_uring_sqe* sqe) {
            io_uring_prep_read_fixed(sqe, fd.Get(), start_addr, safe_len,
                                     static_cast<std::uint64_t>(file_offset),
                                     static_cast<int>(buf.Index()));
        },
        timeout);
}

Result<uint32_t> IO::ReadFixed(const UniqueFd& fd, FixedRegion& region, const off_t file_offset,
                               const std::chrono::nanoseconds timeout) noexcept {
    if (file_offset < -1) {
        return Error::FailErrno(EINVAL, "read_fixed");
    }
    TRY(const safe_len, FixedRegionRange(region, "read_fixed"));
    return Syscall(
        "read_fixed",
        [&](io_uring_sqe* sqe) {
            io_uring_prep_read_fixed(sqe, fd.Get(), region.Ptr(), safe_len,
                                     static_cast<std::uint64_t>(file_offset),
                                     static_cast<int>(region.Index()));
        },
        timeout);
}

Result<uint32_t> IO::ReadFixed(const UniqueFd& fd, FixedBuffer& buf) noexcept {
    return ReadFixed(fd, buf, buf.Size(), -1, 0);
}

Result<uint32_t> IO::WriteFixed(const UniqueFd& fd, const FixedBuffer& buf, const size_t len,
                                const off_t file_offset, const size_t buffer_offset,
                                const std::chrono::nanoseconds timeout,
                                const int rw_flags) noexcept {
    if (file_offset < -1) {
        return Error::FailErrno(EINVAL, "write_fixed");
    }
    TRY(const safe_len, FixedRange(buf, len, buffer_offset, "write_fixed"));
    const auto* start_addr = buf.Ptr() + buffer_offset;
    return Syscall(
        "write_fixed",
        [&](io_uring_sqe* sqe) {
            io_uring_prep_write_fixed(sqe, fd.Get(), start_addr, safe_len,
                                      static_cast<std::uint64_t>(file_offset),
                                      static_cast<int>(buf.Index()));
            sqe->rw_flags = static_cast<__kernel_rwf_t>(rw_flags);
        },
        timeout);
}

Result<uint32_t> IO::WriteFixed(const UniqueFd& fd, const FixedRegion& region,
                                const off_t file_offset, const int rw_flags,
                                const std::chrono::nanoseconds timeout) noexcept {
    if (file_offset < -1) {
        return Error::FailErrno(EINVAL, "write_fixed");
    }
    TRY(const safe_len, FixedRegionRange(region, "write_fixed"));
    return Syscall(
        "write_fixed",
        [&](io_uring_sqe* sqe) {
            io_uring_prep_write_fixed(sqe, fd.Get(), region.Ptr(), safe_len,
                                      static_cast<std::uint64_t>(file_offset),
                                      static_cast<int>(region.Index()));
            sqe->rw_flags = static_cast<__kernel_rwf_t>(rw_flags);
        },
        timeout);
}

Result<uint32_t> IO::WriteFixed(const UniqueFd& fd, const FixedBuffer& buf) noexcept {
    return WriteFixed(fd, buf, buf.Size(), -1, 0);
}

Result<uint32_t> IO::Readv(const UniqueFd& fd, std::span<const iovec> iovecs, const off_t offset,
                           const std::chrono::nanoseconds timeout) noexcept {
    if (offset < -1) {
        return Error::FailErrno(EINVAL, "readv");
    }
    return Syscall(
        "readv",
        [&](io_uring_sqe* sqe) {
            io_uring_prep_readv(sqe, fd.Get(), iovecs.data(),
                                static_cast<unsigned int>(iovecs.size()),
                                static_cast<std::uint64_t>(offset));
        },
        timeout);
}

Result<uint32_t> IO::Writev(const UniqueFd& fd, std::span<const iovec> iovecs, const off_t offset,
                            const std::chrono::nanoseconds timeout, const int rw_flags) noexcept {
    if (offset < -1) {
        return Error::FailErrno(EINVAL, "writev");
    }
    return Syscall(
        "writev",
        [&](io_uring_sqe* sqe) {
            io_uring_prep_writev2(sqe, fd.Get(), iovecs.data(),
                                  static_cast<unsigned int>(iovecs.size()),
                                  static_cast<std::uint64_t>(offset), rw_flags);
        },
        timeout);
}

Result<void> IO::Fsync(const UniqueFd& fd, const bool datasync,
                       const std::chrono::nanoseconds timeout) noexcept {
    return Syscall<void>(
        "fsync",
        [&](io_uring_sqe* sqe) {
            io_uring_prep_fsync(sqe, fd.Get(), datasync ? IORING_FSYNC_DATASYNC : 0);
        },
        timeout);
}

Result<UniqueFd> IO::Open(const std::filesystem::path& path, const int flags,
                          const mode_t mode) noexcept {
    return Syscall<UniqueFd>("open", [&](io_uring_sqe* sqe) {
        io_uring_prep_openat(sqe, AT_FDCWD, path.c_str(), flags, mode);
    });
}

Result<void> IO::Fallocate(const UniqueFd& fd, const int mode, const off_t offset,
                           const off_t len) noexcept {
    if (offset < 0 || len <= 0) {
        return Error::FailErrno(EINVAL, "fallocate");
    }
    return Syscall<void>("fallocate", [&](io_uring_sqe* sqe) {
        io_uring_prep_fallocate(sqe, fd.Get(), mode, static_cast<std::uint64_t>(offset),
                                static_cast<std::uint64_t>(len));
    });
}

Result<void> IO::Ftruncate(const UniqueFd& fd, const off_t len) noexcept {
    if (len < 0) {
        return Error::FailErrno(EINVAL, "ftruncate");
    }
    return Syscall<void>("ftruncate",
                         [&](io_uring_sqe* sqe) { io_uring_prep_ftruncate(sqe, fd.Get(), len); });
}

Result<UniqueFd> IO::Accept(const UniqueFd& server_fd, SocketAddress& client_addr, const int flags,
                            const std::chrono::nanoseconds timeout) noexcept {
    return Syscall<UniqueFd>(
        "accept",
        [&](io_uring_sqe* sqe) {
            client_addr.addrlen = sizeof(sockaddr_storage);
            io_uring_prep_accept(sqe, server_fd.Get(), client_addr.GetMutable(),
                                 &client_addr.addrlen, flags);
        },
        timeout);
}

Result<UniqueFd> IO::Accept(const UniqueFd& server_fd, const int flags,
                            const std::chrono::nanoseconds timeout) noexcept {
    return Syscall<UniqueFd>(
        "accept",
        [&](io_uring_sqe* sqe) {
            io_uring_prep_accept(sqe, server_fd.Get(), nullptr, nullptr, flags);
        },
        timeout);
}

Result<struct statx> IO::Statx(const UniqueFd& fd, const unsigned int mask) noexcept {
    struct statx stx{};
    TRY_VOID(Syscall<void>("statx", [&](io_uring_sqe* sqe) {
        io_uring_prep_statx(sqe, fd.Get(), "", AT_EMPTY_PATH, mask, &stx);
    }));
    return stx;
}

Result<struct statx> IO::Statx(const std::filesystem::path& path, const unsigned int mask,
                               const int flags) noexcept {
    struct statx stx{};
    TRY_VOID(Syscall<void>("statx", [&](io_uring_sqe* sqe) {
        io_uring_prep_statx(sqe, AT_FDCWD, path.c_str(), flags, mask, &stx);
    }));
    return stx;
}

Result<void> IO::Connect(const UniqueFd& fd, const SocketAddress& addr,
                         const std::chrono::nanoseconds timeout) noexcept {
    return Syscall<void>(
        "connect",
        [&](io_uring_sqe* sqe) { io_uring_prep_connect(sqe, fd.Get(), addr.Get(), addr.addrlen); },
        timeout);
}

Result<uint32_t> IO::Poll(const UniqueFd& fd, const unsigned poll_mask,
                          const std::chrono::nanoseconds timeout) noexcept {
    return Syscall(
        "poll", [&](io_uring_sqe* sqe) { io_uring_prep_poll_add(sqe, fd.Get(), poll_mask); },
        timeout);
}

Result<void> IO::Remove(const std::filesystem::path& path) noexcept {
    return Syscall<void>("remove", [&](io_uring_sqe* sqe) {
        io_uring_prep_unlinkat(sqe, AT_FDCWD, path.c_str(), 0);
    });
}

Result<void> IO::Rename(const std::filesystem::path& from,
                        const std::filesystem::path& to) noexcept {
    return Syscall<void>("rename", [&](io_uring_sqe* sqe) {
        io_uring_prep_renameat(sqe, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), 0);
    });
}

Result<void> IO::SleepForNs(const std::chrono::nanoseconds duration) noexcept {
    if (duration.count() <= 0) {
        return {};
    }

    __kernel_timespec ts;
    const auto ns = duration.count();
    ts.tv_sec = ns / 1'000'000'000LL;
    ts.tv_nsec = ns % 1'000'000'000LL;

    // A timeout SQE completes with -ETIME when it fires: that is success here.
    auto r = Syscall<void>("sleep_for",
                           [&](io_uring_sqe* sqe) { io_uring_prep_timeout(sqe, &ts, 0U, 0U); });
    if (!r && r.error().Value() == ETIME) {
        return {};
    }
    return r;
}

}  // namespace tinyio
