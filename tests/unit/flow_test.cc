#include "taut/session.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
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

std::array<std::byte, 4> payload_of(std::uint32_t v) {
    std::array<std::byte, 4> b{};
    std::memcpy(b.data(), &v, sizeof(v));
    return b;
}

std::uint32_t val_of(taut::ByteSpan p) {
    std::uint32_t v = 0;
    std::memcpy(&v, p.data(), std::min(sizeof(v), p.size()));
    return v;
}

// Decode a datagram's type and advertised window for a drop hook.
bool peek(std::span<const std::byte> d, taut::PacketType& type, std::uint16_t& adv) {
    taut::Packet p{};
    if (taut::decode(d, p) != taut::DecodeError::Ok) {
        return false;
    }
    type = p.type;
    adv = p.adv_window;
    return true;
}

} // namespace

// A receiver that stops draining fills its buffer, drives the advertised window to zero, and
// stalls the sender - then resumes with no deadlock, no buffer overflow, and every message
// delivered in order. Invariants 1, 2, 3, 5 (§6.4).
TEST(Flow, StalledReceiverNoDeadlockNoOverflow) {
    taut::SimNet net(2026, taut::Impairments{.loss = 0.02, .delay = 5ms, .jitter = 6ms});
    const auto a = ep(1);
    const auto b = ep(2);
    taut::Config cfg;
    cfg.window_pkts = 8;
    const std::size_t capacity = cfg.window_pkts;

    taut::Session sa(net.endpoint(a), b, cfg);
    taut::Session sb(net.endpoint(b), a, cfg);

    std::vector<std::uint32_t> got;
    sb.on_message([&](taut::Class, taut::ByteSpan p) { got.push_back(val_of(p)); });

    const int n = 50;
    int next = 0;
    const auto feed = [&] {
        while (next < n && sa.send(taut::Class::ReliableOrdered,
                                   payload_of(static_cast<std::uint32_t>(next)))) {
            ++next;
        }
    };

    const auto step_and_check = [&] {
        net.advance(5ms);
        sa.poll();
        sb.poll();
        sa.tick();
        sb.tick();
        feed();
        EXPECT_LE(sb.rcv_occupancy(), capacity); // no overflow (invariant 3, the safety half)
        EXPECT_LE(sa.in_flight(),
                  std::min<std::size_t>(cfg.window_pkts, sa.peer_adv_window())); // invariant 3
    };

    // Phase 1: receiver stalled ~5 s of sim time. Nothing should be delivered to the app; the
    // sender should quiesce against a zero window without spinning the buffer past capacity.
    sb.set_receiving(false);
    feed();
    for (int iter = 0; iter < 1000; ++iter) {
        step_and_check();
    }
    EXPECT_EQ(got.size(), 0u); // app never saw anything while stalled
    EXPECT_LE(sb.rcv_occupancy(), capacity);

    // Phase 2: app resumes draining; the transfer must complete.
    sb.set_receiving(true);
    for (int iter = 0; iter < 40000 && got.size() < static_cast<std::size_t>(n); ++iter) {
        step_and_check();
    }

    ASSERT_EQ(got.size(), static_cast<std::size_t>(n)); // no deadlock, nothing lost
    for (int i = 0; i < n; ++i) {
        EXPECT_EQ(got[static_cast<std::size_t>(i)], static_cast<std::uint32_t>(i)); // in order
    }
}

// The advertised window shrinks to zero as the stalled buffer fills, and reopens when the app
// drains - visible on both the receiver's side and the sender's belief.
TEST(Flow, AdvWindowShrinksToZeroAndReopens) {
    tlink::TestLink net;
    const auto a = ep(1);
    const auto b = ep(2);
    taut::Config cfg;
    cfg.window_pkts = 4;
    taut::Session sa(net.endpoint(a), b, cfg);
    taut::Session sb(net.endpoint(b), a, cfg);

    std::vector<std::uint32_t> got;
    sb.on_message([&](taut::Class, taut::ByteSpan p) { got.push_back(val_of(p)); });

    sb.set_receiving(false);
    const int n = 12;
    int next = 0;
    const auto feed = [&] {
        while (next < n && sa.send(taut::Class::ReliableOrdered,
                                   payload_of(static_cast<std::uint32_t>(next)))) {
            ++next;
        }
    };
    feed();
    for (int iter = 0; iter < 6; ++iter) {
        net.advance(1ms);
        sa.poll();
        sb.poll();
        sa.tick();
        sb.tick();
        feed();
    }

    EXPECT_EQ(sb.adv_window(), 0u);      // receiver buffer full
    EXPECT_EQ(sa.peer_adv_window(), 0u); // sender has learned it
    EXPECT_EQ(got.size(), 0u);

    sb.set_receiving(true);
    for (int iter = 0; iter < 6; ++iter) {
        net.advance(1ms);
        sa.poll();
        sb.poll();
        sa.tick();
        sb.tick();
        feed();
    }
    EXPECT_GT(sb.adv_window(), 0u);
    EXPECT_GT(sa.peer_adv_window(), 0u);
}

// Persist-probe liveness (§5.6): even when the window-reopening ack is lost, the sender's
// persist timer re-probes and recovers - no deadlock.
TEST(Flow, ZeroWindowProbeRecoversFromLostReopen) {
    tlink::TestLink net;
    const auto a = ep(1);
    const auto b = ep(2);
    taut::Config cfg;
    cfg.window_pkts = 4;
    taut::Session sa(net.endpoint(a), b, cfg);
    taut::Session sb(net.endpoint(b), a, cfg);

    bool drop_reopen = false;
    net.drop = [&](const taut::Endpoint& from, const taut::Endpoint&,
                   std::span<const std::byte> d) {
        taut::PacketType t{};
        std::uint16_t adv = 0;
        if (drop_reopen && from == b && peek(d, t, adv) && adv > 0) {
            drop_reopen = false; // drop exactly the first window-reopening ack
            return true;
        }
        return false;
    };

    std::vector<std::uint32_t> got;
    sb.on_message([&](taut::Class, taut::ByteSpan p) { got.push_back(val_of(p)); });

    sb.set_receiving(false);
    const int n = 12;
    int next = 0;
    const auto feed = [&] {
        while (next < n && sa.send(taut::Class::ReliableOrdered,
                                   payload_of(static_cast<std::uint32_t>(next)))) {
            ++next;
        }
    };
    feed();
    for (int iter = 0; iter < 8; ++iter) {
        net.advance(5ms);
        sa.poll();
        sb.poll();
        sa.tick();
        sb.tick();
        feed();
    }
    ASSERT_EQ(sa.peer_adv_window(), 0u); // stalled against a zero window, in-flight drained

    // Resume, but lose the reopening ack. Recovery must come from the persist probe.
    drop_reopen = true;
    sb.set_receiving(true);
    for (int iter = 0; iter < 4000 && got.size() < static_cast<std::size_t>(n); ++iter) {
        net.advance(20ms); // advance enough that the persist timer (≤ 1 s) fires
        sa.poll();
        sb.poll();
        sa.tick();
        sb.tick();
        feed();
    }
    EXPECT_FALSE(drop_reopen);                          // the reopening ack really was dropped
    ASSERT_EQ(got.size(), static_cast<std::size_t>(n)); // yet the transfer completed
    for (int i = 0; i < n; ++i) {
        EXPECT_EQ(got[static_cast<std::size_t>(i)], static_cast<std::uint32_t>(i));
    }
}
