#include "taut/codec.h"

#include <array>
#include <cstring>

#include "taut/crc32c.h"

namespace taut {
namespace {

// Little-endian byte access. Shift-based on purpose (D10): identical on any host
// endianness, no aliasing/alignment UB. Offsets are always in-bounds by construction of
// the callers below.
std::uint8_t load_u8(std::span<const std::byte> b, std::size_t off) {
    return std::to_integer<std::uint8_t>(b[off]);
}

std::uint16_t load_u16_le(std::span<const std::byte> b, std::size_t off) {
    return static_cast<std::uint16_t>(static_cast<unsigned>(load_u8(b, off)) |
                                      (static_cast<unsigned>(load_u8(b, off + 1)) << 8));
}

std::uint32_t load_u32_le(std::span<const std::byte> b, std::size_t off) {
    return static_cast<std::uint32_t>(load_u8(b, off)) |
           (static_cast<std::uint32_t>(load_u8(b, off + 1)) << 8) |
           (static_cast<std::uint32_t>(load_u8(b, off + 2)) << 16) |
           (static_cast<std::uint32_t>(load_u8(b, off + 3)) << 24);
}

void store_u8(std::span<std::byte> b, std::size_t off, std::uint8_t v) {
    b[off] = std::byte{v};
}

void store_u16_le(std::span<std::byte> b, std::size_t off, std::uint16_t v) {
    store_u8(b, off, static_cast<std::uint8_t>(v & 0xFFu));
    store_u8(b, off + 1, static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}

void store_u32_le(std::span<std::byte> b, std::size_t off, std::uint32_t v) {
    store_u8(b, off, static_cast<std::uint8_t>(v & 0xFFu));
    store_u8(b, off + 1, static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    store_u8(b, off + 2, static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    store_u8(b, off + 3, static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

// CRC field lives at offsets [17, 21). The checksum covers the whole datagram with those
// 4 bytes treated as zero.
constexpr std::size_t kCrcOffset = 17;

} // namespace

std::size_t encode(const Packet& pkt, std::span<std::byte> out) {
    if (pkt.flags != 0) {
        return 0; // SACK / membership emit not yet supported
    }
    const std::size_t payload_len = pkt.payload.size();
    if (payload_len > 0xFFFFu) {
        return 0;
    }
    const std::size_t total = kBaseHeaderSize + payload_len;
    if (total > kMaxDatagram || out.size() < total) {
        return 0;
    }

    store_u8(out, 0, kMagic0);
    store_u8(out, 1, kMagic1);
    store_u8(
        out, 2,
        static_cast<std::uint8_t>((kVersion << 4) | (static_cast<std::uint8_t>(pkt.type) & 0x0Fu)));
    store_u8(out, 3, pkt.flags);
    store_u8(out, 4, static_cast<std::uint8_t>(pkt.cls));
    store_u32_le(out, 5, pkt.seq);
    store_u32_le(out, 9, pkt.cum_ack);
    store_u16_le(out, 13, pkt.adv_window);
    store_u16_le(out, 15, static_cast<std::uint16_t>(payload_len));
    store_u32_le(out, kCrcOffset, 0); // placeholder; CRC is computed over this as zero

    if (payload_len > 0) {
        std::memcpy(out.data() + kBaseHeaderSize, pkt.payload.data(), payload_len);
    }

    // CRC field is currently zero, so a one-shot over the whole datagram is exactly
    // "CRC with the crc field zeroed".
    const std::uint32_t crc = crc32c(out.subspan(0, total));
    store_u32_le(out, kCrcOffset, crc);
    return total;
}

DecodeError decode(std::span<const std::byte> in, Packet& out) {
    if (in.size() < kBaseHeaderSize) {
        return DecodeError::TooShort;
    }
    if (load_u8(in, 0) != kMagic0 || load_u8(in, 1) != kMagic1) {
        return DecodeError::BadMagic;
    }
    const std::uint8_t ver_type = load_u8(in, 2);
    const std::uint8_t version = static_cast<std::uint8_t>(ver_type >> 4);
    const std::uint8_t type = static_cast<std::uint8_t>(ver_type & 0x0Fu);
    if (version != kVersion) {
        return DecodeError::BadVersion;
    }

    const std::uint16_t payload_len = load_u16_le(in, 15);
    const std::size_t total = kBaseHeaderSize + payload_len;
    if (total > kMaxDatagram || in.size() != total) {
        return DecodeError::LengthOverrun;
    }

    // Verify integrity before trusting any other field: CRC over [0,17) + 4 zero bytes
    // (the zeroed crc field) + [21, total). Uses the incremental API (D11) — no copy.
    const std::uint32_t stored_crc = load_u32_le(in, kCrcOffset);
    static constexpr std::array<std::byte, 4> kZeroCrc{};
    std::uint32_t crc = crc32c_init();
    crc = crc32c_update(crc, in.subspan(0, kCrcOffset));
    crc = crc32c_update(crc, kZeroCrc);
    crc = crc32c_update(crc, in.subspan(kBaseHeaderSize, payload_len));
    crc = crc32c_final(crc);
    if (crc != stored_crc) {
        return DecodeError::BadCrc;
    }

    const std::uint8_t flags = load_u8(in, 3);
    if (flags != 0) {
        return DecodeError::Unsupported; // SACK / membership not parsed yet
    }
    const std::uint8_t cls_raw = load_u8(in, 4);
    if (cls_raw > static_cast<std::uint8_t>(Class::ReliableOrdered)) {
        return DecodeError::Unsupported;
    }

    out.version = version;
    out.type = static_cast<PacketType>(type);
    out.flags = flags;
    out.cls = static_cast<Class>(cls_raw);
    out.seq = load_u32_le(in, 5);
    out.cum_ack = load_u32_le(in, 9);
    out.adv_window = load_u16_le(in, 13);
    out.payload = in.subspan(kBaseHeaderSize, payload_len);
    return DecodeError::Ok;
}

} // namespace taut
