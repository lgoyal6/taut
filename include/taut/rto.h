#pragma once

#include <chrono>

namespace taut {

// Jacobson/Karn RTO estimator (RFC 6298, §5.5):
//   first sample:  SRTT = R;              RTTVAR = R/2
//   later:         RTTVAR = 3/4 RTTVAR + 1/4 |SRTT - R'|
//                  SRTT   = 7/8 SRTT   + 1/8 R'
//   RTO = clamp(SRTT + max(G, 4 RTTVAR), floor, max)
// The `floor` is the thesis knob (default 25 ms, well below TCP's ~200 ms minimum).
// Karn's rule is the caller's job: never call sample() for a retransmitted packet.
class RttEstimator {
  public:
    RttEstimator(std::chrono::milliseconds floor, std::chrono::milliseconds max);

    void sample(std::chrono::milliseconds rtt);
    std::chrono::milliseconds rto() const;
    bool has_sample() const {
        return has_;
    }

  private:
    bool has_ = false;
    double srtt_ = 0.0;   // ms
    double rttvar_ = 0.0; // ms
    std::chrono::milliseconds floor_;
    std::chrono::milliseconds max_;
};

} // namespace taut
