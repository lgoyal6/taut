#include "taut/session.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
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

std::uint32_t val_of(taut::ByteSpan p) {
    std::uint32_t v = 0;
    std::memcpy(&v, p.data(), std::min(sizeof(v), p.size()));
    return v;
}

std::array<std::byte, 4> payload_of(std::uint32_t v) {
    std::array<std::byte, 4> b{};
    std::memcpy(b.data(), &v, sizeof(v));
    return b;
}

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

// Class 0 (Unreliable): never retransmitted, deduplicated. Under heavy duplication + loss, the
// receiver delivers each message at most once (no dup) and the sender issues no retransmits;
// some messages may be lost (permitted).
TEST(Rx, UnreliableDedupNoRetransmit) {
    taut::SimNet net(1234,
                     taut::Impairments{.loss = 0.10, .dup = 0.40, .delay = 3ms, .jitter = 4ms});
    const auto a = ep(1);
    const auto b = ep(2);
    taut::Config cfg;
    taut::Session sa(net.endpoint(a), b, cfg);
    taut::Session sb(net.endpoint(b), a, cfg);

    std::vector<std::uint32_t> got;
    sb.on_message([&](taut::Class c, taut::ByteSpan p) {
        EXPECT_EQ(c, taut::Class::Unreliable);
        got.push_back(val_of(p));
    });

    const int n = 200;
    for (int i = 0; i < n; ++i) {
        ASSERT_TRUE(sa.send(taut::Class::Unreliable, payload_of(static_cast<std::uint32_t>(i))));
    }
    for (int iter = 0; iter < 5000; ++iter) {
        net.advance(2ms);
        sb.poll();
    }

    std::set<std::uint32_t> uniq(got.begin(), got.end());
    EXPECT_EQ(uniq.size(), got.size()); // no duplicate ever delivered
    EXPECT_EQ(sa.retransmits(), 0u);    // class 0 is never retransmitted
    EXPECT_LE(got.size(), static_cast<std::size_t>(n));
    EXPECT_GT(got.size(), static_cast<std::size_t>(n / 2)); // most got through
    for (auto v : uniq) {
        EXPECT_LT(v, static_cast<std::uint32_t>(n)); // only things we actually sent
    }
}

// Class 1 (ReliableUnordered): exactly-once under reorder + dup + loss, delivered as they
// arrive (any order). All N messages arrive exactly once.
TEST(Rx, ReliableUnorderedExactlyOnce) {
    taut::SimNet net(99,
                     taut::Impairments{.loss = 0.05, .dup = 0.03, .delay = 5ms, .jitter = 12ms});
    const auto a = ep(1);
    const auto b = ep(2);
    taut::Config cfg;
    cfg.window_pkts = 32;
    taut::Session sa(net.endpoint(a), b, cfg);
    taut::Session sb(net.endpoint(b), a, cfg);

    std::vector<std::uint32_t> got;
    sb.on_message([&](taut::Class c, taut::ByteSpan p) {
        EXPECT_EQ(c, taut::Class::ReliableUnordered);
        got.push_back(val_of(p));
    });

    const int n = 200;
    int next = 0;
    const auto feed = [&] {
        while (next < n && sa.send(taut::Class::ReliableUnordered,
                                   payload_of(static_cast<std::uint32_t>(next)))) {
            ++next;
        }
    };
    feed();
    for (int iter = 0; iter < 80000 && got.size() < static_cast<std::size_t>(n); ++iter) {
        net.advance(2ms);
        sa.poll();
        sb.poll();
        sa.tick();
        sb.tick();
        feed();
    }

    ASSERT_EQ(got.size(), static_cast<std::size_t>(n)); // exactly once, none lost
    std::set<std::uint32_t> uniq(got.begin(), got.end());
    EXPECT_EQ(uniq.size(), static_cast<std::size_t>(n)); // every distinct message present
}

