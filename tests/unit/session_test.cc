// Checks reliable Session delivery through the seeded SimNet transport.
// Congestion tests cover the paced option alongside fixed-window behavior.
// Each transfer checks ordered, exactly-once delivery and bounded in-flight state.
#include "taut/session.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include <gtest/gtest.h>

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

// Transfer `n` ordered messages A->B over a lossy/impaired SimNet with the given send
// window, then assert every message arrives exactly once, in send order (invariants 1,2,3,5).
void run_transfer(std::uint64_t seed, taut::Impairments imp, std::uint16_t window, int n,
                  int max_iters, std::chrono::milliseconds step,
                  taut::CongestionMode mode = taut::CongestionMode::FixedWindow) {
    taut::SimNet net(seed, imp);
    const auto a = ep(1);
    const auto b = ep(2);

    taut::Config cfg;
    cfg.window_pkts = window;
    cfg.congestion = mode;

    taut::Session sa(net.endpoint(a), b, cfg);
    taut::Session sb(net.endpoint(b), a, cfg);

    std::vector<std::uint32_t> received;
    sb.on_message([&](taut::Class, taut::ByteSpan p) {
        std::uint32_t v = 0;
        std::memcpy(&v, p.data(), std::min(sizeof(v), p.size()));
        received.push_back(v);
    });

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
    for (int iter = 0; iter < max_iters && static_cast<int>(received.size()) < n; ++iter) {
        net.advance(step);
        sa.poll();
        sb.poll();
        sa.tick();
        sb.tick();
        feed();
        EXPECT_LE(sa.in_flight(), static_cast<std::size_t>(window)); // invariant 3
    }

    ASSERT_EQ(received.size(), static_cast<std::size_t>(n)); // no loss, no stall (inv. 1, 5)
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(n); ++i) {
        EXPECT_EQ(received[i], i); // in order, exactly once (inv. 1, 2)
    }
}

TEST(SessionCongestion, GrowsOnlyAsReliablePacketsAreAcknowledged) {
    taut::SimNet net(17, {});
    const auto a = ep(11);
    const auto b = ep(12);
    taut::Config cfg;
    cfg.window_pkts = 16;
    cfg.congestion = taut::CongestionMode::PacedAimd;
    taut::Session sender(net.endpoint(a), b, cfg);
    taut::Session receiver(net.endpoint(b), a, cfg);
    std::array<std::byte, 1> payload{};

    EXPECT_EQ(sender.congestion_window(), 10u);
    for (int i = 0; i < 16; ++i) {
        ASSERT_TRUE(sender.send(taut::Class::ReliableOrdered, payload));
    }
    EXPECT_EQ(sender.in_flight(), 1u); // the pacer earns no initial burst credit

    for (int i = 0; i < 4; ++i) {
        net.advance(10ms);
        receiver.poll();
        sender.poll();
        sender.tick();
        EXPECT_LE(sender.in_flight(), sender.congestion_window());
    }
    EXPECT_GT(sender.congestion_window(), 10u);
    EXPECT_LE(sender.congestion_window(), cfg.window_pkts);
}

} // namespace

TEST(Session, StopAndWaitReliableAt5PercentLoss) {
    run_transfer(2024, taut::Impairments{.loss = 0.05, .delay = 5ms}, 1, 100, 20000, 5ms);
}

TEST(Session, StopAndWaitReliableAt20PercentLoss) {
    run_transfer(7, taut::Impairments{.loss = 0.20, .delay = 5ms}, 1, 100, 20000, 5ms);
}

TEST(Session, StopAndWaitCleanLink) {
    run_transfer(1, taut::Impairments{}, 1, 50, 20000, 5ms);
}

// Window 64: real pipelining, plus reorder (jitter), duplication, and loss all at once.
TEST(Session, SlidingWindowReorderDupLoss) {
    run_transfer(99, taut::Impairments{.loss = 0.05, .dup = 0.02, .delay = 5ms, .jitter = 8ms}, 64,
                 300, 80000, 2ms);
}

// Seeded property-style check: every sequence must arrive once and in order while bounded.
TEST(Session, FixedAndPacedPropertiesAcrossLossReorderAndDuplicationSeeds) {
    for (auto mode : {taut::CongestionMode::FixedWindow, taut::CongestionMode::PacedAimd}) {
        for (std::uint64_t seed = 301; seed < 321; ++seed) {
            SCOPED_TRACE(seed);
            run_transfer(seed,
                         taut::Impairments{.loss = 0.10, .dup = 0.05, .delay = 5ms, .jitter = 12ms},
                         16, 100, 30000, 2ms, mode);
        }
    }
}

TEST(SessionCongestion, DuplicateSackAndCumulativeAckCreditEachPacketOnce) {
    tlink::TestLink net;
    const auto a = ep(41), b = ep(42);
    taut::Config cfg;
    cfg.window_pkts = 16;
    cfg.congestion = taut::CongestionMode::PacedAimd;
    taut::Session sender(net.endpoint(a), b, cfg);
    std::array<std::byte, 1> payload{};
    for (int i = 0; i < 3; ++i)
        ASSERT_TRUE(sender.send(taut::Class::ReliableOrdered, payload));
    for (int i = 0; i < 2; ++i) {
        net.advance(3ms);
        sender.tick();
    }
    ASSERT_EQ(sender.in_flight(), 3u);
    auto inject_ack = [&](std::uint32_t generation, std::uint32_t cumulative, bool sack) {
        taut::Packet p{};
        p.type = taut::PacketType::Ack;
        p.seq = generation;
        p.cum_ack = cumulative;
        p.adv_window = 16;
        if (sack) {
            p.flags = static_cast<std::uint8_t>(taut::Flag::SackPresent);
            p.sack = 3;
        }
        std::array<std::byte, taut::kMaxDatagram> bytes{};
        const auto size = taut::encode(p, bytes);
        net.inject(b, a, std::span<const std::byte>(bytes.data(), size));
        sender.poll();
    };
    inject_ack(1, 0, true);
    EXPECT_EQ(sender.congestion_window(), 12u);
    inject_ack(2, 0, true);
    EXPECT_EQ(sender.congestion_window(), 12u);
    // The cumulative ACK adds only the missing packet, not the two already SACKed.
    inject_ack(3, 3, false);
    EXPECT_EQ(sender.congestion_window(), 13u);
    EXPECT_EQ(sender.in_flight(), 0u);
}

TEST(SessionCongestion, RetransmissionByteCounterUsesTransportAcceptedBytes) {
    tlink::TestLink net;
    const auto a = ep(51), b = ep(52);
    taut::Session sender(net.endpoint(a), b, {});
    std::size_t sent_size = 0;
    net.drop = [&](auto from, auto, auto bytes) {
        if (from == a)
            sent_size = bytes.size();
        return true;
    };
    std::array<std::byte, 7> payload{};
    ASSERT_TRUE(sender.send(taut::Class::ReliableOrdered, payload));
    EXPECT_EQ(sender.retransmitted_bytes(), 0u);
    // Before an RTT sample the existing estimator starts at 200ms, above its floor.
    net.advance(199ms);
    sender.tick();
    EXPECT_EQ(sender.retransmits(), 0u);
    net.advance(1ms);
    sender.tick();
    EXPECT_EQ(sender.retransmits(), 1u);
    EXPECT_EQ(sender.retransmitted_bytes(), sent_size);
}
