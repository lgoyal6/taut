#include "taut/transport.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstring>

namespace taut {

std::optional<Endpoint> make_endpoint(std::string_view ipv4, std::uint16_t port) {
    std::array<char, 64> tmp{};
    if (ipv4.size() >= tmp.size()) {
        return std::nullopt;
    }
    std::memcpy(tmp.data(), ipv4.data(), ipv4.size());
    tmp[ipv4.size()] = '\0';
    in_addr a{};
    if (::inet_pton(AF_INET, tmp.data(), &a) != 1) {
        return std::nullopt;
    }
    Endpoint e{};
    e.addr_be = a.s_addr; // already network order
    e.port_be = ::htons(port);
    return e;
}

RealUdpTransport::~RealUdpTransport() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

bool RealUdpTransport::bind(std::string_view addr, std::uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        return false;
    }
    const int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        return false;
    }
    // Allow rebinding a port a prior process just released (avoids EADDRINUSE on rapid
    // restart — e.g. the benchmark matrix rebinding the same port per loss point). Runs are
    // sequential, so this never lets two live sockets share the port.
    const int reuse = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    const auto ep = make_endpoint(addr, port);
    if (!ep) {
        return false;
    }
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = ep->addr_be;
    sa.sin_port = ep->port_be;
    return ::bind(fd_, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) == 0;
}

std::optional<Endpoint> RealUdpTransport::local_endpoint() const {
    if (fd_ < 0) {
        return std::nullopt;
    }
    sockaddr_in sa{};
    socklen_t slen = sizeof(sa);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&sa), &slen) != 0) {
        return std::nullopt;
    }
    Endpoint e{};
    e.addr_be = sa.sin_addr.s_addr;
    e.port_be = sa.sin_port;
    return e;
}

std::size_t RealUdpTransport::send(const Endpoint& to, std::span<const std::byte> data) {
    if (fd_ < 0) {
        return 0;
    }
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = to.addr_be;
    sa.sin_port = to.port_be;
    const ssize_t n = ::sendto(fd_, data.data(), data.size(), 0,
                               reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
    return n < 0 ? 0 : static_cast<std::size_t>(n);
}

std::optional<RecvResult> RealUdpTransport::recv(std::span<std::byte> buf) {
    if (fd_ < 0) {
        return std::nullopt;
    }
    sockaddr_in sa{};
    socklen_t slen = sizeof(sa);
    const ssize_t n =
        ::recvfrom(fd_, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&sa), &slen);
    if (n < 0) {
        return std::nullopt; // EAGAIN/EWOULDBLOCK when the socket is drained
    }
    Endpoint from{};
    from.addr_be = sa.sin_addr.s_addr;
    from.port_be = sa.sin_port;
    return RecvResult{static_cast<std::size_t>(n), from};
}

std::chrono::steady_clock::time_point RealUdpTransport::now() const {
    return std::chrono::steady_clock::now();
}

} // namespace taut
