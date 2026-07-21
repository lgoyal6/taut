#include "taut/rto.h"

#include <chrono>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

TEST(RttEstimator, InitialBeforeAnySample) {
    taut::RttEstimator e(25ms, 2000ms);
    EXPECT_FALSE(e.has_sample());
    EXPECT_EQ(e.rto(), 200ms); // initial guess before a measurement
}

TEST(RttEstimator, FirstSample) {
    taut::RttEstimator e(25ms, 2000ms);
    e.sample(100ms); // SRTT=100, RTTVAR=50, RTO=100+4*50=300
    EXPECT_TRUE(e.has_sample());
    EXPECT_NEAR(e.rto().count(), 300, 1);
}

TEST(RttEstimator, SecondSampleSmooths) {
    taut::RttEstimator e(25ms, 2000ms);
    e.sample(100ms);
    e.sample(100ms); // RTTVAR=37.5, SRTT=100, RTO=100+150=250
    EXPECT_NEAR(e.rto().count(), 250, 1);
}

TEST(RttEstimator, FloorClamps) {
    taut::RttEstimator e(25ms, 2000ms);
    e.sample(1ms); // RTO would be ~3ms; clamped up to the 25ms floor (the thesis knob)
    EXPECT_EQ(e.rto(), 25ms);
}

TEST(RttEstimator, CapClamps) {
    taut::RttEstimator e(25ms, 500ms);
    e.sample(1000ms); // RTO would be ~3000ms; clamped down to the 500ms cap
    EXPECT_EQ(e.rto(), 500ms);
}
