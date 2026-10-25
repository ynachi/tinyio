#include "tinyio/net/net.hpp"

#include <arpa/inet.h>
#include <netdb.h>

#include <cstring>

namespace tinyio {
Result<SocketAddress> SocketAddress::V4(const uint16_t port, const char* ip) {
    SocketAddress sa;
    auto* in = reinterpret_cast<sockaddr_in*>(&sa.addr);
    in->sin_family = AF_INET;
    in->sin_port = htons(port);
    if (ip && *ip) {
        // inet_pton leaves the destination zeroed on failure, and a zeroed
        // sin_addr IS INADDR_ANY: an unchecked failure silently widens the bind.
        if (::inet_pton(AF_INET, ip, &in->sin_addr) != 1) {
            return Error::FailErrno(EINVAL, "inet_pton v4");
        }
    } else {
        in->sin_addr.s_addr = INADDR_ANY;
    }
    sa.addrlen = sizeof(sockaddr_in);
    return sa;
}

Result<SocketAddress> SocketAddress::V6(const uint16_t port, const char* ip) {
    SocketAddress sa;
    auto* in6 = reinterpret_cast<sockaddr_in6*>(&sa.addr);
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons(port);
    if (ip && *ip) {
        // Same as V4: a failed parse would zero sin6_addr == in6addr_any (::).
        if (::inet_pton(AF_INET6, ip, &in6->sin6_addr) != 1) {
            return Error::FailErrno(EINVAL, "inet_pton v6");
        }
    } else {
        in6->sin6_addr = in6addr_any;
    }
    sa.addrlen = sizeof(sockaddr_in6);
    return sa;
}

[[nodiscard]] std::optional<std::string> SocketAddress::GetIp() const {
    char buffer[INET6_ADDRSTRLEN];
    if (addr.ss_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(&addr);
        if (inet_ntop(AF_INET, &in->sin_addr, buffer, sizeof(buffer))) {
            return std::string(buffer);
        }
    } else if (addr.ss_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        if (inet_ntop(AF_INET6, &in6->sin6_addr, buffer, sizeof(buffer))) {
            return std::string(buffer);
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<uint16_t> SocketAddress::GetPort() const {
    if (addr.ss_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(&addr);
        return ntohs(in->sin_port);
    }
    if (addr.ss_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        return ntohs(in6->sin6_port);
    }
    return std::nullopt;
}

Result<SocketAddress> ResolveIp(const std::string_view host, const uint16_t port) {
    addrinfo hints{};
    addrinfo* res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    const std::string service = std::to_string(port);
    const std::string hostname(host);

    if (const int rc = getaddrinfo(hostname.c_str(), service.c_str(), &hints, &res); rc != 0) {
        int code = EIO;
        if (rc == EAI_SYSTEM) {
            code = errno;
        } else if (rc == EAI_AGAIN) {
            code = EAGAIN;
        } else if (rc == EAI_MEMORY) {
            code = ENOMEM;
        } else if (rc == EAI_NONAME || rc == EAI_FAIL) {
            code = EHOSTUNREACH;
        } else if (rc == EAI_FAMILY || rc == EAI_SOCKTYPE || rc == EAI_BADFLAGS ||
                   rc == EAI_SERVICE) {
            code = EINVAL;
        }
        return Error::FailErrno(code, "getaddrinfo");
    }

    if (!res) {
        return Error::FailErrno(EIO, "getaddrinfo_null_result");
    }

    SocketAddress out;
    std::memcpy(&out.addr, res->ai_addr, res->ai_addrlen);
    out.addrlen = res->ai_addrlen;
    freeaddrinfo(res);
    return out;
}
}  // namespace tinyio