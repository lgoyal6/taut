#include "taut/codec.h"

#include "taut/crc32c.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "taut/types.h"

namespace {

// Laksh's hand-laid golden vector: a DATA packet, class ReliableOrdered, seq 1,
// cum_ack 0, adv_window 64, payload "hi". CRC (0xC36F4ECB) at offset 17-20.
constexpr std::array<std::uint8_t, 23> kGolden = {
    0x7A, 0x75, 0x11, 0x00, 0x02, // magic, ver|type, flags, class
    0x01, 0x00, 0x00, 0x00,       // seq = 1
    0x00, 0x00, 0x00, 0x00,       // cum_ack = 0
    0x40, 0x00,                   // adv_window = 64
    0x02, 0x00,                   // payload_len = 2
    0xCB, 0x4E, 0x6F, 0xC3,       // crc32c = 0xC36F4ECB
    0x68, 0x69,                   // "hi"
};

std::array<std::byte, 23> golden_bytes() {
    std::array<std::byte, 23> buf{};
    for (std::size_t i = 0; i < buf.size(); ++i) {
        buf[i] = std::byte{kGolden[i]};
    }
    return buf;
}

taut::Packet golden_packet(std::span<const std::byte> payload) {
    taut::Packet pkt{};
    pkt.type = taut::PacketType::Data;
    pkt.flags = 0;
    pkt.cls = taut::Class::ReliableOrdered;
    pkt.seq = 1;
    pkt.cum_ack = 0;
    pkt.adv_window = 64;
    pkt.payload = payload;
    return pkt;
}

} // namespace

TEST(Codec, EncodeMatchesGoldenVector) {
    const std::array<std::byte, 2> payload{std::byte{0x68}, std::byte{0x69}};
    const taut::Packet pkt = golden_packet(payload);

    std::array<std::byte, 64> out{};
    const std::size_t n = taut::encode(pkt, out);
    ASSERT_EQ(n, 23u);
    for (std::size_t i = 0; i < 23; ++i) {
        EXPECT_EQ(std::to_integer<std::uint8_t>(out[i]), kGolden[i]) << "byte " << i;
    }
}

TEST(Codec, DecodeMatchesGoldenVector) {
    const auto buf = golden_bytes();
    taut::Packet p{};
    ASSERT_EQ(taut::decode(buf, p), taut::DecodeError::Ok);
    EXPECT_EQ(p.version, 1u);
    EXPECT_EQ(p.type, taut::PacketType::Data);
    EXPECT_EQ(p.cls, taut::Class::ReliableOrdered);
    EXPECT_EQ(p.seq, 1u);
    EXPECT_EQ(p.cum_ack, 0u);
    EXPECT_EQ(p.adv_window, 64u);
    ASSERT_EQ(p.payload.size(), 2u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(p.payload[0]), 0x68u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(p.payload[1]), 0x69u);
}

TEST(Codec, RoundTripArbitraryPayload) {
    std::vector<std::byte> payload;
    for (int i = 0; i < 200; ++i) {
        payload.push_back(std::byte{static_cast<std::uint8_t>(i * 7 + 1)});
    }
    taut::Packet pkt = golden_packet(payload);
    pkt.type = taut::PacketType::Data;
    pkt.cls = taut::Class::ReliableUnordered;
    pkt.seq = 0xDEADBEEF;
    pkt.cum_ack = 0x01020304;
    pkt.adv_window = 12345;

    std::array<std::byte, taut::kMaxDatagram> out{};
    const std::size_t n = taut::encode(pkt, out);
    ASSERT_EQ(n, taut::kBaseHeaderSize + payload.size());

    taut::Packet got{};
    ASSERT_EQ(taut::decode(std::span<const std::byte>(out).first(n), got), taut::DecodeError::Ok);
    EXPECT_EQ(got.seq, 0xDEADBEEFu);
    EXPECT_EQ(got.cum_ack, 0x01020304u);
    EXPECT_EQ(got.adv_window, 12345u);
    EXPECT_EQ(got.cls, taut::Class::ReliableUnordered);
    ASSERT_EQ(got.payload.size(), payload.size());
    EXPECT_TRUE(std::equal(payload.begin(), payload.end(), got.payload.begin()));
}

