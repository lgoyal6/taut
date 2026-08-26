#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <unordered_map>
#include <vector>

#include "taut/transport.h"

namespace taut {

// Network impairments applied per datagram, driven from the SimNet's seeded RNG (§6.3).
struct Impairments {
    double loss = 0.0;                   // P(drop each datagram)
    double dup = 0.0;                    // P(deliver a second copy)
    std::chrono::milliseconds delay{0};  // base one-way delay
    std::chrono::milliseconds jitter{0}; // uniform [0, jitter) added to delay (source of reorder)
};

class SimNet;

// One endpoint on a SimNet, satisfying UdpTransport. Created via SimNet::endpoint; its
// clock and I/O are the SimNet's. fd() is -1 (not epoll-pollable - sim tests pump manually).
class SimEndpoint : public UdpTransport {
  public:
    std::size_t send(const Endpoint& to, std::span<const std::byte> data) override;
    std::optional<RecvResult> recv(std::span<std::byte> buf) override;
    std::chrono::steady_clock::time_point now() const override;
    int fd() const override {
        return -1;
    }

  private:
    friend class SimNet;
    SimEndpoint(SimNet* net, Endpoint addr) : net_(net), addr_(addr) {}
    SimNet* net_;
    Endpoint addr_;
};

// Deterministic, in-process datagram network with a virtual clock (§6.3). Same seed +
// same sequence of operations => byte-identical run, so every protocol-test failure
// reproduces from --seed N. That determinism is cross-platform: impairments are drawn
// from the engine directly rather than through std::uniform_*_distribution, which is
// unspecified and differs between libc++ and libstdc++ (see src/sim_net.cc).
class SimNet {
  public:
    using TimePoint = std::chrono::steady_clock::time_point;

    explicit SimNet(std::uint64_t seed, Impairments imp = {});

    // Register (or fetch) an endpoint at `addr`. The reference is stable for SimNet's life.
    SimEndpoint& endpoint(const Endpoint& addr);

    TimePoint now() const {
        return now_;
    }
    void advance(std::chrono::milliseconds by) {
        now_ += by;
    }

  private:
    friend class SimEndpoint;

    struct InFlight {
        TimePoint deliver_at;
        Endpoint from;
        std::vector<std::byte> bytes;
        std::uint64_t seqno; // deterministic tiebreak for equal deliver_at
    };

    static std::uint64_t key(const Endpoint& e) {
        return (static_cast<std::uint64_t>(e.addr_be) << 16) | e.port_be;
    }

    double uniform01();
    std::uint64_t uniform_below(std::uint64_t bound);
    std::chrono::milliseconds draw_delay();
    void deliver(const Endpoint& from, const Endpoint& to, std::span<const std::byte> data);
    std::optional<RecvResult> receive(const Endpoint& at, std::span<std::byte> buf);

    std::mt19937_64 rng_;
    Impairments imp_;
    TimePoint now_{};
    std::uint64_t seqctr_ = 0;
    std::unordered_map<std::uint64_t, std::unique_ptr<SimEndpoint>> endpoints_;
    std::unordered_map<std::uint64_t, std::vector<InFlight>> inbox_;
};

} // namespace taut
