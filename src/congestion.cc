// Implements the controller declared in taut/congestion.h.
// Session supplies new ACKs, loss boundaries and Karn-safe RTT samples.
// New data is paced without idle burst credit; recovery uses existing timers.
#include "taut/congestion.h"
#include <algorithm>

namespace taut {
CongestionControl::CongestionControl(CongestionMode mode, std::size_t maximum,
                                     std::chrono::milliseconds initial_rtt)
    : mode_(mode), maximum_(std::max<std::size_t>(1, maximum)),
      window_(mode == CongestionMode::FixedWindow ? maximum_ : std::min<std::size_t>(10, maximum_)),
      threshold_(maximum_), srtt_(std::max(initial_rtt, std::chrono::milliseconds(1))) {}

void CongestionControl::acknowledge(std::size_t packets) {
    if (mode_ == CongestionMode::FixedWindow)
        return;
    while (packets-- != 0 && window_ < maximum_) {
        if (window_ < threshold_)
            ++window_;
        else if (++credit_ >= window_) {
            ++window_;
            credit_ = 0;
        }
    }
}

void CongestionControl::loss(std::uint32_t sequence, std::uint32_t next_sequence) {
    if (mode_ == CongestionMode::FixedWindow)
        return;
    // The reliable sequence space does not wrap within a supported session.
    if (recovering_ && sequence < recovery_end_)
        return;
    threshold_ = std::max<std::size_t>(1, window_ / 2);
    window_ = threshold_;
    credit_ = 0;
    recovery_end_ = next_sequence;
    recovering_ = true;
}

void CongestionControl::sample_rtt(std::chrono::milliseconds sample) {
    if (mode_ == CongestionMode::FixedWindow)
        return; // the fixed policy never reads pacing state
    const auto bounded = std::max(sample, std::chrono::milliseconds(1));
    srtt_ = (srtt_ * 7 + bounded) / 8;
}
bool CongestionControl::ready(Clock::time_point now) const {
    return mode_ == CongestionMode::FixedWindow || now >= next_send_;
}
void CongestionControl::sent(Clock::time_point now) {
    if (mode_ == CongestionMode::FixedWindow)
        return;
    // Idle time earns no burst credit. Recovery retransmits retain the existing RTO path.
    next_send_ = now + srtt_ / static_cast<std::int64_t>(window_);
}
} // namespace taut