TEST(Codec, BitFlipInPayloadIsRejectedByCrc) {
    auto buf = golden_bytes();
    buf[22] ^= std::byte{0x01}; // flip a payload bit
    taut::Packet p{};
    EXPECT_EQ(taut::decode(buf, p), taut::DecodeError::BadCrc);
}

TEST(Codec, BadMagicRejected) {
    auto buf = golden_bytes();
    buf[0] = std::byte{0x00};
    taut::Packet p{};
    EXPECT_EQ(taut::decode(buf, p), taut::DecodeError::BadMagic);
}

TEST(Codec, BadVersionRejected) {
    auto buf = golden_bytes();
    buf[2] = std::byte{0x21}; // version nibble = 2
    taut::Packet p{};
    EXPECT_EQ(taut::decode(buf, p), taut::DecodeError::BadVersion);
}

TEST(Codec, TruncatedBelowHeaderIsTooShort) {
    const auto full = golden_bytes();
    std::array<std::byte, 10> buf{};
    std::copy(full.begin(), full.begin() + 10, buf.begin());
    taut::Packet p{};
    EXPECT_EQ(taut::decode(buf, p), taut::DecodeError::TooShort);
}

TEST(Codec, PayloadLenDisagreesWithSizeIsOverrun) {
    auto buf = golden_bytes();
    buf[15] = std::byte{0x64}; // payload_len = 100, but only 2 bytes follow
    taut::Packet p{};
    EXPECT_EQ(taut::decode(buf, p), taut::DecodeError::LengthOverrun);
}

// ---- SACK section (flags bit0) ----

// Golden SACK packet: the base golden DATA packet plus flags=SackPresent and an 8-byte
// bitmap at offset 21. Bits 0 and 2 set = reliable seqs cum_ack+1+0=1 and cum_ack+1+2=3
// received out of order. 31 B total; CRC 0x5A4D396A (captured from the encoder, which the
// crc32c known-vector tests and the base golden already anchor).
constexpr std::array<std::uint8_t, 31> kGoldenSack = {
    0x7A, 0x75, 0x11, 0x01, 0x02,                   // magic, ver|type, flags=SACK, class
    0x01, 0x00, 0x00, 0x00,                         // seq = 1
    0x00, 0x00, 0x00, 0x00,                         // cum_ack = 0
    0x40, 0x00,                                     // adv_window = 64
    0x02, 0x00,                                     // payload_len = 2
    0x6A, 0x39, 0x4D, 0x5A,                         // crc32c = 0x5A4D396A
    0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // sack bitmap = 0x05
    0x68, 0x69,                                     // "hi"
};

std::array<std::byte, 31> golden_sack_bytes() {
    std::array<std::byte, 31> buf{};
    for (std::size_t i = 0; i < buf.size(); ++i) {
        buf[i] = std::byte{kGoldenSack[i]};
    }
    return buf;
}

TEST(Codec, EncodeMatchesGoldenSackVector) {
    const std::array<std::byte, 2> payload{std::byte{0x68}, std::byte{0x69}};
    taut::Packet pkt = golden_packet(payload);
    pkt.flags = static_cast<std::uint8_t>(taut::Flag::SackPresent);
    pkt.sack = 0x05;

    std::array<std::byte, 64> out{};
    const std::size_t n = taut::encode(pkt, out);
    ASSERT_EQ(n, 31u);
    for (std::size_t i = 0; i < 31; ++i) {
        EXPECT_EQ(std::to_integer<std::uint8_t>(out[i]), kGoldenSack[i]) << "byte " << i;
    }
}

