#include "taut/crc32c.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <gtest/gtest.h>

namespace {

std::span<const std::byte> as_bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

} // namespace

// Empty input: init ^ xorout == 0xFFFFFFFF ^ 0xFFFFFFFF == 0.
TEST(Crc32c, EmptyIsZero) {
    EXPECT_EQ(taut::crc32c({}), 0x00000000u);
}

// The canonical CRC-32C/iSCSI check value for the ASCII string "123456789".
TEST(Crc32c, CanonicalCheckValue) {
    EXPECT_EQ(taut::crc32c(as_bytes("123456789")), 0xE3069283u);
}

// Splitting the input across two incremental updates must equal the one-shot CRC —
// this is exactly what the codec's two-chunk CRC (skipping the crc field) relies on.
TEST(Crc32c, IncrementalMatchesOneShot) {
    const auto data = as_bytes("hello, taut");
    std::uint32_t state = taut::crc32c_init();
    state = taut::crc32c_update(state, data.subspan(0, 4));
    state = taut::crc32c_update(state, data.subspan(4));
    EXPECT_EQ(taut::crc32c_final(state), taut::crc32c(data));
}

// A single-bit flip anywhere must change the CRC (error-detection sanity).
TEST(Crc32c, BitFlipChangesCrc) {
    std::array<std::byte, 4> buf{std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
                                 std::byte{0x04}};
    const std::uint32_t before = taut::crc32c(buf);
    buf[2] ^= std::byte{0x08};
    const std::uint32_t after = taut::crc32c(buf);
    EXPECT_NE(before, after);
}