// Class 1 delivers past a gap without waiting for reassembly (the latency win over class 2).
// Drop seq 0 once: seqs 1..4 are delivered on arrival, before seq 0 is recovered.
TEST(Rx, ReliableUnorderedDeliversPastGap) {
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
            c == taut::Class::ReliableUnordered && s == 0 && !dropped0) {
            dropped0 = true;
            return true;
        }
        return false;
    };

    std::vector<std::uint32_t> got;
    sb.on_message([&](taut::Class, taut::ByteSpan p) { got.push_back(val_of(p)); });

    const int n = 5;
    for (int i = 0; i < n; ++i) {
        ASSERT_TRUE(
            sa.send(taut::Class::ReliableUnordered, payload_of(static_cast<std::uint32_t>(i))));
    }
    // First, without reaching the RTO, deliver everything that got through: 1..4 arrive.
    for (int iter = 0; iter < 5; ++iter) {
        net.advance(1ms);
        sa.poll();
        sb.poll();
        sa.tick();
        sb.tick();
    }
    ASSERT_FALSE(got.empty());
    EXPECT_NE(got.front(), 0u); // delivered a later message before the gap was filled

    // Now let the gap recover (SACK fast-retransmit or RTO) and finish.
    for (int iter = 0; iter < 2000 && got.size() < static_cast<std::size_t>(n); ++iter) {
        net.advance(1ms);
        sa.poll();
        sb.poll();
        sa.tick();
        sb.tick();
    }
    ASSERT_EQ(got.size(), static_cast<std::size_t>(n));
    std::set<std::uint32_t> uniq(got.begin(), got.end());
    EXPECT_EQ(uniq.size(), static_cast<std::size_t>(n));
}

// The class-0 separate seq space (DESIGN-codec #3): losing every unreliable packet must not
// stall reliable, ordered delivery — the two never share a cumulative-ack sequence.
TEST(Rx, UnreliableLossDoesNotStallReliable) {
    tlink::TestLink net;
    const auto a = ep(1);
    const auto b = ep(2);
    taut::Config cfg;
    cfg.window_pkts = 16;
    taut::Session sa(net.endpoint(a), b, cfg);
    taut::Session sb(net.endpoint(b), a, cfg);

    net.drop = [&](const taut::Endpoint& from, const taut::Endpoint&,
                   std::span<const std::byte> d) {
        taut::PacketType t{};
        taut::Class c{};
        std::uint32_t s = 0;
        // Drop ALL class-0 datagrams from A; reliable traffic is untouched.
        return from == a && peek(d, t, c, s) && t == taut::PacketType::Data &&
               c == taut::Class::Unreliable;
    };

    std::vector<std::uint32_t> reliable;
    int unreliable = 0;
    sb.on_message([&](taut::Class c, taut::ByteSpan p) {
        if (c == taut::Class::Unreliable) {
            ++unreliable;
        } else {
            reliable.push_back(val_of(p));
        }
    });

    const int n = 20;
    for (int i = 0; i < n; ++i) {
        ASSERT_TRUE(sa.send(taut::Class::Unreliable, payload_of(1000))); // will be dropped
        ASSERT_TRUE(
            sa.send(taut::Class::ReliableOrdered, payload_of(static_cast<std::uint32_t>(i))));
    }
    for (int iter = 0; iter < 5000 && reliable.size() < static_cast<std::size_t>(n); ++iter) {
        net.advance(1ms);
        sa.poll();
        sb.poll();
        sa.tick();
        sb.tick();
    }

    ASSERT_EQ(reliable.size(), static_cast<std::size_t>(n)); // reliable fully delivered
    for (int i = 0; i < n; ++i) {
        EXPECT_EQ(reliable[static_cast<std::size_t>(i)], static_cast<std::uint32_t>(i)); // in order
    }
    EXPECT_EQ(unreliable, 0); // every unreliable packet was lost, yet reliable never stalled
}
