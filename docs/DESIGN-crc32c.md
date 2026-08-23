# DESIGN - crc32c

Integrity check for the whole datagram (§5.2). Detects corruption, not tampering (not a
MAC). Implemented Week 1 S2.

## Algorithm
Software **table-driven (Sarwate)**: a 256-entry table (one per byte value), computed at
compile time via a `constexpr` function, then one lookup + XOR per input byte. Chosen over
bit-at-a-time (8× slower) and slicing-by-8 (faster but 8× the tables / more complexity)  - 
correctness and verifiability matter here, raw speed does not until the benchmark phase.
Slicing-by-N is noted as a possible future perf win.

## Parameters (fixed by the "CRC-32C" spec, not a free choice)
- Polynomial (reflected): `0x82F63B78` (reflection of the Castagnoli poly `0x1EDC6F41`).
- init = `0xFFFFFFFF`, final XOR = `0xFFFFFFFF`, refin = refout = **true**.
- Canonical check: `crc32c("123456789") == 0xE3069283`; `crc32c("") == 0x00000000`.

**Why reflected?** CRC-32C is *defined* in reflected (LSB-first) form, and - decisively  - 
the hardware CRC32C instruction (x86 SSE4.2 `_mm_crc32`, ARM `__crc32cd`) computes the
reflected CRC. Using the reflected polynomial in software means the (future) hardware
fast-path produces bit-identical results, so it can be swapped in transparently.

## API
- `crc32c(span)` - one-shot.
- `crc32c_init() / crc32c_update(state, span) / crc32c_final(state)` - incremental, so the
  codec can CRC the datagram in two chunks around the zeroed crc field with no buffer copy.
  The `state` between init and final is the raw register (pre-final-XOR); do not interpret
  it as a CRC until `crc32c_final`.

## Hardware path (planned follow-on, not in the first commit)
Add `_mm_crc32_u64` (x86, SSE4.2) and `__crc32cd` (ARMv8 CRC ext) behind a one-time
runtime dispatch. Guard it with a **startup self-check**: CRC a fixed vector with both
software and HW and assert equality, so a wrong feature/flag assumption fails loudly
instead of silently corrupting. Software stays as the always-correct fallback.

## What CRC32C catches / misses (W1 drill)
Catches: all single-bit errors, all double-bit errors, any odd number of bit errors, all
burst errors ≤ 32 bits, and most longer bursts. Misses: an adversarial edit that
recomputes a valid CRC (it is not keyed) - hence bit2 keyed-CRC is a *fuzz-only* flag to
let the fuzzer past the check, never a security claim.
