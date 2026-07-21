#pragma once

// A deterministic in-process UdpTransport for protocol unit tests: FIFO per destination
// (no reorder), a manually-advanced virtual clock, and a programmable drop hook so a test can
// deterministically drop a chosen datagram (a specific seq, the window-reopening ack, …).
// Distinct from SimNet, whose impairments are random/seeded; this one is fully scripted.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "taut/transport.h"

namespace tlink {

class TestLink;

class TestEndpoint : public taut::UdpTransport {
  public:
    std::size_t send(const taut::Endpoint& to, std::span<const std::byte> data) override;
    std::optional<taut::RecvResult> recv(std::span<std::byte> buf) override;
    std::chrono::steady_clock::time_point now() const override;
    int fd() const override {
        return -1;
    }

  private:
    friend class TestLink;
    TestEndpoint(TestLink* net, taut::Endpoint addr) : net_(net), addr_(addr) {}
    TestLink* net_;
    taut::Endpoint addr_;
};

class TestLink {
  public:
    // Returns true to DROP the datagram. Args: sender, receiver, bytes.
    using DropFn = std::function<bool(const taut::Endpoint&, const taut::Endpoint&,
                                      std::span<const std::byte>)>;

    TestEndpoint& endpoint(const taut::Endpoint& addr) {
        auto& slot = eps_[key(addr)];
        if (!slot) {
            slot = std::unique_ptr<TestEndpoint>(new TestEndpoint(this, addr));
            inbox_[key(addr)];
        }
        return *slot;
    }

    void advance(std::chrono::milliseconds by) {
        now_ += by;
    }
    std::chrono::steady_clock::time_point now() const {
        return now_;
    }

    DropFn drop; // optional

  private:
    friend class TestEndpoint;

    static std::uint64_t key(const taut::Endpoint& e) {
        return (static_cast<std::uint64_t>(e.addr_be) << 16) | e.port_be;
    }

    void deliver(const taut::Endpoint& from, const taut::Endpoint& to,
                 std::span<const std::byte> data) {
        if (drop && drop(from, to, data)) {
            return;
        }
        inbox_[key(to)].push_back({from, std::vector<std::byte>(data.begin(), data.end())});
    }

    std::optional<taut::RecvResult> receive(const taut::Endpoint& at, std::span<std::byte> buf) {
        auto it = inbox_.find(key(at));
        if (it == inbox_.end() || it->second.empty()) {
            return std::nullopt;
        }
        auto pkt = std::move(it->second.front());
        it->second.pop_front();
        const std::size_t n = std::min(pkt.second.size(), buf.size());
        if (n > 0) {
            std::memcpy(buf.data(), pkt.second.data(), n);
        }
        return taut::RecvResult{n, pkt.first};
    }

    std::chrono::steady_clock::time_point now_{};
    std::unordered_map<std::uint64_t, std::unique_ptr<TestEndpoint>> eps_;
    std::unordered_map<std::uint64_t, std::deque<std::pair<taut::Endpoint, std::vector<std::byte>>>>
        inbox_;
};

inline std::size_t TestEndpoint::send(const taut::Endpoint& to, std::span<const std::byte> data) {
    net_->deliver(addr_, to, data);
    return data.size();
}
inline std::optional<taut::RecvResult> TestEndpoint::recv(std::span<std::byte> buf) {
    return net_->receive(addr_, buf);
}
inline std::chrono::steady_clock::time_point TestEndpoint::now() const {
    return net_->now();
}

} // namespace tlink
