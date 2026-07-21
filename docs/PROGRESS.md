# taut — PROGRESS

State log. Start each session by reading this; update it at the end of each session.
Format: newest at top within each week.

---

## Week 2 — reliability core (⇒ HARD CHECKPOINT)

**Operating-model change (2026-07-20):** Laksh chose "full drop" — the §2 comprehension
gates and §10 weekly drills are suspended for weeks 2–5 (see CLAUDE.md amendment). Claude
implements + functionally verifies each module; Laksh commits. Design decisions default to
PLAN §5 reference design. Functional gates retained: unit/sim tests, the 10 MB / 5% loss /
20× checkpoint, and real benchmarks.

### S1a — timer min-heap  (date: 2026-07-20)
- `timers.{h,cc}`: hand-built binary min-heap of deadlines; lazy deletion (cancel marks an
  id dead, pruned when it reaches the root). schedule/cancel/next_deadline/pop_due/empty.
  `docs/DESIGN-timers.md`.
- Verified in Lima under ASan: 19/19 unit tests (5 new timer tests) green; clang-format clean.

### S1b — SimNet deterministic sim transport  (date: 2026-07-20)
- `sim_net.{h,cc}`: in-process `UdpTransport` with seeded `mt19937_64`, virtual clock, and
  per-datagram loss/dup/delay/jitter; earliest-due delivery (reorder emerges from jitter).
  `docs/DESIGN-simnet.md`.
- Verified in Lima: 23/23 tests (4 new) — in-order no-loss delivery, clock-gated delay,
  seed-reproducible loss, intact round-trip. clang-format clean.

**Remaining for Week 2:** send-buffer ring + stop-and-wait ARQ (reliable at 5% sim loss);
then sliding window (64) + cumulative acks + RTO/Karn + SimNet invariant scenarios; then
send_file/recv_file + veth/netem soak → the hard checkpoint.

## Week 1 — foundations (codec + fuzz + event loop skeleton)

### S3 — transport + epoll loop + echo demo + fuzzer  (date: 2026-07-20)

**Decisions D13–D16** (docs/DECISIONS.md): level-triggered epoll; UdpTransport abstraction
(real-socket impl now, SimNet week 2); eventfd wakeup now / timerfd deferred; fuzz_decode
with CRC-patch coverage + round-trip invariant.

**Shipped (all Linux-only except the codec/crc core):**
- `transport.{h,cc}`: `UdpTransport` interface + `RealUdpTransport` (nonblocking UDP,
  bind/send/recv/now/fd/local_endpoint) + `make_endpoint`.
- `loop.{h,cc}`: single-threaded **level-triggered** `EventLoop` — epoll over the UDP fd +
  eventfd wakeup/shutdown; per-fd drain-and-dispatch. timerfd slot deferred to week 2.
- `demo/echo_{server,client}.cc`: two-process datagram echo.
- `fuzz/fuzz_decode.cc` + `run_fuzz.sh`: libFuzzer harness, CRC-patch coverage mode,
  re-encode round-trip invariant.
- `tests/unit/loop_test.cc`: in-process epoll echo integration test.

**Verified in Lima:** 14/14 unit tests pass (incl. `EventLoop.EchoesDatagramThroughEpoll`);
`fuzz_decode` 1.27M execs in 21 s ASan/UBSan-clean (cov 110); two-process echo 10/10.
clang-format clean. *(Follow-ups: curated seed corpus commit; full ≥1h fuzz run in CI.)*

**GATE + W1 DRILL OWED (blocking before Week 2):** S3 gate (level/edge reasoning) + the
§10 Week-1 defense drill. Not yet run.

### S2 — crc32c + codec  (date: 2026-07-20)

**Decisions D6–D12 recorded** in docs/DECISIONS.md (all "go with recommended"):
table-driven CRC; software-first-then-HW; DecodeError enum; zero-copy payload; shift-based
LE; two-chunk CRC; GoogleTest.

**Shipped:**
- `crc32c.{h,cc}`: table-driven Sarwate CRC-32C (reflected Castagnoli poly), one-shot +
  incremental API. `docs/DESIGN-crc32c.md`.
- `codec.{h,cc}`: base header + payload encode/decode, shift-based LE helpers, CRC over
  datagram-with-crc-zeroed (one-shot on encode, incremental 3-chunk on decode), zero-copy
  payload view, `DecodeError` taxonomy. SACK/membership deferred (non-zero flags rejected).
- **Golden vector locked** — Laksh hand-laid the DATA packet (corrected on 2nd attempt;
  1st had 5 errors: missing magic, wrong ver|type nibble, merged flags+class, 16-bit
  seq/cum_ack, crc-after-payload + padding). Final: `7a 75 11 00 02 01 00 00 00 00 00 00 00
  40 00 02 00 cb 4e 6f c3 68 69`, CRC `0xC36F4ECB`. Committed as `kGolden`.
- Verified in Lima under ASan/UBSan: **13/13 tests pass** (crc32c 4 + codec 8 + smoke);
  encode reproduces the golden bytes exactly; bit-flip/bad-magic/bad-version/truncation/
  length-overrun all rejected. clang-format clean.

**S2 GATE: PASSED** (2026-07-20). Explain-back ✓ (minor: initially placed flags-check
pre-CRC — it's post-CRC alongside class; invented enum names). Prediction ✓ — called
`BadCrc` on an offset-4 flip with stale CRC, and *why* it beats the class check; verified
by running decode. Hand-trace 3b ✓. Initial misses on 3a/4A (both the same mechanic: the
zeroed crc field is hashed as 4 zero bytes → 23 bytes, not skipped → 19) corrected on
re-quiz. Week 2 unblocked.

**Week 1 remaining:** none — S3 shipped (see its entry above). Week 1 closes after the S3
gate + W1 defense drill.

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

**Committed + pushed:** root commit `de4073c` on `main`, pushed to
`github.com/lgoyal6/taut` (private). **CI green** — `ci` run success in 35 s (dev + release
build/test + clang-format check). S1 is DONE.

**Owed by Laksh before S2 CORE implementation can start:**
1. ~~Lima up + build/test/format green; commit + push; CI green~~ — done.
2. Fill the `Rationale (Laksh)` lines in `docs/DECISIONS.md` (D1–D5), in his own words.
3. Hand-lay-out the first codec golden vector on paper (§6.1) — precedes the S2 code.

(S2 progress tracked in its own entry above.)

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
