#include "tcp_listener.hpp"

#include <sys/socket.h>

#include "error.hpp"

namespace tinyio {
Result<Fd> TcpListener::Bind(const SocketAddress& addr, int backlog) {
  // Create the raw socket
  const int raw_fd = ::socket(addr.addr.ss_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (raw_fd < 0) {
    return Fail(errno, "socket");
  }

  // Immediately wrap in your RAII Fd primitive
  Fd sock{raw_fd};

  // Reuse your variadic SetOptions!
  // Note: TCP_NODELAY set on a listening socket is inherited by accepted clients in
  // Linux.
  if (auto r = sock.SetOptions(sock_opt::ReuseAddr{true}, sock_opt::ReusePort{true},
                               sock_opt::NoDelay{true}, sock_opt::NonBlocking{true});
      !r) {
    return std::unexpected(r.error());
  }

  // Explicit bind
  if (::bind(sock.Get(), addr.Get(), addr.addrlen) < 0) {
    return Fail(errno, "bind");
  }

  // Start listening
  if (::listen(sock.Get(), backlog) < 0) {
    return Fail(errno, "listen");
  }

  return sock;
}

Result<Fd> TcpListener::Bind(const uint16_t port, const char* ip, const int backlog) {
  SocketAddress addr;

  if (ip != nullptr && std::string_view(ip).find(':') != std::string_view::npos) {
    addr = SocketAddress::V6(port, ip);
  } else {
    addr = SocketAddress::V4(port, ip);
  }

  return Bind(addr, backlog);
}
}  // namespace tinyio