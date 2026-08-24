#include "taut/sim_net.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace taut {

SimNet::SimNet(std::uint64_t seed, Impairments imp) : rng_(seed), imp_(imp) {}

SimEndpoint& SimNet::endpoint(const Endpoint& addr) {
    auto& slot = endpoints_[key(addr)];
    if (!slot) {
        slot = std::unique_ptr<SimEndpoint>(new SimEndpoint(this, addr));
        inbox_[key(addr)]; // ensure an inbox exists
    }
    return *slot;
}

// The class promises that the same seed reproduces a run byte for byte. That
// held only within one standard library: std::mt19937_64 is fully specified and
// portable, but std::uniform_real_distribution and std::uniform_int_distribution
// are not. libc++ and libstdc++ map engine output differently and consume
// different numbers of words doing it, so the same seed drew a different
// impairment sequence on macOS than on Linux, and Rx.UnreliableDedupNoRetransmit
// failed on one platform while passing on the other. Drawing straight from the
// engine is what makes the documented guarantee true everywhere.

double SimNet::uniform01() {
    // Canonical 53-significand-bit construction: one engine word, top 53 bits,
    // scaled by 2^-53. Yields [0, 1) with every representable step equally likely.
    return static_cast<double>(rng_() >> 11) * (1.0 / 9007199254740992.0);
}

std::uint64_t SimNet::uniform_below(std::uint64_t bound) {
    // Unbiased bounded draw. A plain modulo would over-represent the low
    // residues by the width of the leftover tail; rejecting that tail removes
    // the bias. `-bound % bound` is 2^64 mod bound in unsigned arithmetic.
    const std::uint64_t reject_below = (~bound + 1U) % bound;
    std::uint64_t r = rng_();
    while (r < reject_below) {
        r = rng_();
    }
    return r % bound;
}

std::chrono::milliseconds SimNet::draw_delay() {
    std::chrono::milliseconds d = imp_.delay;
    if (imp_.jitter.count() > 0) {
        d += std::chrono::milliseconds(
            static_cast<std::int64_t>(uniform_below(static_cast<std::uint64_t>(imp_.jitter.count()))));
    }
    return d;
}

void SimNet::deliver(const Endpoint& from, const Endpoint& to, std::span<const std::byte> data) {
    // Draw loss/dup unconditionally so the RNG sequence is stable regardless of outcome.
    if (uniform01() < imp_.loss) {
        return; // dropped
    }
    auto& box = inbox_[key(to)];
    const auto enqueue = [&](std::chrono::milliseconds delay) {
        box.push_back(InFlight{now_ + delay, from, std::vector<std::byte>(data.begin(), data.end()),
                               seqctr_++});
    };
    enqueue(draw_delay());
    if (uniform01() < imp_.dup) {
        enqueue(draw_delay()); // a second copy, possibly reordered by its own jitter
    }
}

std::optional<RecvResult> SimNet::receive(const Endpoint& at, std::span<std::byte> buf) {
    auto it = inbox_.find(key(at));
    if (it == inbox_.end()) {
        return std::nullopt;
    }
    auto& box = it->second;

    // Earliest deliverable datagram: min (deliver_at, seqno) among those now due.
    std::size_t best = box.size();
    for (std::size_t i = 0; i < box.size(); ++i) {
        if (box[i].deliver_at > now_) {
            continue;
        }
        if (best == box.size() || box[i].deliver_at < box[best].deliver_at ||
            (box[i].deliver_at == box[best].deliver_at && box[i].seqno < box[best].seqno)) {
            best = i;
        }
    }
    if (best == box.size()) {
        return std::nullopt; // nothing due yet
    }

    const auto& pkt = box[best];
    const std::size_t n = std::min(pkt.bytes.size(), buf.size());
    if (n > 0) {
        std::memcpy(buf.data(), pkt.bytes.data(), n);
    }
    const Endpoint from = pkt.from;
    box[best] = std::move(box.back());
    box.pop_back();
    return RecvResult{n, from};
}

std::size_t SimEndpoint::send(const Endpoint& to, std::span<const std::byte> data) {
    net_->deliver(addr_, to, data);
    return data.size();
}

std::optional<RecvResult> SimEndpoint::recv(std::span<std::byte> buf) {
    return net_->receive(addr_, buf);
}

std::chrono::steady_clock::time_point SimEndpoint::now() const {
    return net_->now();
}

} // namespace taut
