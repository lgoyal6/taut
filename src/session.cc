#include "taut/session.h"

#include <algorithm>
#include <array>
#include <span>
#include <utility>

namespace taut {
namespace {
// Retransmit backoff cap (§5.4). The base RTO now comes from the RttEstimator.
constexpr std::chrono::milliseconds kMaxRto{2000};
} // namespace

Session::Session(UdpTransport& transport, Endpoint peer, Config cfg)
    : tx_(transport), peer_(peer), cfg_(cfg), rtt_(cfg.rto_floor, kMaxRto) {}

bool Session::send(Class cls, ByteSpan payload) {
    if (ring_.size() >= cfg_.window_pkts) {
        return false; // window full — backpressure
    }
    if (payload.size() > kMaxDatagram - kBaseHeaderSize) {
        return false;
    }
    const std::uint32_t seq = next_seq_++;
    Packet p{};
    p.type = PacketType::Data;
    p.flags = 0;
    p.cls = cls;
    p.seq = seq;
    p.cum_ack = next_expected_; // piggyback our ack
    p.adv_window = cfg_.window_pkts;
    p.payload = payload;

    std::vector<std::byte> datagram(kBaseHeaderSize + payload.size());
    const std::size_t n = encode(p, datagram);
    if (n == 0) {
        return false;
    }
    datagram.resize(n);
    tx_.send(peer_, datagram);

    const auto now = tx_.now();
    const auto rto = rtt_.rto();
    const TimerId id = timers_.schedule(now + rto);
    timer_to_seq_[id] = seq;
    ring_.push_back(Slot{seq, id, rto, 1, now, std::move(datagram)});
    return true;
}

void Session::poll() {
    std::array<std::byte, kMaxDatagram> buf{};
    while (auto r = tx_.recv(buf)) {
        Packet p{};
        if (decode(std::span<const std::byte>(buf.data(), r->size), p) != DecodeError::Ok) {
            continue; // malformed / corrupt — drop
        }
        process_cum_ack(p.cum_ack); // acks ride on everything
        if (p.type == PacketType::Data) {
            handle_data(p);
        }
    }
}

void Session::tick() {
    const auto now = tx_.now();
    while (auto id = timers_.pop_due(now)) {
        const auto it = timer_to_seq_.find(*id);
        if (it == timer_to_seq_.end()) {
            continue; // stale timer for an already-acked packet
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
}

void Session::process_cum_ack(std::uint32_t next_expected_ack) {
    const auto now = tx_.now();
    bool sampled = false;
    std::chrono::milliseconds rtt{};
    while (!ring_.empty() && ring_.front().seq < next_expected_ack) {
        const Slot& s = ring_.front();
        if (s.transmit_count == 1) { // Karn: sample only never-retransmitted (unambiguous) acks
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

void Session::handle_data(const Packet& p) {
    const std::uint32_t s = p.seq;
    if (s == next_expected_) {
        deliver(p.cls, p.payload);
        ++next_expected_;
        for (auto it = reasm_.find(next_expected_); it != reasm_.end();
             it = reasm_.find(next_expected_)) {
            deliver(it->second.cls, it->second.data);
            reasm_.erase(it);
            ++next_expected_;
        }
    } else if (s > next_expected_) {
        if (reasm_.find(s) == reasm_.end()) {
            reasm_[s] = RxItem{p.cls, std::vector<std::byte>(p.payload.begin(), p.payload.end())};
        }
    }
    // s < next_expected_ => duplicate already delivered; drop but still ack below.
    send_ack();
}

void Session::retransmit(Slot& slot) {
    tx_.send(peer_, slot.datagram);
    ++slot.transmit_count;
    slot.rto = std::min(slot.rto * 2, kMaxRto);
    const TimerId id = timers_.schedule(tx_.now() + slot.rto);
    timer_to_seq_[id] = slot.seq;
    slot.timer = id;
}

void Session::send_ack() {
    Packet a{};
    a.type = PacketType::Ack;
    a.flags = 0;
    a.cls = Class::Unreliable;
    a.seq = 0;
    a.cum_ack = next_expected_;
    a.adv_window = cfg_.window_pkts;
    a.payload = {};
    std::array<std::byte, kBaseHeaderSize> out{};
    const std::size_t n = encode(a, out);
    if (n > 0) {
        tx_.send(peer_, std::span<const std::byte>(out.data(), n));
    }
}

void Session::deliver(Class cls, ByteSpan payload) {
    if (on_message_) {
        on_message_(cls, payload);
    }
    ++delivered_count_;
}

} // namespace taut
