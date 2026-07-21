#include "taut/session.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "taut/codec.h"
#include "taut/config.h"
#include "taut/sim_net.h"
#include "taut/types.h"
#include "test_link.h"

using namespace std::chrono_literals;

namespace {

taut::Endpoint ep(std::uint16_t port) {
    taut::Endpoint e{};
    e.addr_be = 1;
    e.port_be = port;
    return e;
}

// Decode a datagram's (type, class, seq) for a drop hook. Returns false if it doesn't parse.
bool peek(std::span<const std::byte> d, taut::PacketType& type, taut::Class& cls,
          std::uint32_t& seq) {
    taut::Packet p{};
    if (taut::decode(d, p) != taut::DecodeError::Ok) {
        return false;
    }
    type = p.type;
    cls = p.cls;
    seq = p.seq;
    return true;
}

} // namespace

// Drop seq 0 exactly once. The receiver gets seqs 1..9, SACKs them, and the sender must fast-
// retransmit the single gap after ≥ 3 SACKed slots pile up above it — not wait for its RTO,
// and not touch the already-received packets above the gap.
TEST(Sack, FastRetransmitsSingleGap) {
    tlink::TestLink net;
    const auto a = ep(1);
    const auto b = ep(2);
    taut::Config cfg;
    cfg.window_pkts = 16;

    taut::Session sa(net.endpoint(a), b, cfg);
    taut::Session sb(net.endpoint(b), a, cfg);

    bool dropped0 = false;
    net.drop = [&](const taut::Endpoint& from, const taut::Endpoint&,
                   std::span<const std::byte> d) {
        taut::PacketType t{};
        taut::Class c{};
        std::uint32_t s = 0;
        if (from == a && peek(d, t, c, s) && t == taut::PacketType::Data &&
            c != taut::Class::Unreliable && s == 0 && !dropped0) {
            dropped0 = true;
            return true; // drop the first transmission of seq 0
        }
        return false;
    };

    std::vector<std::uint32_t> got;
    sb.on_message([&](taut::Class, taut::ByteSpan p) {
        std::uint32_t v = 0;
        std::memcpy(&v, p.data(), std::min(sizeof(v), p.size()));
        got.push_back(v);
    });

    const int n = 10;
    for (int i = 0; i < n; ++i) {
        std::array<std::byte, 4> payload{};
        const auto val = static_cast<std::uint32_t>(i);
        std::memcpy(payload.data(), &val, sizeof(val));
        ASSERT_TRUE(sa.send(taut::Class::ReliableOrdered, payload));
    }

    // Pump a few rounds WITHOUT advancing the clock enough to reach any RTO (initial 200 ms):
    // recovery here must come from SACK/fast-retransmit alone.
    for (int iter = 0; iter < 12 && got.size() < static_cast<std::size_t>(n); ++iter) {
        net.advance(1ms);
        sa.poll();
        sb.poll();
        sa.tick();
        sb.tick();
    }

    ASSERT_EQ(got.size(), static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        EXPECT_EQ(got[static_cast<std::size_t>(i)], static_cast<std::uint32_t>(i));
    }
    EXPECT_EQ(sa.retransmits(), 1u); // exactly the gap, fast-retransmitted once
}

// Same single-gap scenario with SACK disabled: recovery falls back to the per-packet RTO, and
// the already-received packets above the gap are retransmitted wastefully too — strictly more
// retransmits than the SACK path, and only after a full RTO wait.
TEST(Sack, CumulativeOnlyRetransmitsMoreThanSack) {
    tlink::TestLink net;
    const auto a = ep(1);
    const auto b = ep(2);
    taut::Config cfg;
    cfg.window_pkts = 16;

    taut::Session sa(net.endpoint(a), b, cfg);
    taut::Session sb(net.endpoint(b), a, cfg);
    sa.set_sack_enabled(false);
    sb.set_sack_enabled(false);

    bool dropped0 = false;
    net.drop = [&](const taut::Endpoint& from, const taut::Endpoint&,
                   std::span<const std::byte> d) {
        taut::PacketType t{};
        taut::Class c{};
        std::uint32_t s = 0;
        if (from == a && peek(d, t, c, s) && t == taut::PacketType::Data &&
            c != taut::Class::Unreliable && s == 0 && !dropped0) {
            dropped0 = true;
            return true;
        }
        return false;
    };

    int delivered = 0;
    sb.on_message([&](taut::Class, taut::ByteSpan) { ++delivered; });

    const int n = 10;
    for (int i = 0; i < n; ++i) {
        std::array<std::byte, 4> payload{};
        const auto val = static_cast<std::uint32_t>(i);
        std::memcpy(payload.data(), &val, sizeof(val));
        ASSERT_TRUE(sa.send(taut::Class::ReliableOrdered, payload));
    }

    for (int iter = 0; iter < 2000 && delivered < n; ++iter) {
        net.advance(1ms);
        sa.poll();
        sb.poll();
        sa.tick();
        sb.tick();
    }

    ASSERT_EQ(delivered, n);
    EXPECT_GT(sa.retransmits(), 1u); // gap + wastefully-resent above-gap packets
}

namespace {

struct Outcome {
    int iters;
    std::uint32_t retransmits;
};

// Run an A->B transfer of `n` ordered messages over a lossy SimNet, with SACK on or off, and
// report how many pump iterations it took and how many retransmits the sender issued.
Outcome run_lossy(std::uint64_t seed, taut::Impairments imp, int n, std::uint16_t window, bool sack,
                  int max_iters, std::chrono::milliseconds step) {
    taut::SimNet net(seed, imp);
    const auto a = ep(1);
    const auto b = ep(2);
    taut::Config cfg;
    cfg.window_pkts = window;

    taut::Session sa(net.endpoint(a), b, cfg);
    taut::Session sb(net.endpoint(b), a, cfg);
    sa.set_sack_enabled(sack);
    sb.set_sack_enabled(sack);

    int received = 0;
    sb.on_message([&](taut::Class, taut::ByteSpan) { ++received; });

    int next = 0;
    const auto feed = [&] {
        while (next < n) {
            std::array<std::byte, 4> payload{};
            const auto val = static_cast<std::uint32_t>(next);
            std::memcpy(payload.data(), &val, sizeof(val));
            if (!sa.send(taut::Class::ReliableOrdered, payload)) {
                break;
            }
            ++next;
        }
    };

    feed();
    int iter = 0;
    for (; iter < max_iters && received < n; ++iter) {
        net.advance(step);
        sa.poll();
        sb.poll();
        sa.tick();
        sb.tick();
        feed();
    }
    return Outcome{iter, sa.retransmits()};
}

} // namespace

// The §7 story on a controlled sim: SACK + fast-retransmit recovers a lossy transfer with
// fewer retransmits and no more iterations than cumulative-ack-only recovery.
TEST(Sack, FewerRetransmitsThanCumulativeOnly) {
    const taut::Impairments imp{.loss = 0.05, .delay = 10ms};
    const Outcome with = run_lossy(20240720, imp, 300, 32, /*sack=*/true, 200000, 1ms);
    const Outcome without = run_lossy(20240720, imp, 300, 32, /*sack=*/false, 200000, 1ms);

    EXPECT_LT(with.retransmits, without.retransmits);
    EXPECT_LE(with.iters, without.iters);
}
