#include "taut/session.h"

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

using namespace std::chrono_literals;

namespace {

taut::Endpoint ep(std::uint16_t port) {
    taut::Endpoint e{};
    e.addr_be = 1;
    e.port_be = port;
    return e;
}

// Run a stop-and-wait (window=1) transfer of `n` ordered messages A->B over a lossy SimNet,
// and assert every message is delivered exactly once, in send order.
void run_transfer(std::uint64_t seed, double loss, int n) {
    taut::SimNet net(seed, taut::Impairments{.loss = loss, .delay = 5ms});
    const auto a = ep(1);
    const auto b = ep(2);

    taut::Config cfg;
    cfg.window_pkts = 1; // stop-and-wait

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
    for (int iter = 0; iter < 20000 && static_cast<int>(received.size()) < n; ++iter) {
        net.advance(5ms);
        sa.poll();
        sb.poll();
        sa.tick();
        sb.tick();
        feed();
        EXPECT_LE(sa.in_flight(), cfg.window_pkts); // invariant 3
    }

    ASSERT_EQ(received.size(), static_cast<std::size_t>(n)); // no loss, no stall (inv. 1, 5)
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(n); ++i) {
        EXPECT_EQ(received[i], i); // in order, exactly once (inv. 1, 2)
    }
}

} // namespace

TEST(Session, StopAndWaitReliableAt5PercentLoss) {
    run_transfer(2024, 0.05, 100);
}

TEST(Session, StopAndWaitReliableAt20PercentLoss) {
    run_transfer(7, 0.20, 100);
}

TEST(Session, StopAndWaitCleanLink) {
    run_transfer(1, 0.0, 50);
}
