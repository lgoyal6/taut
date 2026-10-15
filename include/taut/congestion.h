// Declares the congestion policy used by Session in session.h.
// FixedWindow preserves the original policy; PacedAimd separates pacing from delivery.
// Packet units match the one-message-per-datagram wire format.
#ifndef TAUT_CONGESTION_H
#define TAUT_CONGESTION_H

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace taut {

enum class CongestionMode : std::uint8_t { FixedWindow, PacedAimd };

// Packet accounting is appropriate here because every reliable datagram fits one MTU.
// Slow start, then one packet per window of new ACKs; halve once per recovery flight.
// This is a bounded AIMD experiment, not a claim of TCP Reno wire compatibility.
class CongestionControl {
  public:
    using Clock = std::chrono::steady_clock;
    CongestionControl(CongestionMode mode, std::size_t maximum,
                      std::chrono::milliseconds initial_rtt);
    void acknowledge(std::size_t packets);
    void loss(std::uint32_t sequence, std::uint32_t next_sequence);
    void sample_rtt(std::chrono::milliseconds sample);
    bool ready(Clock::time_point now) const;
    void sent(Clock::time_point now);
    std::size_t window() const {
        return window_;
    }

  private:
    CongestionMode mode_;
    std::size_t maximum_, window_, threshold_, credit_ = 0;
    std::uint32_t recovery_end_ = 0;
    bool recovering_ = false;
    std::chrono::nanoseconds srtt_;
    Clock::time_point next_send_{};
};

} // namespace taut

#endif // TAUT_CONGESTION_H
