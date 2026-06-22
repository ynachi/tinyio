#include "tcp_listener.hpp"

#include <sys/socket.h>

#include "error.hpp"

namespace tinyio
{
Result<Fd> TcpListener::bind(const SocketAddress& addr, int backlog)
{
    // Create the raw socket
    const int raw_fd = ::socket(addr.addr.ss_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (raw_fd < 0)
    {
        return fail(errno, "socket");
    }

    // Immediately wrap in your RAII Fd primitive
    Fd sock{raw_fd};

    // Reuse your variadic SetOptions!
    // Note: TCP_NODELAY set on a listening socket is inherited by accepted clients in Linux.
    if (auto r = sock.set_options(SockOpt::ReuseAddr{true}, SockOpt::ReusePort{true}, SockOpt::NoDelay{true},
                                 SockOpt::NonBlocking{true});
        !r)
    {
        return std::unexpected(r.error());
    }

    // Explicit bind
    if (::bind(sock.get(), addr.get(), addr.addrlen) < 0)
    {
        return fail(errno, "bind");
    }

    // Start listening
    if (::listen(sock.get(), backlog) < 0)
    {
        return fail(errno, "listen");
    }

    return sock;
}

Result<Fd> TcpListener::bind(const uint16_t port, const char* ip, const int backlog)
{
    SocketAddress addr;

    if (ip != nullptr && std::string_view(ip).find(':') != std::string_view::npos)
    {
        addr = SocketAddress::v6(port, ip);
    }
    else
    {
        addr = SocketAddress::v4(port, ip);
    }

    return bind(addr, backlog);
}
}  // namespace tinyio