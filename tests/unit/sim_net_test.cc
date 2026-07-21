#include "taut/sim_net.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

namespace {

taut::Endpoint ep(std::uint16_t port) {
    taut::Endpoint e{};
    e.addr_be = 1;
    e.port_be = port;
    return e;
}

std::span<const std::byte> one_byte(std::byte& b) {
    return {&b, 1};
}

// Drain every currently-deliverable datagram from `t`, returning the first payload byte of
// each in delivery order.
std::vector<std::uint8_t> drain(taut::UdpTransport& t) {
    std::vector<std::uint8_t> out;
    std::array<std::byte, 256> buf{};
    while (auto r = t.recv(buf)) {
        out.push_back(std::to_integer<std::uint8_t>(buf[0]));
    }
    return out;
}

} // namespace

TEST(SimNet, NoLossDeliversAllInOrder) {
    taut::SimNet net(1);
    auto& a = net.endpoint(ep(1));
    auto& b = net.endpoint(ep(2));
    for (int i = 0; i < 10; ++i) {
        std::byte payload{static_cast<std::uint8_t>(i)};
        a.send(ep(2), one_byte(payload));
    }
    const auto got = drain(b);
    ASSERT_EQ(got.size(), 10u);
    for (std::uint8_t i = 0; i < 10; ++i) {
        EXPECT_EQ(got[i], i); // delay 0 => delivery order == send order (seqno tiebreak)
    }
}

TEST(SimNet, DelayHoldsUntilClockAdvances) {
    taut::SimNet net(1, taut::Impairments{.delay = 30ms});
    auto& a = net.endpoint(ep(1));
    auto& b = net.endpoint(ep(2));
    std::byte payload{0x7};
    a.send(ep(2), one_byte(payload));

    std::array<std::byte, 256> buf{};
    EXPECT_FALSE(b.recv(buf).has_value()); // not due yet
    net.advance(29ms);
    EXPECT_FALSE(b.recv(buf).has_value()); // still not due
    net.advance(1ms);
    ASSERT_TRUE(b.recv(buf).has_value()); // due at +30ms
}

TEST(SimNet, LossIsReproducibleFromSeed) {
    const auto run = [](std::uint64_t seed) {
        taut::SimNet net(seed, taut::Impairments{.loss = 0.5});
        auto& a = net.endpoint(ep(1));
        net.endpoint(ep(2));
        for (int i = 0; i < 200; ++i) {
            std::byte payload{static_cast<std::uint8_t>(i)};
            a.send(ep(2), one_byte(payload));
        }
        return drain(net.endpoint(ep(2)));
    };
    const auto first = run(12345);
    const auto second = run(12345);
    EXPECT_EQ(first, second);     // same seed => identical run
    EXPECT_GT(first.size(), 60u); // ~50% of 200, generously bounded
    EXPECT_LT(first.size(), 140u);
    EXPECT_NE(run(999), first); // a different seed diverges
}

TEST(SimNet, PayloadRoundTripsIntact) {
    taut::SimNet net(7);
    auto& a = net.endpoint(ep(1));
    auto& b = net.endpoint(ep(2));
    const std::array<std::byte, 4> sent{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE},
                                        std::byte{0xEF}};
    a.send(ep(2), sent);
    std::array<std::byte, 256> buf{};
    const auto r = b.recv(buf);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size, 4u);
    EXPECT_EQ(std::memcmp(buf.data(), sent.data(), 4), 0);
}
