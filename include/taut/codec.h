#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "taut/types.h"

namespace taut {

// Wire constants (§5.2 / docs/DESIGN-codec.md).
inline constexpr std::uint8_t kMagic0 = 0x7A;
inline constexpr std::uint8_t kMagic1 = 0x75;
inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::size_t kBaseHeaderSize = 21;
inline constexpr std::size_t kSackSize = 8; // 64-bit SACK bitmap, present iff Flag::SackPresent
inline constexpr std::size_t kMaxDatagram = 1200;

enum class PacketType : std::uint8_t {
    Data = 1,
    Ack = 2,
    Ping = 3,
    PingReq = 4,
    Pong = 5,
    Join = 6,
    ProbeWindow = 7,
};

// Header flag bits (§5.2). SACK (bit0) is parsed by this codec; membership (bit1) and
// keyed-CRC (bit2) are not yet handled — a decoder rejects those bits as Unsupported and
// encode refuses to emit them.
enum class Flag : std::uint8_t {
    SackPresent = 0x01,
    MembershipPiggyback = 0x02,
    KeyedCrc = 0x04,
};

// Why decode rejected a datagram — makes fuzz failures categorizable.
enum class DecodeError : std::uint8_t {
    Ok = 0,
    TooShort,      // fewer bytes than the base header
    BadMagic,      // first two bytes are not the taut magic
    BadVersion,    // version nibble != 1
    LengthOverrun, // payload_len disagrees with the datagram size, or size > 1200
    BadCrc,        // CRC mismatch (corruption)
    Unsupported,   // a flag/class value this codec version does not handle
};

// Parsed packet. `payload` is a zero-copy view into the decoded input buffer, valid only
// as long as that buffer lives. On encode, `payload` is the bytes to send and `version`
// is ignored (the codec always writes version 1).
struct Packet {
    std::uint8_t version;
    PacketType type;
    std::uint8_t flags;
    Class cls;
    std::uint32_t seq;
    std::uint32_t cum_ack;
    std::uint16_t adv_window;
    // SACK bitmap (§5.2 / DESIGN-codec.md #2). Meaningful iff `flags & Flag::SackPresent`.
    // Bit i (LSB-first) set = the reliable seq (cum_ack + 1 + i) was received out of order,
    // i in [0,63]. Little-endian on the wire at offset 21; byte 0 holds bits 0..7.
    std::uint64_t sack;
    ByteSpan payload;
};

// Serialize `pkt` into `out`. Returns bytes written, or 0 on failure (buffer too small,
// payload > 65535 or datagram > 1200, or an unsupported flag bit set — only SackPresent is
// emittable). When Flag::SackPresent is set, the 8-byte bitmap is written at offset 21.
// The CRC is computed and written.
std::size_t encode(const Packet& pkt, std::span<std::byte> out);

// Parse `in`. On DecodeError::Ok, `out` is populated and out.payload views into `in`.
DecodeError decode(std::span<const std::byte> in, Packet& out);

} // namespace taut
