// Minimal, self-contained SHA-256 (FIPS 180-4) for the file-transfer demos.
//
// This lives in demo/ deliberately: it is NOT library code and carries no third-party
// dependency (CLAUDE.md forbids deps in src/). The soak's authoritative integrity check
// is the system `sha256sum` on both files; this in-process digest is what send_file /
// recv_file print so a human can eyeball that both ends agree. The two must match, and
// the soak verifies that during bring-up.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

namespace demo {

class Sha256 {
  public:
    Sha256() = default;

    void update(std::span<const std::byte> data) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
        std::size_t n = data.size();
        bitlen_ += static_cast<std::uint64_t>(n) * 8;
        while (n > 0) {
            const std::size_t take = std::min<std::size_t>(64 - buflen_, n);
            std::memcpy(buf_.data() + buflen_, p, take);
            buflen_ += take;
            p += take;
            n -= take;
            if (buflen_ == 64) {
                transform(buf_.data());
                buflen_ = 0;
            }
        }
    }

    // Finalize and return the 64-char lowercase hex digest. Consumes the state.
    std::string hex() {
        // Pad: 0x80, then zeros, then the 64-bit big-endian bit length.
        std::array<std::uint8_t, 8> lenbe{};
        for (int i = 0; i < 8; ++i) {
            lenbe[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(bitlen_ >> (56 - 8 * i));
        }
        const std::uint8_t pad = 0x80;
        update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(&pad), 1));
        const std::uint8_t zero = 0;
        while (buflen_ != 56) {
            update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(&zero), 1));
        }
        update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(lenbe.data()), 8));

        static const char* kHex = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (std::uint32_t h : h_) {
            for (int shift = 28; shift >= 0; shift -= 4) {
                out.push_back(kHex[(h >> shift) & 0xF]);
            }
        }
        return out;
    }

    static std::string of(std::span<const std::byte> data) {
        Sha256 s;
        s.update(data);
        return s.hex();
    }

  private:
    static std::uint32_t rotr(std::uint32_t x, unsigned n) {
        return (x >> n) | (x << (32 - n));
    }

    void transform(const std::uint8_t* block) {
        static const std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
            0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
            0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
            0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
            0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
            0xc67178f2};

        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            const std::size_t j = static_cast<std::size_t>(i) * 4;
            w[i] = (static_cast<std::uint32_t>(block[j]) << 24) |
                   (static_cast<std::uint32_t>(block[j + 1]) << 16) |
                   (static_cast<std::uint32_t>(block[j + 2]) << 8) |
                   static_cast<std::uint32_t>(block[j + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
        std::uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = h + s1 + ch + k[i] + w[i];
            const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
        h_[5] += f;
        h_[6] += g;
        h_[7] += h;
    }

    std::array<std::uint32_t, 8> h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<std::uint8_t, 64> buf_{};
    std::size_t buflen_ = 0;
    std::uint64_t bitlen_ = 0;
};

} // namespace demo
