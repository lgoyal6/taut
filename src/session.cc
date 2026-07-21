#include "taut/session.h"

#include <algorithm>
#include <array>
#include <span>
#include <utility>

namespace taut {
namespace {
// Retransmit backoff cap (§5.4). The base RTO comes from the RttEstimator.
constexpr std::chrono::milliseconds kMaxRto{2000};
// Zero-window persist timer bounds (§5.6): start 100 ms, ×2 backoff, cap 1 s.
constexpr std::chrono::milliseconds kPersistMin{100};
constexpr std::chrono::milliseconds kPersistMax{1000};
} // namespace

Session::Session(UdpTransport& transport, Endpoint peer, Config cfg)
    : tx_(transport), peer_(peer), cfg_(cfg), rtt_(cfg.rto_floor, kMaxRto),
      peer_adv_window_(cfg.window_pkts), capacity_(cfg.window_pkts) {}

// ---------------------------------------------------------------------------
// send path
// ---------------------------------------------------------------------------

bool Session::send(Class cls, ByteSpan payload) {
    // The worst-case header is base + SACK; refuse anything that can't fit one datagram.
    if (payload.size() > kMaxDatagram - kBaseHeaderSize - kSackSize) {
        return false;
    }

    if (cls == Class::Unreliable) {
        // Fire-and-forget: its own seq space, no ring, no retransmit, not windowed (§5.3).
        Packet p{};
        p.type = PacketType::Data;
        p.cls = Class::Unreliable;
        p.seq = next_unreliable_seq_++;
        fill_ack_fields(p);
        p.payload = payload;
        std::array<std::byte, kMaxDatagram> out{};
        const std::size_t n = encode(p, out);
        if (n == 0) {
            return false;
        }
        tx_.send(peer_, std::span<const std::byte>(out.data(), n));
        return true;
    }

    // Reliable: queue for the pump. Bound the queue so a stalled window can't grow memory
    // without bound — that is the backpressure signal to the app.
    if (pending_.size() >= cfg_.window_pkts) {
        return false;
    }
    pending_.push_back(PendingMsg{cls, std::vector<std::byte>(payload.begin(), payload.end())});
    pump();
    maybe_arm_persist();
    return true;
}

std::size_t Session::effective_window() const {
    return std::min<std::size_t>(cfg_.window_pkts, peer_adv_window_);
}

void Session::pump() {
    while (!pending_.empty() && ring_.size() < effective_window()) {
        PendingMsg msg = std::move(pending_.front());
        pending_.pop_front();

        const std::uint32_t seq = next_seq_++;
        Packet p{};
        p.type = PacketType::Data;
        p.cls = msg.cls;
        p.seq = seq;
        fill_ack_fields(p);
        p.payload = msg.data;

        std::vector<std::byte> datagram(kMaxDatagram);
        const std::size_t n = encode(p, datagram);
        if (n == 0) {
            continue; // should not happen (size checked in send); drop rather than loop
        }
        datagram.resize(n);
        tx_.send(peer_, datagram);

        const auto now = tx_.now();
        const auto rto = rtt_.rto();
        const TimerId id = timers_.schedule(now + rto);
        timer_to_seq_[id] = seq;
        Slot slot{};
        slot.seq = seq;
        slot.timer = id;
        slot.rto = rto;
        slot.transmit_count = 1;
        slot.send_time = now;
        slot.datagram = std::move(datagram);
        ring_.push_back(std::move(slot));
    }
}

void Session::process_cum_ack(std::uint32_t cum_ack) {
    const auto now = tx_.now();
    bool sampled = false;
    std::chrono::milliseconds rtt{};
    while (!ring_.empty() && ring_.front().seq < cum_ack) {
        const Slot& s = ring_.front();
        // Karn: sample only never-retransmitted (unambiguous) acks, and only once per slot.
        if (s.transmit_count == 1 && !s.rtt_sampled) {
            rtt = std::chrono::duration_cast<std::chrono::milliseconds>(now - s.send_time);
            sampled = true;
        }
        timers_.cancel(s.timer);
        timer_to_seq_.erase(s.timer);
        ring_.pop_front();
    }
    if (sampled) {
        rtt_.sample(rtt);
    }
}

void Session::process_sack(std::uint32_t cum_ack, std::uint64_t bitmap) {
    const auto now = tx_.now();
    // Mark selectively-acked slots. A SACKed packet is known-received: cancel its RTO so it is
    // never wastefully retransmitted, and take its (unambiguous) RTT sample now rather than at
    // the later cumulative ack, which would over-measure.
    for (auto& slot : ring_) {
        if (slot.seq <= cum_ack || slot.sacked) {
            continue;
        }
        const std::uint32_t i = slot.seq - cum_ack - 1;
        if (i < 64 && ((bitmap >> i) & 1ull) != 0) {
            slot.sacked = true;
            timers_.cancel(slot.timer);
            timer_to_seq_.erase(slot.timer);
            if (slot.transmit_count == 1 && !slot.rtt_sampled) {
                rtt_.sample(
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - slot.send_time));
                slot.rtt_sampled = true;
            }
        }
    }

