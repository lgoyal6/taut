# DECISIONS

One entry per design decision point. Per CLAUDE.md, **Laksh writes the rationale in his
own words** — the "why" is the ownership, and it's what interviews probe. The decision
itself is recorded factually; fill each `Rationale:` line before the module's gate.

---

## Week 1 S1 — Dev environment & codec wire format

### D1. Dev environment: **Lima** (Ubuntu VM on the Mac)
- Chosen over Docker/OrbStack container.
- Rationale (Laksh): _______________________________________________

### D2. Sequence space: **single reliable seq space + separate class-0 dedup counter**
- Reliable classes (1,2) share one per-peer counter; class 0 has its own dedup-only
  counter, excluded from cum_ack/SACK/ring/retransmit. (Clarifies PLAN §5.2; see
  docs/DESIGN-codec.md #3.)
- Alternatives considered: all-classes-share-one-space (rejected: a lost class-0 packet
  pins the SACK window base forever); per-class seq spaces (rejected: triples bookkeeping).
- Rationale (Laksh): _______________________________________________

### D3. Ack scheme: **cumulative ack + fixed 64-bit SACK bitmap**
- Chosen over variable-length SACK ranges.
- Rationale (Laksh): _______________________________________________

### D4. Checksum: **CRC32C (Castagnoli)**, software fallback first then HW path
- Chosen over Adler32 / xxHash / plain checksum.
- Rationale (Laksh): _______________________________________________

### D5. Endianness **little-endian**; max datagram **1200 B**; magic **0x7A 0x75**
- Field sizes: seq u32, cum_ack u32, adv_window u16 (packets), payload_len u16, crc u32.
- Rationale (Laksh): _______________________________________________
