#include "taut/codec.h"

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