    // Fast retransmit (§5.4): resend any un-SACKed slot that has ≥ 3 SACKed slots above it (the
    // Reno 3-dup-ack idea expressed on the bitmap). ring_ is ascending, so walk it backwards
    // counting SACKed slots seen so far.
    std::uint32_t sacked_above = 0;
    for (auto it = ring_.rbegin(); it != ring_.rend(); ++it) {
        if (it->sacked) {
            ++sacked_above;
        } else if (sacked_above >= 3 && !it->fast_retransmitted) {
            fast_retransmit(*it);
        }
    }
}

void Session::retransmit(Slot& slot) {
    if (slot.sacked) {
        return; // already received; do not resend
    }
    tx_.send(peer_, slot.datagram);
    ++slot.transmit_count;
    ++retransmit_count_;
    slot.rto = std::min(slot.rto * 2, kMaxRto); // exponential backoff on timeout
    const TimerId id = timers_.schedule(tx_.now() + slot.rto);
    timer_to_seq_[id] = slot.seq;
    slot.timer = id;
}

void Session::fast_retransmit(Slot& slot) {
    tx_.send(peer_, slot.datagram);
    ++slot.transmit_count; // Karn: its next ack yields no RTT sample
    ++retransmit_count_;
    slot.fast_retransmitted = true;
    // Not a timeout: keep the current RTO (no backoff), just refresh the timer so the RTO path
    // still backs up the fast retransmit if it too is lost.
    timers_.cancel(slot.timer);
    timer_to_seq_.erase(slot.timer);
    const TimerId id = timers_.schedule(tx_.now() + slot.rto);
    timer_to_seq_[id] = slot.seq;
    slot.timer = id;
}

void Session::maybe_arm_persist() {
    // The deadlock case (§5.6): the app has more to send, the peer advertised a zero window,
    // and nothing is in flight whose ack would re-advertise a reopened window. Probe on a
    // backing-off persist timer. (With packets still in flight, their RTO retransmits already
    // elicit fresh window advertisements, so no separate probe is needed.)
    const bool need = !pending_.empty() && peer_adv_window_ == 0 && ring_.empty();
    if (need && !persist_active_) {
        persist_id_ = timers_.schedule(tx_.now() + persist_interval_);
        persist_active_ = true;
    } else if (!need && persist_active_) {
        timers_.cancel(persist_id_);
        persist_active_ = false;
        persist_interval_ = kPersistMin;
    }
}

void Session::send_probe() {
    Packet p{};
    p.type = PacketType::ProbeWindow;
    p.cls = Class::Unreliable;
    fill_ack_fields(p);
    std::array<std::byte, kBaseHeaderSize + kSackSize> out{};
    const std::size_t n = encode(p, out);
    if (n > 0) {
        tx_.send(peer_, std::span<const std::byte>(out.data(), n));
    }
}

// ---------------------------------------------------------------------------
// receive path
// ---------------------------------------------------------------------------

