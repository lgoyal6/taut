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

---

## Week 1 S2 — crc32c + codec implementation

### D6. CRC32C software algorithm: **table-driven (Sarwate, 256-entry)**
- Over bitwise (8× slower) and slicing-by-8 (faster, more tables/complexity).
- Rationale (Laksh): _______________________________________________

### D7. CRC32C rollout: **software first (verified vs known vectors), then HW path**
- HW (`_mm_crc32` / `__crc32cd`) is a follow-on behind a runtime self-check (HW == software
  on a fixed vector at init).
- Rationale (Laksh): _______________________________________________

### D8. Codec error handling: **`DecodeError` enum + out-param** (no exceptions)
- Over `std::optional<Packet>` (C++20 has no `std::expected`; no third-party deps in src).
  Enum gives fuzz-diagnosable reject reasons (TooShort/BadMagic/BadVersion/BadCrc/...).
- Rationale (Laksh): _______________________________________________

### D9. Codec decode payload: **zero-copy `ByteSpan` view into the input buffer**
- Over copy-on-decode; the loop owns buffer lifetime, so no allocation on the hot path.
- Rationale (Laksh): _______________________________________________

### D10. Integer access: **shift-based little-endian load/store helpers**
- Over memcpy+bswap; portable, no aliasing/alignment UB, UBSan-clean.
- Rationale (Laksh): _______________________________________________

### D11. CRC-field handling: **two-chunk CRC** (bytes [0,17) + 4 zero bytes + [21,end))
- Over copy-and-zero; same result, no full-packet copy.
- Rationale (Laksh): _______________________________________________

### D12. Test framework: **GoogleTest via FetchContent** (tooling)
- Minor: over doctest; industry-standard and recognizable. Test targets use a relaxed
  warning set (`-Wall -Wextra`, no `-Werror`) so framework headers don't break the build.
- Rationale (Laksh): _______________________________________________

---

## Week 1 S3 — event loop skeleton + transport + fuzzer

