# DESIGN - codec / wire format

Finalized in Week 1 S1. This is the byte-level contract every other module speaks.
It follows the PLAN §5.2 reference design; the one clarification we add on top of the
reference (class-0 sequence handling) is called out explicitly below and mirrored in
`docs/DECISIONS.md`.

All integers **little-endian** (native on x86/ARM - no byte-swap on the target). One
datagram = one taut packet. **Max datagram = 1200 B** (QUIC RFC 9000's choice: safely
below practical path MTU everywhere, so we never rely on IP fragmentation).

## Header layout

```
offset size field
0      2    magic          0x7A 0x75  ("zu")
2      1    ver(4b)|type(4b)   version = 1;
                                type: DATA=1 ACK=2 PING=3 PING_REQ=4
                                      PONG=5 JOIN=6 PROBE_WINDOW=7
3      1    flags          bit0 SACK-present, bit1 membership-piggyback,
                           bit2 keyed-CRC (fuzz build only, §6.2), rest reserved = 0
4      1    class          0=Unreliable 1=ReliableUnordered 2=ReliableOrdered
5      4    seq            u32 (see "sequence spaces" below)
9      4    cum_ack        u32 — highest reliable seq received with no gaps below it
13     2    adv_window     u16 — receiver free buffer, IN PACKETS (flow control, §5.6)
15     2    payload_len    u16
17     4    crc32c         u32 — over the entire datagram with this field zeroed
--- base header ends at offset 21 ---
21     [8]  sack_bitmap    iff flags.bit0 — bit i set = reliable seq (cum_ack+1+i) rx'd,
                           i in [0,63]; covers exactly one 64-packet window
[+]    [n]  membership     iff flags.bit1 — up to 3 × 12 B entries:
                           { addr4, port u16, incarnation u32, state u8, pad u8 }
[+]    ...  payload        (payload_len bytes)
```

