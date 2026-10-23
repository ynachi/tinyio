#pragma once
#include <unistd.h>

#include "error.hpp"
#include "net.hpp"

namespace tinyio {
class IO;  // forward declaration for typed back-pointer

class UniqueFd {
 public:
  using Closer = void (*)(int) noexcept;

  static void DefaultCloser(const int fd) noexcept {
    if (fd >= 0) {
      ::close(fd);
    }
  }

  static void NoopCloser(int) noexcept {}

 private:
  int fd_ = -1;
  Closer closer_ = DefaultCloser;
  // A direct descriptor holds an index into the io_uring registered file
  // table instead of a kernel descriptor: it must never be closed.
  bool direct_ = false;

 public:
  UniqueFd() noexcept = default;

  explicit UniqueFd(const int fd, const Closer closer = DefaultCloser) noexcept
      : fd_(fd), closer_(closer) {}

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  UniqueFd(UniqueFd&& other) noexcept
      : fd_(std::exchange(other.fd_, -1)),
        closer_(std::exchange(other.closer_, DefaultCloser)) {}

  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      Reset();
      fd_ = std::exchange(other.fd_, -1);
      closer_ = std::exchange(other.closer_, DefaultCloser);
    }
    return *this;
  }

  ~UniqueFd() { Reset(); }

  [[nodiscard]] int Get() const noexcept { return fd_; }

  [[nodiscard]] bool Valid() const noexcept { return fd_ >= 0; }

  /// True when this holds an index into the io_uring registered file
  /// table rather than a kernel descriptor.
  [[nodiscard]] bool IsDirect() const noexcept { return direct_; }

  void SetDirect(const bool direct = true) noexcept { direct_ = direct; }

  void Reset(const int fd = -1, Closer closer = DefaultCloser) noexcept {
    // Same fd: never close-and-rehold (that would store a closed fd
    // and double-close it later); just install the closer.
    if (fd_ == fd) {
      closer_ = closer;
      return;
    }
    if (fd_ >= 0 && closer_) {
      closer_(fd_);
    }
    fd_ = fd;
    closer_ = closer;
  }

  int Release() noexcept {
    closer_ = DefaultCloser;
    return std::exchange(fd_, -1);
  }

  /// Single option
  /// @code
  /// io::UniqueFd sock{raw_fd};
  /// if (auto r = sock.set_option(SockOpt::NoDelay{}); !r)
  ///    return r;
  /// @endcode
  template <SocketOption Opt>
  [[nodiscard]] Result<void> SetOption(const Opt& opt) const noexcept {
    return opt.Apply(fd_);
  }

  /// Variadic — short-circuits on first error
  /// @code
  /// if (auto r = sock.set_options(
  ///     SockOpt::ReuseAddr{},
  ///     SockOpt::ReusePort{},
  ///     SockOpt::NonBlocking{},
  ///     SockOpt::RecvBuffer{256 * 1024},
  ///     SockOpt::SendBuffer{256 * 1024}); !r)
  /// return r;
  /// @endcode
  template <SocketOption... Opts>
  [[nodiscard]] Result<void> SetOptions(Opts&&... opts) const noexcept {
    Result<void> result;
    static_cast<void>(
        ((result = std::forward<Opts>(opts).Apply(fd_), result.has_value()) && ...));
    return result;
  }
};
}  // namespace tinyio