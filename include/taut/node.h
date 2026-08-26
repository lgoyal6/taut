#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

#include "taut/config.h"
#include "taut/types.h"

namespace taut {

// Public transport node (§5.1). Single-threaded by design (§5.7); no exceptions cross
// this boundary - fallible calls return bool/expected-style results. This is the Week 1
// S1 API *sketch*: declarations only. The implementation lands module-by-module through
// the PLAN §2 loop (loop.cc / transport.cc), each behind its comprehension gate.
class Node {
  public:
    using MessageHandler = std::function<void(PeerId, Class, ByteSpan)>;
    using PeerStateHandler = std::function<void(PeerId, MemberState)>;

    explicit Node(Config cfg);
    ~Node();

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    Node(Node&&) noexcept;
    Node& operator=(Node&&) noexcept;

    // Bind the UDP socket. Returns false on failure (no throw).
    bool bind(std::string_view addr, std::uint16_t port);

    // Register a peer; returns its handle.
    PeerId add_peer(std::string_view addr, std::uint16_t port);

    // Enqueue a message for delivery under the given reliability class.
    // Returns false on backpressure (window full) - the caller decides what to do.
    bool send(PeerId peer, Class cls, ByteSpan payload);

    void on_message(MessageHandler handler);
    void on_peer_state(PeerStateHandler handler);

    void run();                                   // blocking event loop
    void poll(std::chrono::milliseconds timeout); // single-iteration variant

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace taut
