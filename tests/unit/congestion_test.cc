// Checks the controller declared in taut/congestion.h without real clocks.
// Covers fixed-window compatibility, recovery reductions and pacing deadlines.
// These deterministic controls complement the Session integration tests.
#include "taut/congestion.h"
#include <gtest/gtest.h>
using namespace std::chrono_literals;

TEST(Congestion, FixedWindowPreservesBaselineAndIgnoresLoss) {
    taut::CongestionControl c(taut::CongestionMode::FixedWindow, 64, 25ms);
    c.loss(1, 10);
    c.acknowledge(1000);
    EXPECT_EQ(c.window(), 64u);
    c.sent({});
    EXPECT_TRUE(c.ready({}));
}
TEST(Congestion, SlowStartThenAdditiveGrowthAndOneReductionPerFlight) {
    taut::CongestionControl c(taut::CongestionMode::PacedAimd, 64, 30ms);
    EXPECT_EQ(c.window(), 10u);
    c.acknowledge(10);
    EXPECT_EQ(c.window(), 20u);
    c.loss(0, 20);
    EXPECT_EQ(c.window(), 10u);
    c.acknowledge(9);
    EXPECT_EQ(c.window(), 10u);
    c.loss(5, 25);
    EXPECT_EQ(c.window(), 10u); // progress inside recovery is not new loss
    c.acknowledge(1);
    EXPECT_EQ(c.window(), 11u);
    c.loss(20, 30);
    EXPECT_EQ(c.window(), 5u);
    c.acknowledge(0);
    EXPECT_EQ(c.window(), 5u);
}
TEST(Congestion, PacerUsesRttAndDoesNotAccumulateIdleCredit) {
    taut::CongestionControl c(taut::CongestionMode::PacedAimd, 10, 30ms);
    const auto t = taut::CongestionControl::Clock::time_point{};
    EXPECT_TRUE(c.ready(t));
    c.sent(t);
    EXPECT_FALSE(c.ready(t + 2ms));
    EXPECT_TRUE(c.ready(t + 3ms));
    c.sent(t + 1s);
    EXPECT_FALSE(c.ready(t + 1s));
    c.sample_rtt(110ms); // smoothed RTT becomes 40ms
    c.sent(t + 2s);
    EXPECT_FALSE(c.ready(t + 2s + 3ms));
    EXPECT_TRUE(c.ready(t + 2s + 4ms));
}
