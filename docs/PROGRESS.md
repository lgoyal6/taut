# taut — PROGRESS

State log. Start each session by reading this; update it at the end of each session.
Format: newest at top within each week.

---

## Final status — v0.1.0 (2026-07-21)

Shipped and verified: codec + CRC32C (hand-laid golden vector + libFuzzer, ASan/UBSan-clean),
level-triggered epoll loop + UDP transport, hand-built timer min-heap, deterministic SimNet,
reliable session (SACK + fast-retransmit, RTO Jacobson/Karn, classes 0/1/2, flow control +
zero-window persist probe), SWIM membership + `mesh_node`, file-transfer demos.
**Week-2 hard checkpoint PASSED 20/20** (10 MB, 5 % loss, sha256-identical). Benchmarks real —
all three plots: p99 ~6–12× below kernel TCP under 5–20 % loss at ~1.2× bandwidth overhead; TCP
wins bulk throughput ~27× (disclosed). **CI green** after the clang-tidy fix (build/test dev +
release, format, clang-tidy, fuzz-smoke, sim-suite). Tagged **v0.1.0**. Resume bullet drafted in
`docs/RESUME.md`.

**Honest gaps (not done):** Week-5 profiling + `recvmmsg`/`sendmmsg` batching (optional, skipped);
`DURATION=60` publication run (used 10 s — tail *ranking* robust); the 25 `docs/DECISIONS`
rationales are blank and the weeks-2–5 comprehension gates were dropped (full-drop mode) — the
code and numbers are real, but defending the design cold is homework still owed (see RESUME.md).

---

## Integration + Week-2 checkpoint (2026-07-20)

Four parallel feature branches merged into `main` (core, bench, swim, infra), integrated
build verified in Lima: **55/55 unit tests pass**, clang-format clean, all demos build,
`mesh_node` reconverges after a partition (invariant 6 held).

**WEEK-2 HARD CHECKPOINT (PLAN §9): PASSED — 20/20 GREEN.** `sudo bench/scripts/soak.sh`:
twenty consecutive 10 MB transfers over veth+netem @ 5% loss (delay 30ms±10ms, reorder 1%,
dup 0.5%), every one sha256-identical. Loss sweep shows goodput 1049→182 kB/s across 0→20%.
Week 2 is complete; the abandon protocol is not triggered.

**Benchmarks (§7): real data now committed.** Fixed the two harness bugs (`SO_REUSEADDR` on
`RealUdpTransport::bind` → taut rebinds per loss point; ENet works on a fresh topology), then
ran the full clean matrix `LOSSES="0 1 5 10 20" RUNS=5 DURATION=10 RUN_OPENLOOP=1` (RTT 30 ms,
512 B, seeds 1–5, veth+netns, offloads off).
- **Headline (p99 RR latency, median):** at 5% loss taut c1 62 / c2 92 vs TCP 409 / ENet 148 ms;
  at 20% loss taut ~280 vs TCP 3363 / ENet 2997 ms — taut's tail is ~6–12× below TCP's.
- **Clean-link throughput:** TCP 235 Mbit/s vs taut 8.7 (ENet 16) — TCP wins bulk ~27%.
- **Bandwidth overhead (sustained load):** taut ~1.13× (0%) → ~1.27× (10%) bytes-on-wire vs
  TCP ~1.06–1.17× — ~15–20% more wire bytes for the tail win. All 3 plots generated
  (`latency_vs_loss`, `throughput_cleanlink`, `overhead_vs_loss`).
- **plot.py bug fixed:** overhead join used a `received` column that doesn't exist (the CSV
  column is `replies`) → empty overhead table; corrected.
