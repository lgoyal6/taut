// Defines the bounded session settings consumed by session.cc and benchmarks.
// The fixed-window policy remains the default; paced AIMD is opt-in.
// Payload and poll limits bound message size and work per event-loop pass.
#pragma once

#include "taut/congestion.h"
#include <chrono>
#include <cstdint>

namespace taut {

struct Config {
    // Short retransmit floor for closed meshes; not an Internet-safety guarantee.
    std::chrono::milliseconds rto_floor{25};

    // Maximum reliable send window in packets (§5.4). The congestion window may be smaller.
    std::uint16_t window_pkts{64};
    CongestionMode congestion{CongestionMode::FixedWindow};

    // Bounds datagrams per poll so timers and other peers can still run.
    // poll returns true at the bound; callers can drain again after calling tick.
    std::uint32_t max_recv_per_poll{64};

    // Max application payload per datagram. The header (§5.2) is carved out of the
    // 1200 B datagram budget; see docs/DESIGN-codec.md for the exact ceiling.
    std::uint16_t mtu_payload{1200};
};

} // namespace taut