TEST(Codec, DecodeMatchesGoldenSackVector) {
    const auto buf = golden_sack_bytes();
    taut::Packet p{};
    ASSERT_EQ(taut::decode(buf, p), taut::DecodeError::Ok);
    EXPECT_EQ(p.flags, static_cast<std::uint8_t>(taut::Flag::SackPresent));
    EXPECT_EQ(p.seq, 1u);
    EXPECT_EQ(p.cum_ack, 0u);
    EXPECT_EQ(p.sack, 0x05u); // seqs 1 and 3 SACKed
    ASSERT_EQ(p.payload.size(), 2u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(p.payload[0]), 0x68u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(p.payload[1]), 0x69u);
}

TEST(Codec, RoundTripWithSackBitmap) {
    std::vector<std::byte> payload;
    for (int i = 0; i < 50; ++i) {
        payload.push_back(std::byte{static_cast<std::uint8_t>(i * 3 + 2)});
    }
    taut::Packet pkt = golden_packet(payload);
    pkt.type = taut::PacketType::Ack;
    pkt.flags = static_cast<std::uint8_t>(taut::Flag::SackPresent);
    pkt.seq = 0;
    pkt.cum_ack = 0x00ABCDEF;
    pkt.sack = 0x8000'0000'0000'0001ull; // highest (bit 63) and lowest (bit 0) both set

    std::array<std::byte, taut::kMaxDatagram> out{};
    const std::size_t n = taut::encode(pkt, out);
    ASSERT_EQ(n, taut::kBaseHeaderSize + taut::kSackSize + payload.size());

    taut::Packet got{};
    ASSERT_EQ(taut::decode(std::span<const std::byte>(out).first(n), got), taut::DecodeError::Ok);
    EXPECT_EQ(got.cum_ack, 0x00ABCDEFu);
    EXPECT_EQ(got.sack, 0x8000'0000'0000'0001ull);
    ASSERT_EQ(got.payload.size(), payload.size());
    EXPECT_TRUE(std::equal(payload.begin(), payload.end(), got.payload.begin()));
}

TEST(Codec, BitFlipInSackSectionIsRejectedByCrc) {
    auto buf = golden_sack_bytes();
    buf[21] ^= std::byte{0x02}; // flip a bit inside the SACK bitmap
    taut::Packet p{};
    EXPECT_EQ(taut::decode(buf, p), taut::DecodeError::BadCrc);
}

TEST(Codec, TruncatedSackSectionIsTooShort) {
    // Claims SackPresent but only 25 bytes present (< 21 + 8 header).
    const auto full = golden_sack_bytes();
    std::array<std::byte, 25> buf{};
    std::copy(full.begin(), full.begin() + 25, buf.begin());
    taut::Packet p{};
    EXPECT_EQ(taut::decode(buf, p), taut::DecodeError::TooShort);
}

TEST(Codec, MembershipFlagIsUnsupported) {
    // A well-formed datagram whose flags carry the (unhandled) membership bit must be
    // rejected as Unsupported, not misparsed. Build one via encode's SACK path then flip the
    // bit and repair the CRC so it survives to the flags check.
    auto buf = golden_sack_bytes();
    buf[3] = std::byte{0x03}; // SackPresent | MembershipPiggyback
    // Recompute a valid CRC over the tampered datagram (crc field zeroed).
    std::array<std::byte, 31> tmp = buf;
    for (int i = 17; i < 21; ++i) {
        tmp[static_cast<std::size_t>(i)] = std::byte{0};
    }
    const std::uint32_t crc = taut::crc32c(tmp);
    buf[17] = std::byte{static_cast<std::uint8_t>(crc & 0xFF)};
    buf[18] = std::byte{static_cast<std::uint8_t>((crc >> 8) & 0xFF)};
    buf[19] = std::byte{static_cast<std::uint8_t>((crc >> 16) & 0xFF)};
    buf[20] = std::byte{static_cast<std::uint8_t>((crc >> 24) & 0xFF)};
    taut::Packet p{};
    EXPECT_EQ(taut::decode(buf, p), taut::DecodeError::Unsupported);
}