void Session::poll() {
    std::array<std::byte, kMaxDatagram> buf{};
    while (auto r = tx_.recv(buf)) {
        Packet p{};
        if (decode(std::span<const std::byte>(buf.data(), r->size), p) != DecodeError::Ok) {
            continue; // malformed / corrupt — drop
        }
        // Update the send window only from a non-stale ack (cum_ack never goes backwards),
        // so a reordered older ack can't clobber it with a stale window (RFC 793 WL rule).
        if (p.cum_ack >= last_ack_) {
            peer_adv_window_ = p.adv_window;
            last_ack_ = p.cum_ack;
        }
        process_cum_ack(p.cum_ack);
        if ((p.flags & static_cast<std::uint8_t>(Flag::SackPresent)) != 0) {
            process_sack(p.cum_ack, p.sack);
        }
        if (p.type == PacketType::Data) {
            handle_data(p);
        } else if (p.type == PacketType::ProbeWindow) {
            send_ack(); // reply with our current advertised window
        }
    }
    pump();
    maybe_arm_persist();
}

void Session::tick() {
    const auto now = tx_.now();
    while (auto id = timers_.pop_due(now)) {
        if (persist_active_ && *id == persist_id_) {
            send_probe();
            persist_interval_ = std::min(persist_interval_ * 2, kPersistMax);
            persist_id_ = timers_.schedule(tx_.now() + persist_interval_);
            continue;
        }
        const auto it = timer_to_seq_.find(*id);
        if (it == timer_to_seq_.end()) {
            continue; // stale timer for an already-acked/SACKed packet
        }
        const std::uint32_t seq = it->second;
        timer_to_seq_.erase(it);
        for (auto& slot : ring_) {
            if (slot.seq == seq) {
                retransmit(slot);
                break;
            }
        }
    }
    maybe_arm_persist();
}

void Session::handle_data(const Packet& p) {
    if (p.cls == Class::Unreliable) {
        handle_unreliable(p);
    } else {
        handle_reliable(p);
    }
}

void Session::handle_unreliable(const Packet& p) {
    // Class 0: no retransmit, deliver immediately, dedup with a 64-seq sliding window (§5.3).
    // Excluded from cum_ack/SACK/ring; not acked (the sender never waits on it).
    const std::uint32_t s = p.seq;
    if (!ur_init_) {
        ur_init_ = true;
        ur_base_ = s;
        ur_seen_ = 0;
    }
    if (s < ur_base_) {
        return; // below the window — treat as a stale duplicate, drop
    }
    if (s - ur_base_ >= 64) {
        const std::uint32_t shift = s - ur_base_ - 63; // slide so s sits at the window top
        ur_seen_ = (shift >= 64) ? 0 : (ur_seen_ >> shift);
        ur_base_ += shift;
    }
    const std::uint32_t i = s - ur_base_;
    if (((ur_seen_ >> i) & 1ull) != 0) {
        return; // duplicate
    }
    ur_seen_ |= (1ull << i);
    if (app_ready_) {
        emit_to_app(Class::Unreliable, p.payload);
    }
    // If the app is not draining, an unreliable message may simply be dropped (semantics
    // permit it), keeping class 0 entirely out of the flow-control buffer.
}

void Session::handle_reliable(const Packet& p) {
    const std::uint32_t s = p.seq;
    if (s < rcv_next_) {
        // Already delivered (retransmit or network dup) — drop, but still ack so the sender
        // learns our cum_ack/window.
    } else if (s == rcv_next_) {
        if (deliver_reliable(p.cls, p.payload)) {
            ++rcv_next_;
            flush_reassembled();
        }
        // else: no buffer room (receiver full) — leave the gap; the sender retransmits later.
    } else { // s > rcv_next_ : out of order
        if (reasm_.find(s) == reasm_.end() && has_buffer_space()) {
            if (p.cls == Class::ReliableUnordered) {
                // Deliver on arrival (the latency win); keep a payload-less marker so cum_ack
                // and the SACK bitmap still account for it and dedup its retransmits.
                if (app_ready_) {
                    emit_to_app(p.cls, p.payload);
                } else {
                    rcv_buf_.push_back(PendingMsg{
                        p.cls, std::vector<std::byte>(p.payload.begin(), p.payload.end())});
                }
                reasm_.emplace(s, RxItem{p.cls, {}, true});
            } else {
                // Class 2: hold the payload for in-order delivery.
                reasm_.emplace(s, RxItem{p.cls,
                                         std::vector<std::byte>(p.payload.begin(), p.payload.end()),
                                         false});
                ++buffered_c2_;
            }
        }
        // Duplicate out-of-order, or no room: drop; ack below.
    }
    send_ack();
}

