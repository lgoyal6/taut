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
                  int max_iters, std::chrono::milliseconds step) {
    taut::SimNet net(seed, imp);
    const auto a = ep(1);
    const auto b = ep(2);

    taut::Config cfg;
    cfg.window_pkts = window;

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
