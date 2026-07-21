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
#include "taut/rto.h"
#include "taut/timers.h"
#include "taut/transport.h"
#include "taut/types.h"

namespace taut {

// Reliable session with a single peer over a UdpTransport. Covers the send-buffer ring +
// cumulative acks + SACK/fast-retransmit + RTO retransmit (send path), the three reliability
// classes on the receive path (§5.3), and receiver-driven flow control with a zero-window
// persist probe (§5.6). Driven by poll() (process inbound) and tick() (fire timers) — the
// real event loop and the deterministic sim harness both just call those.
//
// Wire note: cum_ack here means "next expected sequence" (lowest reliable seq not yet
// received), i.e. the TCP-style ack convention, which handles gaps unambiguously; this
// refines PLAN §5.2's "highest received" wording (see docs/DESIGN-window.md). Consequently a
// SACK bit i marks reliable seq (cum_ack + 1 + i) — the seq at cum_ack is by definition the
// gap the receiver is still missing.
class Session {
  public:
    using MessageHandler = std::function<void(Class, ByteSpan)>;

    Session(UdpTransport& transport, Endpoint peer, Config cfg);

    void on_message(MessageHandler handler) {
        on_message_ = std::move(handler);
    }

    // Enqueue a message. Reliable classes (1,2) return false when the internal send queue is
    // full (backpressure); class 0 (Unreliable) is sent immediately, fire-and-forget, and is
    // never windowed or retransmitted. Returns false if the payload cannot fit one datagram.
    bool send(Class cls, ByteSpan payload);

    // Process every datagram currently readable from the transport.
    void poll();

    // Fire any due timers: RTO retransmits and the zero-window persist probe.
    void tick();

    // Application drain control (receiver side, flow control). When set false the app has
    // stopped consuming; delivered messages queue in the receive buffer, shrinking the
    // advertised window toward zero. Setting it true flushes the queue to the handler and
    // advertises the reopened window. Default true.
    void set_receiving(bool receiving);

    // Toggle SACK emission (for A/B against cumulative-ack-only recovery). Default true.
    void set_sack_enabled(bool enabled) {
        sack_enabled_ = enabled;
    }

    // Introspection for tests / invariant checks.
    std::size_t in_flight() const {
        return ring_.size();
    }
    std::uint32_t delivered() const {
        return delivered_count_;
    }
    std::uint32_t next_expected() const {
        return rcv_next_;
    }
    std::uint32_t retransmits() const {
        return retransmit_count_;
    }
    std::uint16_t peer_adv_window() const {
        return peer_adv_window_;
    }
    std::uint16_t adv_window() const {
        return my_adv_window();
    }
    // Receive-buffer occupancy in packets: app-queued messages + out-of-order class-2
    // payloads held for reassembly. Must never exceed the capacity (no-overflow invariant).
    std::size_t rcv_occupancy() const {
        return rcv_buf_.size() + buffered_c2_;
    }

  private:
    struct Slot {
        std::uint32_t seq;
        TimerId timer;
        std::chrono::milliseconds rto;
        std::uint32_t transmit_count;
        std::chrono::steady_clock::time_point send_time; // first transmit, for RTT (Karn)
        std::vector<std::byte> datagram;                 // full encoded packet, ready to resend
        bool sacked = false;                             // selectively acked by the peer
        bool fast_retransmitted = false;                 // already fast-retransmitted once
        bool rtt_sampled = false;                        // contributed an RTT sample already
    };
    // A received reliable packet held for ordering/dedup. For class 2 held out of order,
    // `data` carries the payload until it can be delivered in seq order. For class 1 (already
    // delivered on arrival) it is a payload-less marker: it exists only to advance cum_ack,
    // build the SACK bitmap, and dedup retransmits.
    struct RxItem {
        Class cls;
        std::vector<std::byte> data;
        bool delivered; // true => class-1 marker, nothing left to deliver
    };
    struct PendingMsg {
        Class cls;
        std::vector<std::byte> data;
    };

    // send path
    void pump(); // move queued messages into the ring while allowed
    std::size_t effective_window() const;
    void process_cum_ack(std::uint32_t cum_ack);
    void process_sack(std::uint32_t cum_ack, std::uint64_t bitmap);
    void retransmit(Slot& slot);      // timer-driven
    void fast_retransmit(Slot& slot); // SACK-driven (Reno on the bitmap)
    void maybe_arm_persist();
    void send_probe();

    // receive path
    void handle_data(const Packet& p);
    void handle_unreliable(const Packet& p);            // class 0 dedup + immediate delivery
    void handle_reliable(const Packet& p);              // class 1/2 dedup, ordering, delivery
    bool deliver_reliable(Class cls, ByteSpan payload); // in-order delivery; false if no room
    void flush_reassembled(); // deliver the contiguous run above rcv_next_
    void send_ack();
    void fill_ack_fields(Packet& p) const; // cum_ack + adv_window + SACK piggyback
    std::uint64_t build_sack() const;
    std::uint16_t my_adv_window() const;
    bool has_buffer_space() const {
        return rcv_occupancy() < capacity_;
    }
    void emit_to_app(Class cls, ByteSpan payload);

    UdpTransport& tx_;
    Endpoint peer_;
    Config cfg_;
    RttEstimator rtt_;
    MessageHandler on_message_;
    bool sack_enabled_ = true;

    // send side
    std::uint32_t next_seq_ = 0;            // reliable seq space (classes 1,2)
    std::uint32_t next_unreliable_seq_ = 0; // class-0 dedup counter (independent, §5.2 D2)
    std::deque<PendingMsg> pending_;        // reliable messages awaiting a ring slot
    std::deque<Slot> ring_;                 // in-flight reliable packets, ascending seq
    TimerHeap timers_;
    std::unordered_map<TimerId, std::uint32_t> timer_to_seq_;
    std::uint16_t peer_adv_window_; // last window advertised by the peer
    std::uint32_t last_ack_ = 0;    // highest cum_ack seen (gates window updates)
    std::uint32_t retransmit_count_ = 0;
    // zero-window persist timer (§5.6)
    bool persist_active_ = false;
    TimerId persist_id_ = 0;
    std::chrono::milliseconds persist_interval_{100};

    // receive side
    std::uint32_t rcv_next_ = 0;            // next reliable seq to cumulatively ack
    std::map<std::uint32_t, RxItem> reasm_; // received seqs above the gap (ordering + SACK)
    std::size_t buffered_c2_ = 0;           // class-2 payloads currently held in reasm_
    std::deque<PendingMsg> rcv_buf_;        // delivered-ready messages the app hasn't drained
    std::size_t capacity_;                  // receive-buffer capacity, in packets
    bool app_ready_ = true;                 // app is draining (see set_receiving)
    std::uint32_t delivered_count_ = 0;
    // class-0 anti-replay dedup window (§5.3): 64-seq sliding bitmap
    bool ur_init_ = false;
    std::uint32_t ur_base_ = 0;
    std::uint64_t ur_seen_ = 0;
};

} // namespace taut
