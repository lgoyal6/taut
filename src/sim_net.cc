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

double SimNet::uniform01() {
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(rng_);
}

std::chrono::milliseconds SimNet::draw_delay() {
    std::chrono::milliseconds d = imp_.delay;
    if (imp_.jitter.count() > 0) {
        std::uniform_int_distribution<std::int64_t> j(0, imp_.jitter.count() - 1);
        d += std::chrono::milliseconds(j(rng_));
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
