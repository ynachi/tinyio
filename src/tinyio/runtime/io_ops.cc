#include <algorithm>
#include <cerrno>

#include "tinyio/runtime/runtime.hpp"

namespace tinyio {
Result<FixedBuffer> IO::TakeFixedBuffer() noexcept {
  CHECK(buffer_pool_.has_value())
      << "take_fixed_buffer: no buffer pool configured (IoOptions::buffer_pool)";
  return buffer_pool_->Take();
}

Result<FixedBuffer> IO::AcquireFixedBuffer() noexcept {
  CHECK(buffer_pool_.has_value())
      << "acquire_fixed_buffer: no buffer pool configured (IoOptions::buffer_pool)";
  return buffer_pool_->Acquire(*this);
}

Result<unsigned int> IO::FixedRange(const FixedBuffer& buf, const size_t len,
                                    const size_t buffer_offset,
                                    const char* op_name) const noexcept {
  // The slot has to come from this runtime's pool: the kernel resolves the
  // buf_index against the table registered with this ring, and a foreign
  // index would name the wrong buffer.
  CHECK(buffer_pool_.has_value() && buf.Pool() == &*buffer_pool_)
      << op_name << ": the buffer is not registered with this runtime";
  // The buffer offset has to land inside the slot, and the length must not
  // be zero: either is an EINVAL. A length that runs past the slot is
  // clamped to what is left of it, so the kernel never touches the next
  // slot.
  if (buffer_offset >= buf.Size() || len == 0) {
    return Error::fail_errno(EINVAL, op_name);
  }
  return static_cast<unsigned int>(
      std::min({len, buf.Size() - buffer_offset, kMaxRwLen}));
}

Result<void> IO::Close(const int fd) noexcept {
  return Syscall<void>("close", [&](io_uring_sqe* sqe) { io_uring_prep_close(sqe, fd); });
}

Result<void> IO::Close(UniqueFd fd) noexcept {
  if (!fd.Valid()) return {};

  return Syscall<void>(
      "close", [&](io_uring_sqe* sqe) { io_uring_prep_close(sqe, fd.Release()); });
}

//
// ======================= IO Methods ==========================================
//

Result<uint32_t> IO::Read(const UniqueFd& fd, const std::span<std::byte> buf,
                          const off_t offset,
                          const std::chrono::nanoseconds timeout) noexcept {
  // A span past the kernel's per-op cap would truncate through the unsigned
  // SQE length; clamp so the caller gets a short read instead of a wrong one.
  const auto len = static_cast<unsigned int>(std::min(buf.size(), kMaxRwLen));
  return Syscall(
      "read",
      [&](io_uring_sqe* sqe) {
        io_uring_prep_read(sqe, fd.Get(), buf.data(), len,
                           static_cast<std::uint64_t>(offset));
      },
      timeout);
}

Result<uint32_t> IO::Write(const UniqueFd& fd, const std::span<const std::byte> buf,
                           const off_t offset,
                           const std::chrono::nanoseconds timeout) noexcept {
  // Same clamp as Read: a partial write beats a truncated length.
  const auto len = static_cast<unsigned int>(std::min(buf.size(), kMaxRwLen));
  return Syscall(
      "write",
      [&](io_uring_sqe* sqe) {
        io_uring_prep_write(sqe, fd.Get(), buf.data(), len,
                            static_cast<std::uint64_t>(offset));
      },
      timeout);
}

Result<uint32_t> IO::ReadFixed(const UniqueFd& fd, FixedBuffer& buf, const size_t len,
                               const off_t file_offset,
                               const size_t buffer_offset) noexcept {
  TRY(const safe_len, FixedRange(buf, len, buffer_offset, "read_fixed"));
  auto* start_addr = buf.Ptr() + buffer_offset;
  return Syscall("read_fixed", [&](io_uring_sqe* sqe) {
    io_uring_prep_read_fixed(sqe, fd.Get(), start_addr, safe_len,
                             static_cast<std::uint64_t>(file_offset),
                             static_cast<int>(buf.Index()));
  });
}

Result<uint32_t> IO::ReadFixed(const UniqueFd& fd, FixedBuffer& buf,
                               const off_t file_offset) noexcept {
  return ReadFixed(fd, buf, buf.Size(), file_offset, 0);
}

Result<uint32_t> IO::WriteFixed(const UniqueFd& fd, const FixedBuffer& buf,
                                const size_t len, const off_t file_offset,
                                const size_t buffer_offset) noexcept {
  TRY(const safe_len, FixedRange(buf, len, buffer_offset, "write_fixed"));
  const auto* start_addr = buf.Ptr() + buffer_offset;
  return Syscall("write_fixed", [&](io_uring_sqe* sqe) {
    io_uring_prep_write_fixed(sqe, fd.Get(), start_addr, safe_len,
                              static_cast<std::uint64_t>(file_offset),
                              static_cast<int>(buf.Index()));
  });
}

Result<uint32_t> IO::WriteFixed(const UniqueFd& fd, const FixedBuffer& buf,
                                const off_t file_offset) noexcept {
  return WriteFixed(fd, buf, buf.Size(), file_offset, 0);
}

Result<uint32_t> IO::Readv(const UniqueFd& fd, std::span<const iovec> iovecs,
                           const off_t offset) noexcept {
  return Syscall("readv", [&](io_uring_sqe* sqe) {
    io_uring_prep_readv(sqe, fd.Get(), iovecs.data(),
                        static_cast<unsigned int>(iovecs.size()),
                        static_cast<std::uint64_t>(offset));
  });
}

Result<uint32_t> IO::Writev(const UniqueFd& fd, std::span<const iovec> iovecs,
                            const off_t offset) noexcept {
  return Syscall("writev", [&](io_uring_sqe* sqe) {
    io_uring_prep_writev(sqe, fd.Get(), iovecs.data(),
                         static_cast<unsigned int>(iovecs.size()),
                         static_cast<std::uint64_t>(offset));
  });
}

Result<void> IO::Fsync(const UniqueFd& fd, const bool datasync) noexcept {
  return Syscall<void>("fsync", [&](io_uring_sqe* sqe) {
    io_uring_prep_fsync(sqe, fd.Get(), datasync ? IORING_FSYNC_DATASYNC : 0);
  });
}

Result<UniqueFd> IO::Open(const std::filesystem::path& path, const int flags,
                          const mode_t mode) noexcept {
  return Syscall<UniqueFd>("open", [&](io_uring_sqe* sqe) {
    io_uring_prep_openat(sqe, AT_FDCWD, path.c_str(), flags, mode);
  });
}

Result<void> IO::Fallocate(const UniqueFd& fd, const int mode, const off_t offset,
                           const off_t len) noexcept {
  return Syscall<void>("fallocate", [&](io_uring_sqe* sqe) {
    io_uring_prep_fallocate(sqe, fd.Get(), mode, static_cast<std::uint64_t>(offset),
                            static_cast<std::uint64_t>(len));
  });
}

Result<void> IO::Ftruncate(const UniqueFd& fd, const off_t len) noexcept {
  return Syscall<void>("ftruncate", [&](io_uring_sqe* sqe) {
    io_uring_prep_ftruncate(sqe, fd.Get(), len);
  });
}

Result<UniqueFd> IO::Accept(const UniqueFd& server_fd, SocketAddress& client_addr,
                            const int flags,
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

Result<void> IO::Connect(const UniqueFd& fd, const SocketAddress& addr,
                         const std::chrono::nanoseconds timeout) noexcept {
  return Syscall<void>(
      "connect",
      [&](io_uring_sqe* sqe) {
        io_uring_prep_connect(sqe, fd.Get(), addr.Get(), addr.addrlen);
      },
      timeout);
}

Result<uint32_t> IO::Poll(const UniqueFd& fd, const unsigned poll_mask,
                          const std::chrono::nanoseconds timeout) noexcept {
  return Syscall(
      "poll",
      [&](io_uring_sqe* sqe) { io_uring_prep_poll_add(sqe, fd.Get(), poll_mask); },
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
  auto r = Syscall<void>(
      "sleep_for", [&](io_uring_sqe* sqe) { io_uring_prep_timeout(sqe, &ts, 0U, 0U); });
  if (!r && r.error().value() == ETIME) {
    return {};
  }
  return r;
}

}  // namespace tinyio