**Sizes:** base header **21 B**; +8 with SACK (29 B); +up to 36 with membership. So max
app payload = 1200 − header: **1179 B** plain, **1171 B** with SACK, **1135 B** with SACK
+ 3 membership entries. `Config::mtu_payload` (default 1200) names the *datagram* budget,
not a literal payload cap - the codec enforces the real per-flags ceiling. (This
reconciles the loose §5.1 sketch, where the name reads as if payload could be a full
1200 B; it can't - the header is carved out of the 1200.)

## Decisions embedded here (each is W1-drill material - know *why*)

1. **CRC32C (Castagnoli), whole-datagram, crc field zeroed.** Hardware instruction on
   x86 (SSE4.2) and ARM; strong at catching burst errors and bit flips that a plain
   summation/Adler32 misses. We write the software table-driven fallback first, verify
   it against published vectors, *then* enable the HW path. It is an integrity check,
   **not** a security MAC - it catches corruption, not a malicious tamper (that's what
   the keyed-CRC fuzz flag, bit2, is about: it lets the fuzzer reach parser guts instead
   of bouncing off CRC rejection).

2. **Cumulative ack + fixed 64-bit SACK bitmap** (not variable SACK ranges). Fixed 8 B,
   O(1) to build/apply, and it covers exactly the 64-packet window (`window_pkts`
   default 64). Ranges would be variable-length, need parsing, and bound packet size less
   predictably. `cum_ack` = highest reliable seq with *no gaps below it*; the bitmap then
   describes which of the next 64 arrived out of order.

3. **Single sequence space for reliable traffic + separate class-0 dedup counter.**
   This is the one clarification of the reference. Reliable classes (1 and 2) share ONE
   per-peer-pair seq counter; ordering is enforced only for class 2 at the receiver.
   Class 0 (Unreliable) carries a seq drawn from an **independent** per-peer counter used
   *only* for a sliding dedup window - it is **excluded** from cum_ack, SACK, the
   in-flight ring, and retransmission.
   *Why:* if class 0 shared the reliable counter, a single lost class-0 packet (never
   retransmitted, by definition) would leave a permanent gap that cum_ack can never
   advance past - pinning the SACK window base forever and, after 64 further packets,
   making the fixed bitmap unable to represent live reliable packets. Separating class 0
   removes that failure entirely at the cost of one extra small counter per peer.

4. **Acks ride on everything; delayed pure-ACKs.** Every outgoing packet carries the
   current `cum_ack`/`adv_window`. A standalone ACK is emitted only when there's no
   reverse traffic, delayed up to 10 ms or coalesced every 2nd packet (classic
   delayed-ack shape). *(Timing lives in the rx/timer modules, not the codec - noted here
   for context.)*

5. **Minimal 2-way JOIN handshake** to exchange initial seq + config. No TIME_WAIT
   machinery; the restart-ambiguity limitation is documented honestly rather than
   engineered around.

## Invariants the codec must uphold
- Decode of any byte string never crashes / reads out of bounds / invokes UB; malformed
  input returns an error (this is exactly what `fuzz_decode` asserts, §6.2).
- `encode(decode(x)) == x` for every well-formed packet (round-trip).
- A single-bit flip anywhere in a valid datagram is rejected by the CRC check.
- `payload_len` and the flags-implied optional sections must fit within the received
  datagram length, or decode fails - never trust the length field against the buffer.

## Golden vectors (Laksh writes these first, by hand - §6.1)
Before the encoder/decoder is implemented (S2), Laksh hand-computes on paper: (a) one
DATA packet's exact bytes incl. CRC32C, (b) the decoded struct for a known byte array,
(c) a one-bit-flip case that must be rejected. That exercise is where the format becomes
his. (Committed as `kGolden` in `tests/unit/codec_test.cc`; Laksh's hand-laid packet is
`7a 75 11 00 02 01 00 00 00 00 00 00 00 40 00 02 00 cb 4e 6f c3 68 69`, CRC `0xC36F4ECB`.)

## Codec implementation (Week 1 S2 base; SACK added feat/core)

The first codec parsed the **base header (offsets 0–20) + payload + CRC** only. The **SACK
bitmap (flags bit0)** is now parsed and emitted; the membership (bit1) and keyed-CRC (bit2)
sections are still unhandled - the decoder rejects those bits as `Unsupported` and `encode`
refuses to emit them. Each optional section lands as its own increment with its own golden
vector.

### SACK section (offset 21, 8 B)

When `flags & SackPresent`, an 8-byte little-endian bitmap sits at offset 21, between the base
header and the payload (header 29 B; max plain payload drops to 1171 B). `Packet` exposes it as
`std::uint64_t sack`; **bit i (LSB-first) marks reliable seq `cum_ack + 1 + i` received**
(i ∈ [0,63]), byte 0 holding bits 0–7. The interpretation of the bit indices relative to
`cum_ack` is a session-layer contract (see docs/DESIGN-window.md), not the codec's - the codec
only places/extracts the 8 bytes. Golden vector `kGoldenSack`: the base golden DATA packet plus
`flags=SackPresent`, bitmap `0x05` (seqs 1,3), 31 B, CRC `0x5A4D396A`.

**`DecodeError` taxonomy** (categorizable rejects, for fuzzing): `TooShort` (< 21 B, or a
SACK-flagged datagram shorter than the 29 B header), `BadMagic`, `BadVersion`, `LengthOverrun`
(payload_len disagrees with datagram size, or size > 1200), `BadCrc`, `Unsupported`
(unhandled flag bit, or class > 2).

**Decode check order** - `TooShort → BadMagic → BadVersion → size(flags-derived) → length →
CRC → flags → class`. One refinement for SACK: the `SackPresent` bit (offset 3, always inside
the already-length-checked 21 B) is read *before* CRC purely to size the packet
(`header_len = 21 + 8·present`). A corrupted bit0 therefore makes `header_len`/`total` wrong and
the datagram is rejected as `LengthOverrun`, or - if the sizes still happen to line up - as
`BadCrc`; it is never misparsed. Everything else (the remaining flag bits, class) is still
trusted only *after* the CRC passes, so a corrupted flag/class byte is caught as `BadCrc`.
Length is checked before CRC because computing the CRC needs a bounded, exact-size buffer; a
datagram must be exactly one packet - trailing bytes are `LengthOverrun`.

**CRC computation.** Encode writes the crc field as zero, fills the SACK+payload, then takes a
one-shot CRC over the whole datagram (the field is already zero) and writes it back. Decode
can't zero the input, so it uses the incremental API in three chunks - `[0,17)` + four zero
bytes + `[21, total)` - equivalent, no buffer copy (D11). The `[21, total)` chunk spans the
SACK section and payload alike, so no CRC-code change was needed to cover SACK.

**Deviation note:** `Config::mtu_payload` (1200) names the datagram budget; the codec caps
`total = 21 + payload_len ≤ 1200`, so the real max payload is 1179 B (base header only).
