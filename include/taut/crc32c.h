#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace taut {

// CRC-32C (Castagnoli), reflected polynomial 0x82F63B78, init/xorout 0xFFFFFFFF.
// Software, table-driven (Sarwate). Matches the hardware CRC32C instruction bit-for-bit
// so a future HW fast-path can be dropped in behind a runtime self-check. See
// docs/DESIGN-crc32c.md.

// One-shot: CRC of a byte span.
std::uint32_t crc32c(std::span<const std::byte> data);

// Incremental API — used by the codec's two-chunk CRC (bytes before the crc field,
// then 4 zero bytes, then bytes after). Usage:
//   auto s = crc32c_init();
//   s = crc32c_update(s, chunk_a);
//   s = crc32c_update(s, chunk_b);
//   uint32_t crc = crc32c_final(s);
std::uint32_t crc32c_init();
std::uint32_t crc32c_update(std::uint32_t state, std::span<const std::byte> data);
std::uint32_t crc32c_final(std::uint32_t state);

} // namespace taut
