#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace taut {

// Opaque handle for a peer, assigned by Node::add_peer.
using PeerId = std::uint32_t;

// Reliability class (§5.3). Wire value is the low 8 bits of the header `class` field.
enum class Class : std::uint8_t {
    Unreliable = 0,        // may drop / reorder, no dup; dedup window only
    ReliableUnordered = 1, // exactly-once, any order; the latency star
    ReliableOrdered = 2,   // exactly-once, in send order
};

// SWIM membership state (§5.9).
enum class MemberState : std::uint8_t {
    Alive = 0,
    Suspect = 1,
    Dead = 2,
};

// Read-only view of payload bytes handed to/from the app. No ownership.
using ByteSpan = std::span<const std::byte>;

} // namespace taut
