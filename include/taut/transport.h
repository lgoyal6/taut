#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace taut {

// A datagram peer address (IPv4). Stored in network byte order, as it comes off the wire.
struct Endpoint {
    std::uint32_t addr_be = 0; // s_addr, network order
    std::uint16_t port_be = 0; // network order
    bool operator==(const Endpoint&) const = default;
};

struct RecvResult {
    std::size_t size; // bytes written into the caller's buffer
    Endpoint from;
};

// Build an Endpoint from a dotted-IPv4 string and host-order port. nullopt on parse error.
std::optional<Endpoint> make_endpoint(std::string_view ipv4, std::uint16_t port);

// Abstraction over the datagram substrate (§6.3). Two impls: real sockets (this file) and
// the in-process SimNet (week 2). No exceptions cross this boundary.
class UdpTransport {
  public:
    virtual ~UdpTransport() = default;

    // Send one datagram. Returns bytes sent, or 0 on failure.
    virtual std::size_t send(const Endpoint& to, std::span<const std::byte> data) = 0;

    // Receive one datagram into `buf`. nullopt when nothing is ready (would block).
    virtual std::optional<RecvResult> recv(std::span<std::byte> buf) = 0;

    // Monotonic clock (virtual so SimNet can drive a fake clock).
    virtual std::chrono::steady_clock::time_point now() const = 0;

    // Pollable fd for epoll, or -1 if not applicable (SimNet).
    virtual int fd() const = 0;
};

// Real, nonblocking UDP socket transport (Linux).
class RealUdpTransport : public UdpTransport {
  public:
    RealUdpTransport() = default;
    ~RealUdpTransport() override;
    RealUdpTransport(const RealUdpTransport&) = delete;
    RealUdpTransport& operator=(const RealUdpTransport&) = delete;

    // Create a nonblocking UDP socket and bind it. Returns false on failure.
    bool bind(std::string_view addr, std::uint16_t port);

    // The socket's actual bound address (resolves an ephemeral port bound with port 0).
    std::optional<Endpoint> local_endpoint() const;

    std::size_t send(const Endpoint& to, std::span<const std::byte> data) override;
    std::optional<RecvResult> recv(std::span<std::byte> buf) override;
    std::chrono::steady_clock::time_point now() const override;
    int fd() const override {
        return fd_;
    }

  private:
    int fd_ = -1;
};

} // namespace taut
