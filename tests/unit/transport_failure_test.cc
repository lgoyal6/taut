// Uses test_link.h to trigger bounded Session failure and recovery paths.
// Checks ordered delivery, MTU limits, peer filtering and timer boundaries.
// Keeps unsafe same-endpoint reuse as an explicit adverse control.
#include "taut/session.h"
#include "test_link.h"
#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <limits>
using namespace std::chrono_literals;
namespace {
taut::Endpoint ep(std::uint16_t p) {
    taut::Endpoint e{};
    e.addr_be = 1;
    e.port_be = p;
    return e;
}
std::array<std::byte, 4> value(std::uint32_t n) {
    std::array<std::byte, 4> p{};
    std::memcpy(p.data(), &n, 4);
    return p;
}
} // namespace
TEST(TransportFailures, BurstLossDelayedAckSlowConsumerAndRecoveryInBothModes) {
    for (auto mode : {taut::CongestionMode::FixedWindow, taut::CongestionMode::PacedAimd}) {
        tlink::TestLink net;
        taut::Config cfg;
        cfg.congestion = mode;
        cfg.window_pkts = 16;
        taut::Session a(net.endpoint(ep(1)), ep(2), cfg), b(net.endpoint(ep(2)), ep(1), cfg);
        std::vector<std::uint32_t> got;
        unsigned drops = 0;
        b.on_message([&](taut::Class, taut::ByteSpan p) {
            std::uint32_t n = 0;
            std::memcpy(&n, p.data(), 4);
            got.push_back(n);
        });
        net.drop = [&](auto from, auto, auto bytes) {
            taut::Packet p{};
            if (taut::decode(bytes, p) != taut::DecodeError::Ok)
                return false;
            if (from == ep(1) && p.type == taut::PacketType::Data &&
                net.now() < taut::CongestionControl::Clock::time_point{} + 100ms) {
                ++drops;
                return true;
            }
            return false;
        };
        b.set_receiving(false);
        std::uint32_t next = 0;
        for (int i = 0; i < 10000 && got.size() < 100; ++i) {
            while (next < 100 && a.send(taut::Class::ReliableOrdered, value(next)))
                ++next;
            net.advance(2ms);
            a.poll();
            a.tick();
            // Polling the receiver every 40ms creates delayed ACKs, then a 2ms RTT phase.
            if (i > 200 || i % 20 == 0) {
                b.poll();
                b.tick();
            }
            if (i == 300)
                b.set_receiving(true);
            ASSERT_LE(a.in_flight(), cfg.window_pkts);
            ASSERT_LE(b.rcv_occupancy(), cfg.window_pkts);
        }
        EXPECT_GT(drops, 0u);
        ASSERT_EQ(got.size(), 100u);
        for (std::uint32_t i = 0; i < 100; ++i)
            EXPECT_EQ(got[i], i);
        EXPECT_GT(a.retransmits(), 0u);
    }
}
TEST(TransportFailures, ConfiguredMtuRejectsOversizeAndAcceptsBoundary) {
    tlink::TestLink net;
    taut::Config cfg;
    cfg.mtu_payload = 128;
    taut::Session a(net.endpoint(ep(1)), ep(2), cfg);
    std::array<std::byte, 128> valid{};
    std::array<std::byte, 129> invalid{};
    EXPECT_FALSE(a.send(taut::Class::ReliableOrdered, invalid));
    EXPECT_FALSE(a.send(taut::Class::Unreliable, invalid));
    EXPECT_TRUE(a.send(taut::Class::ReliableOrdered, valid));
}
TEST(TransportFailures, WideMonotonicTimersCross32BitMilliseconds) {
    taut::TimerHeap timers;
    const auto near = taut::TimerHeap::TimePoint{} +
                      std::chrono::milliseconds(std::numeric_limits<std::uint32_t>::max() - 1ULL);
    auto id = timers.schedule(near + 10ms);
    EXPECT_FALSE(timers.pop_due(near + 9ms));
    EXPECT_EQ(timers.pop_due(near + 10ms), id);
}
TEST(TransportFailures, EndpointReuseHasNoEpochNegativeControl) {
    tlink::TestLink net;
    std::vector<std::byte> held;
    net.drop = [&](auto from, auto, auto bytes) {
        if (from == ep(1))
            held.assign(bytes.begin(), bytes.end());
        return true;
    };
    {
        taut::Session old(net.endpoint(ep(1)), ep(2), {});
        ASSERT_TRUE(old.send(taut::Class::ReliableOrdered, value(99)));
    }
    ASSERT_FALSE(held.empty());
    net.drop = {};
    taut::Session restarted(net.endpoint(ep(2)), ep(1), {});
    unsigned delivered = 0;
    restarted.on_message([&](auto, auto) { ++delivered; });
    net.inject(ep(1), ep(2), held);
    restarted.poll();
    EXPECT_EQ(delivered, 1u); // retained adverse control: old data is accepted on endpoint reuse
}
TEST(TransportFailures, FreshEndpointReconnectRejectsOldPeerAndDeliversNewSession) {
    tlink::TestLink net;
    taut::Session a(net.endpoint(ep(3)), ep(4), {}), b(net.endpoint(ep(4)), ep(3), {});
    std::vector<std::uint32_t> got;
    b.on_message([&](auto, auto p) {
        std::uint32_t n = 0;
        std::memcpy(&n, p.data(), 4);
        got.push_back(n);
    });
    taut::Packet old{};
    old.type = taut::PacketType::Data;
    old.cls = taut::Class::ReliableOrdered;
    // Keep the payload alive while encoding the stale datagram.
    auto stale_payload = value(99);
    old.payload = stale_payload;
    std::array<std::byte, taut::kMaxDatagram> bytes{};
    auto n = taut::encode(old, bytes);
    net.inject(ep(1), ep(4), std::span<const std::byte>(bytes.data(), n));
    b.poll();
    EXPECT_TRUE(got.empty());
    ASSERT_TRUE(a.send(taut::Class::ReliableOrdered, value(7)));
    b.poll();
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], 7u);
}