### D13. epoll: **level-triggered** (not edge-triggered)
- LT re-signals while data remains (can't lose a wakeup); ET signals once per transition
  and demands full drain. Correctness-first; ET noted as future work.
- Rationale (Laksh): _______________________________________________

### D14. **`UdpTransport` abstraction** (send/recv/now/fd); real-socket impl in S3
- SimNet (in-process, seeded, virtual clock) arrives week 2 for deterministic tests.
- Rationale (Laksh): _______________________________________________

### D15. Loop wiring: **eventfd** for wake/shutdown now; **timerfd** deferred to week 2
- The timer heap and its timerfd land with the reliability core; S3 is a skeleton.
- Rationale (Laksh): _______________________________________________

### D16. `fuzz_decode`: **CRC-patch coverage mode + round-trip invariant**
- Patch mode forces structural fields valid and fixes the CRC so coverage reaches the
  parser; a decoded packet must re-encode to its own bytes, else the fuzzer traps.
- Rationale (Laksh): _______________________________________________

---

## feat/core — SACK + reliability classes + flow control

### D17. Fast retransmit trigger: **≥ 3 SACKed slots above an un-SACKed slot**
- The Reno 3-dup-ack heuristic expressed on the SACK bitmap. SACKing a slot also cancels its
  RTO (a received packet is never resent) and takes its RTT sample early.
- Alternatives: RTO-only recovery (slow — the whole reason goodput collapses under loss);
  retransmit on the first gap seen (too eager — reorder would trigger spurious resends).
- Rationale (Laksh): _______________________________________________

### D18. Class dispatch on the rx path: **shared reliable seq space, per-class delivery**
- Class 1 delivers on arrival and leaves a payload-less `reasm_` marker (for cum_ack/SACK/
  dedup); class 2 buffers for in-order delivery; class 0 uses a separate 64-seq anti-replay
  dedup window and is never acked/retransmitted (D2).
- Alternative: per-class sequence spaces (rejected in D2 — triples bookkeeping).
- Rationale (Laksh): _______________________________________________

### D19. `adv_window` counts **only the app-delivery queue**, not out-of-order reassembly
- Out-of-order data sits within the sender's in-flight window, which `adv_window` already
  bounds; subtracting it again would double-count. Lets invariant 3 hold literally for class 2
  and keeps no-overflow structural (see docs/DESIGN-flow.md).
- Alternative: count all buffered packets (over-tight, transiently violates invariant 3 even
  for ordered traffic).
- Rationale (Laksh): _______________________________________________

### D20. Zero-window handling: **bounded internal send queue + PROBE_WINDOW persist timer**
- `send()` parks reliable messages in a bounded `pending_` queue so the session knows it has
  data to send while the window is shut; a persist timer (100 ms, ×2, cap 1 s) sends a
  dedicated `PROBE_WINDOW` packet when `pending && adv_window==0 && ring empty`, so a lost
  window-reopening ack can't deadlock.
- Alternative: rely on data-packet RTO to re-probe (fails when nothing is in flight).
- Rationale (Laksh): _______________________________________________

---

## Week 4 — SWIM membership (§5.9)

### D21. Gossip wire location: **carried in the packet PAYLOAD** (not the §5.2 header piggyback)
- Uses `PacketType::{Ping,PingReq,Pong,Join}` + `Class::Unreliable`, codec untouched. Lane
  constraint for the parallel-worktree split; header-piggyback (flags.bit1) is a later merge.
  Sender identity comes from the transport `from`, never the payload (mirrors SWIM's recvfrom).
- Rationale (Laksh): _______________________________________________

### D22. State merge: **incarnation precedence** (Suspect beats Alive at equal inc; Dead terminal)
- Single `apply_rumor()` entry point for wire gossip, own conclusions, and tests. Only the
  accused refutes (incarnation+1). Alternative (bare timestamp/last-writer) rejected: it can't
  distinguish a stale suspicion from a fresh one — the exact failure incarnation numbers fix.
- Rationale (Laksh): _______________________________________________

### D23. Dissemination: **one rumor per subject, least-transmitted-first, budget ceil(3·ln N)**
- Overwrite-on-supersede bounds the buffer to N and auto-dedups. Budget=5 at N=5.
- **Deviation:** unresolved Suspect rumors are re-injected each period (anti-entropy) so a
  partition-heal reconverges — otherwise the budget is spent broadcasting into the partition and
  the accused never hears the suspicion to refute. (docs/DESIGN-swim.md.)
- Rationale (Laksh): _______________________________________________

### D24. Timing/driving: **scalar per-period deadlines**, `poll()`/`tick()` (not the timer heap)
- SWIM has a handful of coarse deadlines (ping 300 ms, period 1 s, suspicion 3 s); explicit
  scalars read clearer than reusing the ARQ min-heap. Detection stages/math in DESIGN-swim.md.
- Rationale (Laksh): _______________________________________________

### D25. Partition modeling: **`LinkFilter` transport decorator** in test/demo (SimNet untouched)
- Symmetric block on both endpoints == bidirectional partition; keeps the shared SimNet
  (feat/core) unedited. k=3 indirect probes, T=1 s, suspicion 3 s are the §5.9 reference values.
- Rationale (Laksh): _______________________________________________

### D26. (v0.1.1) SWIM rejoin: **lexicographic `(incarnation, state)` precedence + JOIN reply marking**
- tautq (kill/restart chaos) exposed that v0.1.0's terminal-Dead rule makes restart-and-rejoin
  impossible, and that JOIN replies were themselves answered (infinite JOIN ping-pong).
- `Alive@k+1` now resurrects `Dead@k` (memberlist/Lifeguard ordering; only the subject mints
  higher incarnations, so it is first-hand proof of life). JOIN replies name the joiner as
  subject so only requests are answered. No wire-format change. (docs/DESIGN-swim.md "Rejoin".)
- Rationale (Laksh): approved 2026-07-27 in the tautq protocol brief (§0) — chaos scenarios
  (a) SIGKILL+restart and (d) stale-log restart require rejoin.

### D27. (v0.1.2) SWIM post-Dead refutation channel: **dead-probing + Dead-rumor re-send on contact**
- Found LIVE by tautq's chaos partition scenario: a partition held past suspicion_timeout
  leaves both sides holding Dead verdicts; base SWIM then never exchanges another packet
  across the healed link (Dead members are never probed) and the accusation's gossip budget
  is spent, so the accused never refutes — a permanent split. Rejoin (D26) doesn't apply:
  nothing restarted.
- One direct PING per period to a random Dead member, carrying its own Dead rumor
  (re-queued, fresh budget); a live accused refutes at inc+1 and resurrects under the D26
  ordering. Any packet FROM a Dead-believed member also re-queues its Dead rumor.
  (docs/DESIGN-swim.md "Post-Dead refutation channel".)
- Rationale (Laksh): part of the standing tautq mandate ("finish all modules") — the chaos
  suite's §5 assertions define correctness; this was a straight bug against them.
