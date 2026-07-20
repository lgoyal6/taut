#include "taut/crc32c.h"

#include <array>

namespace taut {
namespace {

// Reflected CRC-32C / Castagnoli polynomial (reflection of 0x1EDC6F41). The reflected
// form is what the hardware CRC32C instruction computes, so software and HW agree.
constexpr std::uint32_t kPoly = 0x82F63B78u;

// 256-entry Sarwate table, computed at compile time.
constexpr std::array<std::uint32_t, 256> make_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256u; ++i) {
        std::uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) ? (crc >> 1) ^ kPoly : (crc >> 1);
        }
        table[i] = crc;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kTable = make_table();

} // namespace

std::uint32_t crc32c_init() {
    return 0xFFFFFFFFu;
}

std::uint32_t crc32c_update(std::uint32_t state, std::span<const std::byte> data) {
    for (std::byte b : data) {
        const std::uint32_t byte = std::to_integer<std::uint32_t>(b);
        const std::uint8_t idx = static_cast<std::uint8_t>((state ^ byte) & 0xFFu);
        state = kTable[idx] ^ (state >> 8);
    }
    return state;
}

std::uint32_t crc32c_final(std::uint32_t state) {
    return state ^ 0xFFFFFFFFu;
}

std::uint32_t crc32c(std::span<const std::byte> data) {
    return crc32c_final(crc32c_update(crc32c_init(), data));
}

} // namespace taut
