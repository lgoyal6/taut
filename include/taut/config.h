#pragma once

#include <chrono>
#include <cstdint>

namespace taut {

// Node configuration (§5.1). Strong defaults chosen so week 1 isn't spent bikeshedding;
// every field is a documented design knob.
struct Config {
    // The thesis knob (§5.5): retransmit floor well below TCP's ~200 ms Linux minimum.
    // Safe on a closed mesh, unsafe on the open internet - be able to explain why.
    std::chrono::milliseconds rto_floor{25};

    // Fixed send window in packets (§5.4). No congestion control in v1 (§5.8).
    std::uint16_t window_pkts{64};

    // Max application payload per datagram. The header (§5.2) is carved out of the
    // 1200 B datagram budget; see docs/DESIGN-codec.md for the exact ceiling.
    std::uint16_t mtu_payload{1200};
};

} // namespace taut
