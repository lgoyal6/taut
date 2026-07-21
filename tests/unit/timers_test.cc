#include "taut/timers.h"

#include <chrono>
#include <optional>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

namespace {
taut::TimerHeap::TimePoint base() {
    return taut::TimerHeap::TimePoint{} + 1000ms;
}
} // namespace

TEST(TimerHeap, PopsInDeadlineOrder) {
    taut::TimerHeap h;
    const auto t = base();
    const auto a = h.schedule(t + 30ms);
    const auto b = h.schedule(t + 10ms);
    const auto c = h.schedule(t + 20ms);
    EXPECT_EQ(h.pop_due(t + 100ms), b);
    EXPECT_EQ(h.pop_due(t + 100ms), c);
    EXPECT_EQ(h.pop_due(t + 100ms), a);
    EXPECT_FALSE(h.pop_due(t + 100ms).has_value());
    EXPECT_TRUE(h.empty());
}

TEST(TimerHeap, RespectsDueTime) {
    taut::TimerHeap h;
    const auto t = base();
    h.schedule(t + 50ms);
    EXPECT_FALSE(h.pop_due(t + 10ms).has_value()); // not due yet
    ASSERT_TRUE(h.next_deadline().has_value());
    EXPECT_EQ(*h.next_deadline(), t + 50ms);
    EXPECT_TRUE(h.pop_due(t + 50ms).has_value()); // due exactly at deadline
}

TEST(TimerHeap, CancelIsSkipped) {
    taut::TimerHeap h;
    const auto t = base();
    const auto a = h.schedule(t + 10ms);
    const auto b = h.schedule(t + 20ms);
    h.cancel(a);
    EXPECT_EQ(h.pop_due(t + 100ms), b); // a is skipped
    EXPECT_TRUE(h.empty());
}

TEST(TimerHeap, CancelUnknownIdIsNoop) {
    taut::TimerHeap h;
    h.cancel(999);
    EXPECT_TRUE(h.empty());
}

TEST(TimerHeap, NextDeadlinePrunesCancelledRoot) {
    taut::TimerHeap h;
    const auto t = base();
    const auto a = h.schedule(t + 5ms);
    h.schedule(t + 15ms);
    h.cancel(a);
    ASSERT_TRUE(h.next_deadline().has_value());
    EXPECT_EQ(*h.next_deadline(), t + 15ms);
}
