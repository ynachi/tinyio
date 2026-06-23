#pragma once
#include <unistd.h>

#include "error.hpp"
#include "net.hpp"

namespace tinyio {
class IO;  // forward declaration for typed back-pointer

struct Fd {
  int fd = -1;
  bool direct = false;
  IO* io_ptr = nullptr;

  Fd() = default;
  explicit Fd(const int f) : fd(f) {}
  ~Fd() { CloseImpl(); }

  Fd(Fd&& other) noexcept : fd(other.fd), direct(other.direct), io_ptr(other.io_ptr) {
    other.fd = -1;
    other.direct = false;
    other.io_ptr = nullptr;
  }

  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) {
      CloseImpl();
      fd = other.fd;
      direct = other.direct;
      io_ptr = other.io_ptr;
      other.fd = -1;
      other.direct = false;
      other.io_ptr = nullptr;
    }
    return *this;
  }

  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;

  /// @brief Get the raw file descriptor or direct idx.
  [[nodiscard]] int Get() const noexcept { return fd; }

  /// @brief Check if the descriptor is valid.
  [[nodiscard]] bool IsValid() const noexcept { return fd >= 0; }

  /// @brief Check if it's a direct descriptor.
  [[nodiscard]] bool IsDirect() const noexcept { return direct; }

  /// @brief Release ownership to the caller.
  [[nodiscard]] int Release() noexcept {
    const int out = fd;
    fd = -1;
    direct = false;
    io_ptr = nullptr;
    return out;
  }

  void CloseImpl() noexcept;

  /// Single option
  /// @code
  /// URing::Fd sock{raw_fd};
  /// if (auto r = sock.set_option(SockOpt::NoDelay{}); !r)
  ///    return r;
  /// @endcode
  template <SocketOption Opt>
  [[nodiscard]] Result<void> SetOption(const Opt& opt) const noexcept {
    return opt.apply(fd);
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
    Result<void> r;
    ((r = std::forward<Opts>(opts).Apply(fd), r.has_value()) && ...);
    return r;
  }
};
}  // namespace tinyio