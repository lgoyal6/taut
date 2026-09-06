// Per-poll drain bound (Config::max_recv_per_poll, SwimConfig::max_recv_per_poll).
//
// poll() used to drain the socket until it returned EAGAIN. Because tick() - which
// fires RTO retransmits and the SWIM failure detector - only runs after poll()
// returns, a peer that keeps datagrams arriving could hold the single-threaded loop
// inside poll() indefinitely and starve every timer and every other peer behind it.
//
// These tests pin the bound. Without it the first assertion in each test fails:
// one poll() would deliver the whole flood.

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "taut/config.h"
#include "taut/session.h"
#include "taut/sim_net.h"
#include "taut/swim.h"
#include "taut/types.h"

using namespace std::chrono_literals;

namespace {

taut::Endpoint ep(std::uint16_t port) {
    taut::Endpoint e{};
    e.addr_be = 1;
    e.port_be = port;
    return e;
}

// Unreliable messages are fire-and-forget and never windowed, so a sender can put an
// arbitrary burst on the wire in one go - exactly the flood the bound has to survive.
TEST(DrainBound, OnePollStopsAtTheBoundAndReportsMoreWork) {
    taut::SimNet net(1234);
    const auto a = ep(1);
    const auto b = ep(2);

    taut::Config cfg;
    cfg.max_recv_per_poll = 16;

    taut::Session sa(net.endpoint(a), b, cfg);
    taut::Session sb(net.endpoint(b), a, cfg);

    int delivered = 0;
    sb.on_message([&](taut::Class, taut::ByteSpan) { ++delivered; });

    constexpr int kFlood = 200;
    for (int i = 0; i < kFlood; ++i) {
        const std::uint32_t v = static_cast<std::uint32_t>(i);
        std::array<std::byte, sizeof(v)> payload{};
        std::memcpy(payload.data(), &v, sizeof(v));
        ASSERT_TRUE(sa.send(taut::Class::Unreliable, payload));
    }
    net.advance(1ms);

    // One poll must stop at the bound and say so, instead of consuming all 200.
    const bool more = sb.poll();
    EXPECT_TRUE(more) << "poll() should report that it stopped at the bound";
    EXPECT_LE(delivered, static_cast<int>(cfg.max_recv_per_poll))
        << "poll() drained past Config::max_recv_per_poll";

    // The caller gets to run its timers, then keep draining. Nothing is dropped by the
    // bound itself: repeated polls deliver the whole flood.
    int guard = 0;
    while (sb.poll() && ++guard < 1000) {
        sb.tick();
    }
    sb.poll();
    EXPECT_EQ(delivered, kFlood) << "the bound must defer datagrams, never discard them";
}

// The point of the bound: tick() actually gets to run while a flood is in progress.
TEST(DrainBound, TimersStillRunWhileAFloodIsQueued) {
    taut::SimNet net(99);
    const auto a = ep(1);
    const auto b = ep(2);

    taut::Config cfg;
    cfg.max_recv_per_poll = 8;

    taut::Session sa(net.endpoint(a), b, cfg);
    taut::Session sb(net.endpoint(b), a, cfg);
    sb.on_message([](taut::Class, taut::ByteSpan) {});

    for (int i = 0; i < 300; ++i) {
        std::array<std::byte, 4> payload{};
        ASSERT_TRUE(sa.send(taut::Class::Unreliable, payload));
    }
    net.advance(1ms);

    // Interleave poll()/tick() the way a real loop does. Each poll must return control
    // so tick() can run; an unbounded poll() would consume the flood before the first
    // tick() ever executed.
    int polls = 0;
    while (sb.poll() && polls < 1000) {
        ++polls;
        sb.tick(); // must be reachable mid-flood
    }
    EXPECT_GT(polls, 1) << "the flood should have taken more than one bounded poll";
}

TEST(DrainBound, SwimPollIsBoundedToo) {
    taut::SimNet net(7);
    const auto a = ep(1);
    const auto b = ep(2);

    taut::SwimConfig scfg;
    scfg.max_recv_per_poll = 4;

    taut::Swim sw(net.endpoint(b), b, scfg, /*seed=*/1);

    // A burst of raw datagrams at the SWIM endpoint. They need not be well-formed SWIM
    // packets: the bound governs how many are *consumed* per poll, before any parsing
    // verdict, which is precisely the property that keeps tick() reachable.
    auto& sender = net.endpoint(a);
    std::array<std::byte, 32> junk{};
    for (int i = 0; i < 100; ++i) {
        sender.send(b, junk);
    }
    net.advance(1ms);

    EXPECT_TRUE(sw.poll()) << "Swim::poll() should stop at SwimConfig::max_recv_per_poll";
}

} // namespace