- **Honesty caveats (in BENCHMARKS.md/README):** (1) one-outstanding RR ⇒ class 1 ≈ class 2
  (c1's head-of-line-blocking win needs a pipelined load); (2) ENet overhead noisy at high loss
  (median used); (3) DURATION=10 below PLAN §7's 60 s — tail ranking robust, p999 at high loss
  from a few hundred samples.
- **Snag hit + fixed earlier:** a first run orphaned and a relaunch ran concurrently,
  contaminating CSVs; killed all, cleaned netns, re-ran single clean instances (this data).

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

### S1c — reliable session (send ring + acks + retransmit)  (date: 2026-07-20)
- `session.{h,cc}`: per-peer reliable engine over `UdpTransport` — send-buffer ring
  (`std::deque<Slot>`, stores encoded datagrams for straight resend), next-expected
  (TCP-style) cumulative acks, in-order class-2 delivery with `reasm_` buffering,
  exponential-backoff retransmit on RTO (fixed 50 ms for now) via the timer heap.
  poll()/tick() driven. `docs/DESIGN-window.md` (notes the cum_ack = next-expected deviation).
- Verified in Lima: 26/26 tests (3 new) — stop-and-wait (window=1) delivers 100 messages
  exactly once, in order, at 0/5/20% loss (invariants 1, 2, 3, 5). clang-format clean.

### S2 — sliding window + RTO (Jacobson/Karn)  (date: 2026-07-20)
- `rto.{h,cc}`: RFC 6298 SRTT/RTTVAR/RTO estimator; floor = rto_floor (thesis knob), cap 2 s,
  initial 200 ms. `docs/DESIGN-rto.md`.
- `session`: base RTO now from the estimator; RTT sampled only from never-retransmitted acks
  (Karn); window widened past 1 (pipelining).
- Verified in Lima: 32/32 tests (6 new) — RttEstimator formula tests + a window-64 pipelined
  transfer with reorder(jitter)+dup+loss delivering 300 messages exactly once, in order.

### S3 (feat/infra) — send_file/recv_file + netem soak ⇒ HARD CHECKPOINT GREEN  (date: 2026-07-20)

**Owned files:** demo/{send_file,recv_file}.cc + demo/{sha256,file_xfer}.h, demo/CMakeLists.txt
(distinct block), bench/scripts/{netns_setup,soak}.sh, README.md, .github/workflows/ci.yml,
.clang-tidy. No src/ / codec / session / swim / bench/*.cc touched.

- **Demos:** file transfer over class 2 — message 0 is an 8-byte LE length header, then
  1024-byte chunks; the receiver reassembles in order and **lingers ~2 s re-acking the
  sender's retransmits** so a lost *final* ack can't strand an otherwise-complete transfer
  (the one non-obvious correctness point for a 20/20 checkpoint). sha256 printed both ends
  via a vendored `demo/sha256.h` (no src/ dep; verified byte-for-byte against `sha256sum`).
- **netns_setup.sh:** the §6.5 fixture — veth across `taut-a`(10.9.0.1)/`taut-b`(10.9.0.2),
  netem both directions, tso/gso/gro off; `up|netem|down|status` subcommands.
- **soak.sh:** the checkpoint runner + loss sweep. Integrity is the system `sha256sum` on
  both files (ground truth, independent of the binaries' printed digest); each run uses fresh
  random content, so 20/20 is 20 independent proofs.
- **CI:** added the three TODO jobs — clang-tidy (`.clang-tidy` check set the library passes
  clean under warnings-as-errors; the 100+ raw hits were all pure-style/noise, zero real
  defects), 60 s `fuzz_decode` smoke, deterministic SimNet scenario suite (`SimNet.*` +
  `Session.*`). All three validated locally in Lima.
- **README:** rewritten for a 90-second skim — thesis, ASCII architecture diagram, class
  table, run/soak instructions, honest limitations (no CC, no security, single-thread,
  IPv4/no-TIME_WAIT), benchmark-graph placeholders (feat/bench owns the §7 report).

**HARD CHECKPOINT (PLAN §9): GREEN — 20/20** consecutive 10 MB transfers across the veth at
`loss 5% delay 30ms 10ms reorder 1% duplicate 0.5%`, every one sha256-identical, ~28–37 s/run
(~280–360 kB/s). Loss sweep {0,1,5,10,20}% all sha256-identical; rough soak goodput
931 → 633 → 336 → 218 → 105 kB/s.

**Honest notes (not fudged):**
- Recovery here is **RTO-only** — SACK + fast-retransmit is feat/core and is *not* merged in
  this worktree, so this is the genuine Week-2 protocol. That is exactly why goodput collapses
  superlinearly under loss (the §12-Q4 / W3-drill story); Week 3's SACK is what fixes it.
- Even at 0 % loss, goodput is window-limited (~64 pkts / RTT ≈ 930 kB/s), which matches
  theory and is a sanity check that the harness isn't strawmanning.
- The soak's kB/s is a coarse soak metric, **not** the §7 benchmark (proper methodology +
  TCP/ENet baselines, owned by feat/bench). No benchmark numbers are quoted in README prose.
- CI jobs verified by running their exact commands in Lima; first push confirms on
  ubuntu-latest. `.clang-tidy` is a new root file (config backing the clang-tidy CI job).

**Week 2 status: CLOSED — hard checkpoint green.** (Other worktrees continue weeks 2-tail→5.)

---

## Week 4 — SWIM membership (feat/swim worktree)

### S1–S2 — SWIM state machine + mesh_node demo  (date: 2026-07-20)
- `swim.{h,cc}`: per-node membership + failure detector over `UdpTransport`. Protocol period
  T=1s; randomized round-robin target; direct PING (300ms) → PING_REQ k=3 → SUSPECT at period
  end → DEAD after 3s. Incarnation-number precedence (Suspect beats Alive at equal inc; only the
  accused refutes with inc+1; Dead terminal). Gossip piggybacked in the packet **payload** (no
  codec/flags edits — lane rule), one rumor/subject, least-transmitted-first, budget ceil(3·lnN)
  (=5 at N=5); unresolved suspicions re-injected each period so partition-heal reconverges.
  Indirect-probe relaying via a small `relays_` table. `poll()`/`tick()` driven like `Session`.
  `docs/DESIGN-swim.md` (incl. detection-time math for T=1s,k=3,N=5) + `DECISIONS.md` D17–D21.
- `demo/mesh_node.cc`: 5 nodes on SimNet + virtual clock, live membership table; partitions one
  node and heals it, printing time-to-detect / time-to-reconverge. Portable (no epoll) — built
  in a distinct CMake block. Sample run (seed 20260720): detect **1020ms**, reconverge **2040ms**,
  victim never falsely DEAD.
- `tests/unit/swim_test.cc` (7 tests, seeded SimNet): incarnation precedence incl. the exact
  stale-rumor interleaving + self-refutation + Dead-terminal; crash → suspect → dead detection;
  **partition+heal reconverges** (4 seeds); **invariant 6** — a live reachable node is never
  confirmed dead while refutations flow (5 seeds, 10% loss, 30s virtual); JOIN learns roster.
  A `LinkFilter` transport decorator models partitions without touching SimNet.
- Verified in Lima under ASan/UBSan: **39/39 ctest green** (7 new SWIM tests); mesh_node runs
  clean; clang-format clean on all changed files.

**Note on the reconvergence test timing:** at N=5 the spec's detect-before-dead ordering sits
on the boundary `suspicion_timeout > (N-2)·T`; that one test compresses the clock (period 500ms,
suspicion 4s) for determinism across seeds — mechanism is identical to the T=1s demo. Detail in
docs/DESIGN-swim.md.

---

## Parallel workstreams (2026-07-20) — weeks 2-tail through 5

Split across 4 git worktrees / branches to run as parallel agent sessions. File ownership is
disjoint to minimize conflict; shared files (CMakeLists.txt, docs/PROGRESS.md, DECISIONS.md)
merge trivially. Merge **feat/core first** (others build on the finished protocol).
- **feat/core** — SACK + fast-retransmit, class 0/1/2 rx semantics, flow control + zero-window
  probe. Owns codec.{h,cc}, session.{h,cc}, new rx/flow, their tests.
- **feat/bench** — latency/goodput benchmarks vs kernel TCP (TCP_NODELAY) + ENet; bench/,
  BENCHMARKS.md, ENet via FetchContent.
- **feat/swim** — SWIM membership state machine + mesh_node demo (SimNet; no codec edits).
- **feat/infra** — send_file/recv_file, veth/netem soak + the 20× checkpoint runner, README,
  CI jobs (clang-tidy, fuzz smoke, sim suite).

<!-- BEGIN feat/core -->
### feat/core — SACK + fast-retransmit, reliability classes, flow control  (date: 2026-07-20)

Owns codec.{h,cc}, session.{h,cc}, and new tests under tests/unit/. Design defaults to PLAN
§5 (gates suspended per the amendment; functional verification retained).

**Shipped:**
- **SACK (flags bit0):** codec now encodes/decodes the 8-byte LE bitmap at offset 21
  (bit i = reliable seq cum_ack+1+i received). `Packet.sack`; SackPresent read pre-CRC only to
  size the datagram (documented in DESIGN-codec.md). Golden `kGoldenSack` (31 B, CRC
  0x5A4D396A) + round-trip / truncation / bit-flip / membership-unsupported tests.
- **Fast retransmit (§5.4):** `process_sack` marks SACKed slots (cancels their RTO, samples RTT
  early) and fast-retransmits any un-SACKed slot with ≥ 3 SACKed slots above it (Reno on the
  bitmap). `set_sack_enabled` toggles it for A/B.
- **Reliability classes on rx (§5.3):** class 0 = separate 64-seq anti-replay dedup window,
  deliver-immediately, never acked/retransmitted; class 1 = deliver-on-arrival with a
  payload-less reasm marker; class 2 = in-order reassembly (as before). DESIGN-window.md.
- **Flow control (§5.6):** `adv_window` = free app-delivery-queue slots; sender bounds in-flight
  by min(window_pkts, adv_window) via a bounded `pending_` queue + `pump()`. Zero-window
  PROBE_WINDOW persist timer (100 ms, ×2, cap 1 s) + window-update ack on drain. `set_receiving`
  models app backpressure. New docs/DESIGN-flow.md (incl. the invariant-3 class-1 nuance).

**Verified in Lima under ASan/UBSan:** 48/48 unit tests (14 new: 6 SACK codec, 3 SACK/fast-rtx,
4 classes, 3 flow) — incl. SimNet stalled-receiver (5 s, no deadlock, no overflow, invariants
1/2/3/5), probe-loss-no-deadlock, class-1 exactly-once under reorder+dup+loss, class-0 dedup
with zero retransmits, and class-0 loss not stalling reliable. SACK A/B on a 5 %-loss sim:
retransmits 271→16 (~17×), iterations 576→315. `fuzz_decode` 2.68M execs / 46 s ASan-clean.
clang-format clean. Deterministic `tests/unit/test_link.h` added for scripted drop scenarios.
<!-- END feat/core -->


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
