// libFuzzer harness for the decoder (§6.2). Random bytes -> decode() must never crash,
// read out of bounds, or invoke UB under ASan/UBSan; malformed input returns an error.
//
// Built with TAUT_FUZZ_PATCH_CRC, the harness forces the structural fields valid and fixes
// up the CRC before decoding, so coverage reaches the field-parsing guts instead of
// bouncing off the magic/version/length/CRC guards.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "taut/codec.h"
#include "taut/crc32c.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto* bytes = reinterpret_cast<const std::byte*>(data);
    std::span<const std::byte> in{bytes, size};

#ifdef TAUT_FUZZ_PATCH_CRC
    std::vector<std::byte> patched;
    if (size >= taut::kBaseHeaderSize) {
        patched.assign(in.begin(), in.end());
        patched[0] = std::byte{taut::kMagic0};
        patched[1] = std::byte{taut::kMagic1};
        patched[2] = std::byte{static_cast<std::uint8_t>(
            (taut::kVersion << 4) | (std::to_integer<std::uint8_t>(patched[2]) & 0x0Fu))};
        const auto pl = static_cast<std::uint16_t>(size - taut::kBaseHeaderSize);
        patched[15] = std::byte{static_cast<std::uint8_t>(pl & 0xFFu)};
        patched[16] = std::byte{static_cast<std::uint8_t>((pl >> 8) & 0xFFu)};
        for (std::size_t i = 17; i < 21; ++i) {
            patched[i] = std::byte{0};
        }
        const std::uint32_t crc = taut::crc32c(patched);
        patched[17] = std::byte{static_cast<std::uint8_t>(crc & 0xFFu)};
        patched[18] = std::byte{static_cast<std::uint8_t>((crc >> 8) & 0xFFu)};
        patched[19] = std::byte{static_cast<std::uint8_t>((crc >> 16) & 0xFFu)};
        patched[20] = std::byte{static_cast<std::uint8_t>((crc >> 24) & 0xFFu)};
        in = std::span<const std::byte>(patched);
    }
#endif

    taut::Packet pkt{};
    if (taut::decode(in, pkt) == taut::DecodeError::Ok) {
        // Invariant: a packet that decodes must re-encode to exactly its own bytes.
        std::array<std::byte, taut::kMaxDatagram> out{};
        const std::size_t n = taut::encode(pkt, out);
        if (n != in.size() || std::memcmp(out.data(), in.data(), n) != 0) {
            __builtin_trap();
        }
    }
    return 0;
}
