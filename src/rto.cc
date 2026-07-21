#include "taut/rto.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace taut {
namespace {
constexpr double kGranularityMs = 1.0;  // clock granularity G
constexpr double kInitialRtoMs = 200.0; // before the first sample (deviation from RFC's 1 s;
                                        // this is a low-latency mesh — see docs/DESIGN-rto.md)
} // namespace

RttEstimator::RttEstimator(std::chrono::milliseconds floor, std::chrono::milliseconds max)
    : floor_(floor), max_(max) {}

void RttEstimator::sample(std::chrono::milliseconds rtt) {
    const double r = static_cast<double>(rtt.count());
    if (!has_) {
        srtt_ = r;
        rttvar_ = r / 2.0;
        has_ = true;
    } else {
        rttvar_ = 0.75 * rttvar_ + 0.25 * std::abs(srtt_ - r);
        srtt_ = 0.875 * srtt_ + 0.125 * r;
    }
}

std::chrono::milliseconds RttEstimator::rto() const {
    double rto_ms = has_ ? srtt_ + std::max(kGranularityMs, 4.0 * rttvar_) : kInitialRtoMs;
    rto_ms =
        std::clamp(rto_ms, static_cast<double>(floor_.count()), static_cast<double>(max_.count()));
    return std::chrono::milliseconds(static_cast<std::int64_t>(std::llround(rto_ms)));
}

} // namespace taut