bool Session::deliver_reliable(Class cls, ByteSpan payload) {
    if (app_ready_) {
        emit_to_app(cls, payload);
        return true;
    }
    if (!has_buffer_space()) {
        return false;
    }
    rcv_buf_.push_back(PendingMsg{cls, std::vector<std::byte>(payload.begin(), payload.end())});
    return true;
}

void Session::flush_reassembled() {
    for (auto it = reasm_.find(rcv_next_); it != reasm_.end(); it = reasm_.find(rcv_next_)) {
        RxItem& e = it->second;
        if (!e.delivered) {
            // Class-2 payload now in order: moving it to the app (or the app queue) keeps
            // occupancy flat, so it always fits — no buffer check needed.
            if (app_ready_) {
                emit_to_app(e.cls, e.data);
            } else {
                rcv_buf_.push_back(PendingMsg{e.cls, std::move(e.data)});
            }
            --buffered_c2_;
        }
        reasm_.erase(it);
        ++rcv_next_;
    }
}

void Session::set_receiving(bool receiving) {
    app_ready_ = receiving;
    if (receiving) {
        while (!rcv_buf_.empty()) {
            PendingMsg m = std::move(rcv_buf_.front());
            rcv_buf_.pop_front();
            emit_to_app(m.cls, m.data);
        }
        send_ack(); // advertise the reopened window immediately
    }
}

void Session::send_ack() {
    Packet a{};
    a.type = PacketType::Ack;
    a.cls = Class::Unreliable;
    fill_ack_fields(a);
    std::array<std::byte, kBaseHeaderSize + kSackSize> out{};
    const std::size_t n = encode(a, out);
    if (n > 0) {
        tx_.send(peer_, std::span<const std::byte>(out.data(), n));
    }
}

void Session::fill_ack_fields(Packet& p) const {
    p.cum_ack = rcv_next_;
    p.adv_window = my_adv_window();
    if (sack_enabled_ && !reasm_.empty()) {
        p.flags |= static_cast<std::uint8_t>(Flag::SackPresent);
        p.sack = build_sack();
    }
}

std::uint64_t Session::build_sack() const {
    // Bit i marks reliable seq (rcv_next_ + 1 + i) received out of order. Every reasm_ key is
    // strictly above rcv_next_ (the contiguous run below it has already been delivered).
    std::uint64_t bitmap = 0;
    for (const auto& [seq, item] : reasm_) {
        const std::uint32_t i = seq - rcv_next_ - 1;
        if (i < 64) {
            bitmap |= (1ull << i);
        }
    }
    return bitmap;
}

std::uint16_t Session::my_adv_window() const {
    // Free receiver buffer, in packets: only the app-delivery queue counts. Out-of-order
    // class-2 data held in reasm_ sits within the sender's in-flight window (which it already
    // bounds by this advertisement), so counting it here would double-count. See DESIGN-flow.
    const std::size_t used = rcv_buf_.size();
    const std::size_t free = used < capacity_ ? capacity_ - used : 0;
    return static_cast<std::uint16_t>(std::min<std::size_t>(free, 0xFFFFu));
}

void Session::emit_to_app(Class cls, ByteSpan payload) {
    if (on_message_) {
        on_message_(cls, payload);
    }
    ++delivered_count_;
}

} // namespace taut
