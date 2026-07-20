# taut — PROGRESS

State log. Start each session by reading this; update it at the end of each session.
Format: newest at top within each week.

---

## Week 1 — foundations (codec + fuzz + event loop skeleton)

### S1 — repo scaffold + codec design  (date: 2026-07-20)

**Shipped (non-CORE — no gate owed):**
- Build system: top-level `CMakeLists.txt`, `CMakePresets.json` (`dev` = Debug+ASan/UBSan,
  `release`), `cmake/Warnings.cmake` (-Wall -Wextra -Werror + strict extras),
  `cmake/Sanitizers.cmake`, `.clang-format`, `.gitignore`.
- CI skeleton: `.github/workflows/ci.yml` — build+test matrix {dev, release} + clang-format
  check. clang-tidy / fuzz-smoke / sim-suite jobs stubbed as TODO to add as modules land
  (honest: no job runs before its code exists).
- Public API sketch (declarations only, no CORE logic): `include/taut/{types,config,node,
  version}.h`, `src/version.cc`, `tests/smoke.cc` (build/link/parse sanity, keeps CI green
  until real unit tests arrive in S2).
- `README.md`, `LICENSE` (MIT), `docs/DESIGN-codec.md` (finalized §5.2 wire format),
  `docs/DECISIONS.md` (D1–D5 recorded).

**Decisions (D1–D5, docs/DECISIONS.md):** Lima env; single reliable seq space + separate
class-0 dedup counter; cum_ack + 64-bit SACK bitmap; CRC32C; LE / 1200 B / magic 0x7A75.
All reference defaults, plus the class-0 clarification (DESIGN-codec.md #3).

**Verified in Lima (Ubuntu 24.04 aarch64, clang 18, cmake 3.28):** `dev`
(Debug+ASan/UBSan) and `release` both configure + build + `ctest` green (smoke test);
no CMake warnings; clang-format applied and clean. Repo mounts read-write via virtiofs at
`/Users/lakshgoyal/taut`. CI not yet run (first push validates the GitHub Actions matrix).

**Owed by Laksh before S2 CORE implementation can start:**
1. ~~Lima up + build/test/format green~~ — done (verified above); still: commit + push.
2. Fill the `Rationale (Laksh)` lines in `docs/DECISIONS.md` (D1–D5), in his own words.
3. Hand-compute the first codec golden vectors on paper (§6.1) — this precedes the S2 code.

**S2 (next, CORE — gated):** crc32c + codec encode/decode. Starts with a design brief and
Laksh's golden vectors; ends with the comprehension gate (explain-back, prediction,
hand-trace, planted-bug hunt) before any further module.

---

## Week 0 — commitment gate (§12)

**Date:** 2026-07-20
**Result: PASS — 5/5 derivable.** Bar is 3/5. We build taut (not the §13 fallback).

Per-question (examiner notes — these feed the W1/W2/W3 defense drills):

- **Q1 (windows/SACK): pass.** Core concept solid: cumulative-only ack is blind above
  the gap → sender retransmits conservatively/wastefully; SACK bitmap → surgical
  retransmit of only 4 and 5. *Gap:* answered the data-structure part with Linux TCP
  internals (sk_buff linked list + timer **wheel**). taut deliberately uses a hand-built
  **ring** of in-flight slots (§5.4) and a hand-built **binary min-heap** of deadlines
  (§5.7); the timer wheel is the know-but-don't-build follow-up. Also cum_ack = highest
  seq with no gaps below = 3 here, not 4.
- **Q2 (Karn): pass, clean.** Ambiguity both directions, deflation → RTO too small →
  premature-timeout cascade, fix = drop retransmit RTT samples + exponential backoff.
- **Q3 (flow control/deadlock): pass.** Halt at in_flight == adv_window; signal in the
  window field; persist-timer probes break the lost-reopening-ack deadlock. *taut
  specifics to internalize:* adv_window is in **packets** not bytes, and the probe is a
  dedicated PROBE_WINDOW packet (§5.6).
- **Q4 (goodput collapse): pass on mechanism.** Fast retransmit on 3 dup-acks / SACK is
  the right recovery. *Trap flagged:* "resets window to 1 MSS" is congestion-control /
  slow-start, which taut deliberately omits (§5.8, fixed window). taut's stall is pure
  RTO-wait + head-of-line blocking — no cwnd to collapse.
- **Q5 (SWIM): pass, clean.** Indirect probes disambiguate local/asymmetric link failure
  from real death; incarnation numbers let the accused refute; naive timer false-kills a
  briefly-CPU-pegged healthy node.

**Standing pattern to correct in every brief:** reasons fluently in TCP; reached for TCP
machinery taut drops/changes (timer wheel, cwnd/slow-start, byte-windows) 3×. taut's
interview value is knowing *where it diverges from TCP and why* — sharpen that edge.

**Owed before implementation:** none (gate passed). Next: Week 1 S1 design decisions
(dev env + codec wire format) recorded in docs/DECISIONS.md, then scaffolding.
