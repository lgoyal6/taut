#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

#include "taut/codec.h"
#include "taut/config.h"
#include "taut/timers.h"
#include "taut/transport.h"
#include "taut/types.h"

namespace taut {

// Reliable session with a single peer over a UdpTransport. Week 2: a send-buffer ring +
// cumulative-ack processing + timer-driven retransmit; class 2 (ReliableOrdered) delivery.
// Driven by poll() (process inbound) and tick() (fire retransmit timers) — the real event
// loop and the deterministic sim harness both just call those.
//
// Wire note: cum_ack here means "next expected sequence" (lowest not-yet-received), i.e. the
// TCP-style ack convention, which handles gaps unambiguously; this refines PLAN §5.2's
// "highest received" wording (see docs/DESIGN-window.md).
class Session {
  public:
    using MessageHandler = std::function<void(Class, ByteSpan)>;

    Session(UdpTransport& transport, Endpoint peer, Config cfg);

    void on_message(MessageHandler handler) {
        on_message_ = std::move(handler);
    }

    // Enqueue a message. Returns false if the send window is full (backpressure).
    bool send(Class cls, ByteSpan payload);

    // Process every datagram currently readable from the transport.
    void poll();

    // Retransmit any packets whose RTO has expired (uses the transport clock).
    void tick();

    // Introspection for tests / invariant checks.
    std::size_t in_flight() const {
        return ring_.size();
    }
    std::uint32_t delivered() const {
        return delivered_count_;
    }
    std::uint32_t next_expected() const {
        return next_expected_;
    }

  private:
    struct Slot {
        std::uint32_t seq;
        TimerId timer;
        std::chrono::milliseconds rto;
        std::uint32_t transmit_count;
        std::vector<std::byte> datagram; // full encoded packet, ready to resend
    };
    struct RxItem {
        Class cls;
        std::vector<std::byte> data;
    };

    void process_cum_ack(std::uint32_t next_expected_ack);
    void handle_data(const Packet& p);
    void retransmit(Slot& slot);
    void send_ack();
    void deliver(Class cls, ByteSpan payload);

    UdpTransport& tx_;
    Endpoint peer_;
    Config cfg_;
    MessageHandler on_message_;

    // send side
    std::uint32_t next_seq_ = 0;
    std::deque<Slot> ring_; // in-flight, ascending seq
    TimerHeap timers_;
    std::unordered_map<TimerId, std::uint32_t> timer_to_seq_;

    // receive side (class 2 ordered)
    std::uint32_t next_expected_ = 0;
    std::map<std::uint32_t, RxItem> reasm_; // buffered out-of-order payloads
    std::uint32_t delivered_count_ = 0;
};

} // namespace taut
