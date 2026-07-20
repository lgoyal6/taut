# taut — reliable-UDP transport + SWIM membership, in C++

> **One-liner:** a purpose-built reliable transport for small-message meshes on lossy
> networks — sliding-window ARQ with SACK, adaptive RTO, per-message reliability
> classes, and SWIM failure detection — fuzz-hardened, fault-injected with netem,
> and benchmarked honestly against kernel TCP and ENet.
>
> (The name is a placeholder — a taut line over a lossy link. Rename freely; nothing
> below depends on it.)

**Timeline:** 5 weeks at 10–12 h/week (~3 sessions of 3–4 h each).
**Hard checkpoint:** end of week 2 (see §9). **Owner:** Laksh Goyal, sole author of all core code.

---

## 1. Why this project exists (context for any fresh Claude session)

This project was chosen 2026-07-20 after a full audit of Laksh's GitHub found:

- Zero C/C++/Java evidence anywhere in the portfolio.
- The portfolio's flagship "systems" bullets (Pylon's UDP mesh, Vigil's policy gate,
  Winnow's paper reimplementations) trace to **teammates' commits** on team/hackathon
  repos. There is no project that is simultaneously deep and single-author.
- Zero evidence, in any repo, of: manual memory management, hand-built concurrency or
  data structures, on-disk or wire formats, protocol reliability mechanics, fuzzing,
  property tests, fault injection, or a benchmark against a real named baseline.
- Pylon's actual mesh (`node.py`, teammate-authored) is pipe-delimited UTF-8 text over
  UDP: no framing, no sequencing, no acks, no retransmission; "gossip" is a 1 s
  full-peer-list broadcast with a 5 s timeout reaper; the mesh has zero tests.

**Purpose (in priority order):**
1. A resume project for Google SWE/SRE intern applications strong enough to displace
   the Winnow bullet (and to let Vigil be reframed as a team project).
2. Something Laksh can defend **cold** in a systems interview — every design decision
   made by him, and every line understood deeply enough to re-derive at a whiteboard.

**Explicit non-goals:** users, revenue, production readiness, cross-platform support,
encryption, congestion control (deliberately out of scope — see §5.8), kernel bypass.

**The thesis (what makes this more than a reimplementation):** general-purpose
transports carry obligations we can drop. TCP must deliver bytes strictly in order
(head-of-line blocking), must be fair (congestion control), and won't retransmit
faster than a ~200 ms minimum RTO. We serve one niche — small telemetry messages on
lossy links — and trade bandwidth for tail latency. Expected result: **much lower p99
message latency than TCP at 5–10 % loss, at a measured cost in bandwidth overhead and
clean-link throughput.** Both sides of that trade get plotted. A graph where we win at
everything is a bug.

---

## 2. Division of labor — AI-IMPLEMENTED, HUMAN-DIRECTED (the most important section)

Chosen model (Laksh's explicit decision, 2026-07-20): **Claude writes the code;
Laksh directs the design and must pass comprehension gates.** Rationale: he learns
better from worked implementations plus briefings than from typing under deadline.

The risk this model creates is hollowness — an impressive repo whose owner can't
survive follow-up questions, which is exactly the failure the original audit found in
his portfolio (flagship bullets describing code teammates wrote). The compensation is
that **every module ships through a mandatory active-learning loop, and nothing
advances past a failed gate.** The gates are the product. Skipping them to "move
faster" produces a repo worth zero for both stated goals.

**The per-module loop (Claude enforces the order; applies to every CORE module —
codec/CRC32C, window/ARQ/SACK, RTO+timers, rx/classes, flow control, SWIM, epoll loop):**

1. **Design brief (before any code).** Claude presents the problem, 2–3 candidate
   designs with real tradeoffs, and a recommendation. **Laksh makes the call** on each
   module's decision points (window sizing, SACK bitmap vs ranges, level- vs
   edge-triggered epoll, per-class vs single seq space, timer heap vs wheel, …) and
   gives a one-sentence why. Recorded in `docs/DECISIONS.md`. Design ownership is
   real ownership — interviews probe decisions far more than syntax.
2. **Implementation.** Claude writes the module and its tests in small, reviewable
   commits — never the whole subsystem in one dump.
3. **Walkthrough brief.** Guided tour: the data structures, the invariants, the 2–3
   subtlest lines, and "what breaks if we change X." Laksh asks until nothing is fuzzy.
4. **Comprehension gate — must pass before the next module starts:**
   a. *File-blind explain-back:* Laksh explains the module's state machine and edge
      cases from memory, no screen.
   b. *Prediction:* given a scenario ("acks for 1-3 and 6-8 arrive; packet 4's timer
      fires 10 ms later — walk the ring"), Laksh predicts behavior **before** the test
      runs; then they run it.
   c. *Hand-trace:* one worked trace on paper (e.g., SRTT/RTTVAR/RTO across five
      samples including one retransmit).
   d. *Bug hunt:* Claude re-presents the module on a scratch branch with 1–2 planted
      realistic defects (off-by-one at seq wraparound, missing Karn guard, ack past
      window edge…). Laksh must find them by reading. Planted bugs never merge.
5. Only then: next module.

**Laksh still personally produces (cheap, disproportionate value):**
- The golden test vectors, computed by hand on paper (§6.1) — the wire format becomes
  his in an hour.
- Every `docs/DECISIONS.md` entry and the README/BENCHMARKS analysis prose, in his
  own words.
- All whiteboard derivations in the weekly defense drills (§10).

**Git honesty:** real incremental commits (no end-of-project squash); Claude-authored
commits keep the standard `Co-Authored-By: Claude <noreply@anthropic.com>` trailer.
The history never pretends to be hand-typed — what the resume says is Laksh's call,
but the repo manufactures no false evidence, and what makes any claim survivable is
the gates and drills, not the git log.

---

## 3. Deliverables & definition of done

1. `libtaut` static library + public C++ API (§5.1), Linux/epoll, C++20.
2. Demo binaries: `send_file` / `recv_file` (reliability proof) and `mesh_node`
   (N-process SWIM demo with a live membership table).
3. Fuzzing: `fuzz_decode` (codec) and `fuzz_session` (packet-sequence → state machine),
   both ASan-clean after ≥ 1 CPU-hour each, seed corpus committed.
4. Deterministic simulation test suite: loss/reorder/dup/delay schedules from a seed;
   any failure reproduces from `--seed N`.
5. netem soak: scripted, invariant-checked (§6.4), green 20×.
6. Benchmark report (`docs/BENCHMARKS.md` + README graphs): p50/p99/p999 message
   latency and goodput vs **kernel TCP (TCP_NODELAY)** and **ENet**, across loss
   0/1/5/10/20 % — including the plots where taut loses (§7).
7. README written for a skimming interviewer: thesis, architecture diagram, graphs,
   limitations section that names what was deliberately not built.
8. CI (GitHub Actions): build matrix {Debug+ASan/UBSan, Release}, clang-tidy,
   unit + sim tests, 60 s fuzz smoke.
9. Resume bullet with metrics filled (§11) + the resume-swap checklist executed.

---

## 4. Repository layout

```
taut/
├── CMakeLists.txt
├── cmake/                     # toolchain, sanitizer, FetchContent(ENet) modules
├── include/taut/              # public API: node.h, config.h, types.h
├── src/                       # CORE — built via the §2 module loop, gate-by-gate
│   ├── codec.cc/.h            # wire format encode/decode + CRC32C
│   ├── crc32c.cc/.h           # HW-accelerated w/ software fallback
│   ├── window.cc/.h           # send buffer ring, ack/SACK processing, retransmit
│   ├── rto.cc/.h              # SRTT/RTTVAR/RTO per RFC 6298 + Karn + floor
│   ├── timers.cc/.h           # binary min-heap of deadlines
│   ├── rx.cc/.h               # receive path: dedup, per-class ordering, delivery
│   ├── flow.cc/.h             # advertised window, zero-window probe
│   ├── swim.cc/.h             # membership state machine
│   ├── loop.cc/.h             # epoll event loop, socket I/O, dispatch
│   └── transport.cc/.h        # UdpTransport interface + real-socket impl
├── tests/
│   ├── unit/                  # per-module; golden vectors in tests/golden/
│   └── sim/                   # deterministic simulator + protocol scenario tests
├── fuzz/                      # fuzz_decode.cc, fuzz_session.cc, corpus/
├── bench/
│   ├── latency_bench.cc       # taut message-latency load generator
│   ├── tcp_baseline.cc        # length-prefixed messages over TCP_NODELAY
│   ├── enet_baseline.cc       # same workload over ENet
│   └── scripts/               # netns_setup.sh, run_matrix.sh, plot.py
├── demo/                      # send_file.cc, recv_file.cc, mesh_node.cc
├── docs/                      # DESIGN-*.md (one per module), BENCHMARKS.md
├── PLAN.md  CLAUDE.md  README.md  LICENSE (MIT)
```

---

## 5. Design specification

Everything in §5 is a **reference design**: strong defaults chosen so week 1 isn't
spent bikeshedding. Laksh may deviate — but every deviation gets a sentence in the
relevant `docs/DESIGN-*.md` saying why. "I changed the reference design because X"
is interview material; silent drift is not.

### 5.1 Public API (sketch — finalize in week 1)

```cpp
taut::Config cfg;
cfg.rto_floor    = std::chrono::milliseconds(25);   // the thesis knob
cfg.window_pkts  = 64;
cfg.mtu_payload  = 1200;                            // conservative; QUIC's choice

taut::Node node(cfg);
node.bind("0.0.0.0", 7000);
taut::PeerId p = node.add_peer("10.0.0.2", 7000);

node.send(p, taut::Class::ReliableOrdered, payload_bytes);

node.on_message([](taut::PeerId, taut::Class, std::span<const std::byte>) { ... });
node.on_peer_state([](taut::PeerId, taut::MemberState) { ... });      // SWIM events

node.run();          // blocking event loop; node.poll(timeout) also provided
```

Single-threaded by design (§5.7). No exceptions across the API boundary; return
`expected`-style results. No raw `new`/`delete` outside owned buffer internals.

### 5.2 Wire format (byte-level reference)

All integers little-endian. One datagram = one taut packet. Max datagram size
**1200 B** (rationale: below practical MTU everywhere; same choice as QUIC RFC 9000).

```
offset size field
0      2    magic          0x7A 0x75  ("zu" — pick your own)
2      1    ver(4b)|type(4b)   version=1; type: DATA=1 ACK=2 PING=3 PING_REQ=4
                                PONG=5 JOIN=6 PROBE_WINDOW=7
3      1    flags          bit0 SACK-present, bit1 membership-piggyback,
                           bit2 keyed-CRC (see §6.2), rest reserved=0
4      1    class          0=Unreliable 1=ReliableUnordered 2=ReliableOrdered
5      4    seq            u32, per-peer-pair, single sequence space
9      4    cum_ack        highest seq received with no gaps below it
13     2    adv_window     receiver's free buffer, in packets (flow control)
15     2    payload_len
17     4    crc32c         over entire datagram with this field zeroed
21     [8]  sack_bitmap    iff flags.bit0 — bits = (cum_ack+1 .. cum_ack+64) rx'd
[+]    [n]  membership     iff flags.bit1 — up to 3 × 12 B entries:
                           {addr4, port u16, incarnation u32, state u8, pad u8}
[+]    ...  payload        (payload_len bytes)
```

Decisions embedded here (know why for each — they get asked):
- **Single seq space per peer-pair**, ordering enforced only for class 2 at the
  receiver. Simpler than per-class spaces; document the duplicate-detection
  consequence for class 0.
- **Cumulative ack + 64-bit SACK bitmap** rather than SACK ranges: fixed size, O(1),
  covers a 64-packet window exactly.
- **CRC32C** (Castagnoli): hardware instruction on x86 (SSE4.2) and ARM; write the
  software fallback yourself, verify against known vectors, then enable the HW path.
- **Acks ride on everything**: every outgoing packet carries current cum_ack/window;
  pure ACK packets are sent when there's no reverse traffic (delayed up to 10 ms or
  every 2nd packet, whichever first — classic delayed-ack shape).
- Handshake: minimal 2-way JOIN/JOIN-ACK to exchange initial seq + config. Do not
  build TIME_WAIT machinery; document the restart-ambiguity limitation honestly.

### 5.3 Reliability classes (the semantic win over TCP)

| class | delivery guarantee | retransmit? | receive-side behavior |
|---|---|---|---|
| 0 Unreliable | may drop, may reorder, no dup | no | dedup window only, deliver immediately |
| 1 ReliableUnordered | exactly-once, any order | yes | dedup + deliver on arrival |
| 2 ReliableOrdered | exactly-once, in order | yes | reassembly buffer, deliver in seq order |

Class 1 is the latency star: a retransmitted packet doesn't block its successors.
The benchmark story (§7) leans on class 1 vs TCP (which is forced to behave like
class 2 for everything).

### 5.4 ARQ / send path

- Send buffer: **hand-built ring** of in-flight packet slots (seq, deadline,
  transmit_count, retransmitted-flag, payload copy). Capacity = `window_pkts`.
- Window: fixed size in v1 (default 64). Sender blocks (or reports backpressure) when
  `in_flight == min(window_pkts, adv_window)`.
- On ACK: advance ring tail past cum_ack; mark SACKed slots; **fast retransmit** any
  un-SACKed slot with ≥ 3 SACKed slots above it (the Reno fast-retransmit idea,
  applied to the bitmap).
- On timer: retransmit expired slot, double its RTO (cap 2 s), set retransmitted-flag
  (Karn: its next ack yields no RTT sample).

### 5.5 RTO (RFC 6298, with the thesis deviation)

```
first sample:  SRTT = R;         RTTVAR = R/2
after:         RTTVAR = 3/4·RTTVAR + 1/4·|SRTT − R'|
               SRTT   = 7/8·SRTT   + 1/8·R'
RTO = SRTT + max(G, 4·RTTVAR)      clamped to [rto_floor, 2 s]
```
- **rto_floor default 25 ms** — deliberate deviation from TCP's ~200 ms Linux minimum.
  This single constant is a large part of why taut beats TCP's tail latency under
  loss; be able to explain why TCP *can't* safely do this on the open internet
  (spurious retransmit storms, fairness) and why a closed mesh can.
- Karn's rule: never sample RTT from a retransmitted packet.

### 5.6 Flow control

- `adv_window` = free slots in the receiver's reassembly/delivery buffer.
- Zero-window: sender stops; a **PROBE_WINDOW** packet goes out on a persist timer
  (start 100 ms, ×2 backoff, cap 1 s) so the window-reopening ack being lost cannot
  deadlock the connection. (Commitment-test Q3 — you'll implement your own answer.)

### 5.7 Event loop & threading

- **One thread. No locks in v1.** `epoll_wait` (level-triggered first — simpler to
  reason about; document the edge-triggered upgrade as future work) over: the UDP
  socket, a `timerfd` armed to the timer-heap's min deadline, and an `eventfd` for
  cross-thread sends if a demo needs one.
- Timer store: **hand-built binary min-heap** keyed by deadline (lazy deletion via
  generation counters). A timer wheel is the classic follow-up question — know the
  tradeoff, don't build it.
- Per-tick order: drain socket → process acks/data → fire due timers → flush pending
  acks → re-arm timerfd.

### 5.8 Explicit non-feature: congestion control

Fixed window, no Reno/CUBIC/BBR. Say so in the README, and be able to whiteboard
what would happen if two taut flows shared a bottleneck (they'd stomp each other and
everyone else) and roughly what adding AIMD would look like. "I know exactly what I
didn't build and why" is a stronger interview position than a half-built CUBIC.

### 5.9 SWIM membership (week 4; first thing cut if week 3 slips)

Per the SWIM paper (Das, Gupta, Motivala, 2002):
- Protocol period T = 1 s. Each period: pick one member round-robin from a shuffled
  list, direct PING (timeout 300 ms); on miss, PING_REQ via **k = 3** other members;
  on total miss → **suspect**.
- Suspect timeout 3 s → dead. Suspicion, alive, and dead updates are gossiped by
  **piggybacking** (§5.2 flags.bit1) with a per-update budget of ~3·log(N) sends.
- **Incarnation numbers**: only the accused node can refute suspicion, by re-announcing
  itself with incarnation+1. This is what prevents a flapping node from being killed
  by stale rumors — and it's what pylon's naive 5 s reaper got wrong.
- Failure-detection demo metric: time-to-detect and time-to-reconverge after an
  `iptables` partition heals, at N = 5 nodes.

---

## 6. Testing & verification strategy

### 6.1 Unit + golden tests
Every CORE module gets plain unit tests as it's built. The codec additionally gets
**golden vectors**: hand-computed byte arrays committed in `tests/golden/` (encode →
exact bytes; decode → exact struct; bit-flip → CRC reject). Write the first vectors
by hand on paper — that exercise is where the format becomes yours.

### 6.2 Fuzzing (libFuzzer + ASan/UBSan)
- `fuzz_decode`: raw bytes → decoder. Must never crash/overflow/UB; malformed input
  returns an error. Trick: the CRC check rejects most random input before it reaches
  interesting code — add a build flag where the fuzzer computes/patches a valid CRC
  (or a keyed-CRC bypass) so coverage reaches the parser guts. Mention this in the
  README; it's a sophisticated touch.
- `fuzz_session`: fuzzer bytes interpreted as a *script* of events (deliver packet /
  advance time / app-send) driven into a connection pair over the sim transport.
  Asserts internal invariants (§6.4) after every step. This is protocol-state fuzzing,
  not just parser fuzzing — rare in student projects, cheap to build on top of §6.3.
- Budget: ≥ 1 CPU-hour each locally; 60 s smoke in CI; corpus committed.

### 6.3 Deterministic simulation harness
All protocol logic talks to a `UdpTransport` interface (send/recv/now). Two impls:
real sockets, and **SimNet**: in-process, seeded `std::mt19937`, virtual clock, and a
schedule of impairments (loss %, dup %, reorder distance, delay distribution,
partition windows). Protocol tests run on SimNet:
- Same seed → byte-identical run. Every CI failure prints its seed; `--seed N` repros.
- Scenario tests: "10 MB at 20 % loss completes", "partition for 30 s, heal,
  membership reconverges < X s", "receiver stalls 5 s, no deadlock, no overflow".
This harness is *why* the heisenbug failure mode that kills most transport projects
won't kill this one — and it's a legitimately impressive artifact of its own.

### 6.4 Invariants (asserted in sim, fuzz, and soak)
1. Class 1/2: every message sent is delivered exactly once (no loss, no dup).
2. Class 2: delivery order == send order per peer.
3. `in_flight ≤ min(window_pkts, adv_window)` at all times.
4. A retransmitted packet never produces an RTT sample (Karn).
5. Sender makes progress whenever the network permits (no silent stall > RTO cap).
6. SWIM: a live, reachable node is never marked dead while its refutations can flow.

### 6.5 netem soak (real kernel, real sockets)
Run inside the Linux dev VM/container (§8). Use a **veth pair across two network
namespaces** (loopback netem has quirks; veth is realistic and isolated):

```bash
ip netns add A; ip netns add B
ip link add vA type veth peer name vB
ip link set vA netns A; ip link set vB netns B
ip netns exec A ip addr add 10.9.0.1/24 dev vA; ip netns exec A ip link set vA up
ip netns exec B ip addr add 10.9.0.2/24 dev vB; ip netns exec B ip link set vB up
# impairment, both directions:
ip netns exec A tc qdisc add dev vA root netem loss 5% delay 30ms 10ms distribution normal reorder 1% duplicate 0.5%
ip netns exec B tc qdisc add dev vB root netem loss 5% delay 30ms 10ms distribution normal reorder 1% duplicate 0.5%
# for clean TCP baseline comparability:
ip netns exec A ethtool -K vA tso off gso off gro off   # (and on vB)
```

Soak = `send_file` 10 MB, sha256 both ends, invariant log clean, at each of
loss ∈ {0,1,5,10,20} %. **Gate: 20 consecutive green runs at 5 %.**

---

## 7. Benchmark plan (the honesty section)

**Workload:** 512 B messages, Poisson arrivals at a configured rate, 60 s runs,
fixed seeds, ≥ 5 runs per point, medians with min/max whiskers. Raw CSVs committed.

**Baselines — configured fairly or the whole exercise is a strawman:**
- **Kernel TCP** with `TCP_NODELAY` (never benchmark latency against Nagle),
  length-prefixed message framing, offloads disabled on the veth (above).
- **ENet** (vendored via CMake FetchContent), reliable channel, same workload.

**Headline experiment:** p99/p999 message latency vs loss rate (0→20 %) at RTT 30 ms,
three lines: TCP, taut class 2, taut class 1. Expected shape: TCP's tail explodes
past ~200 ms (RTO_min + head-of-line); taut class 2 bounded near RTT + rto_floor;
class 1 lowest. **Also published, same page:** the prices paid —
bandwidth overhead ratio (bytes-on-wire / goodput bytes) vs TCP, and clean-link
(0 % loss) throughput where TCP and ENet should beat taut. If taut wins everything,
the benchmark is broken — find the bug in the harness, not the victory lap.

**Report:** `docs/BENCHMARKS.md` — method, exact commands, hardware, plots, and a
"why we lose where we lose" analysis (that analysis section is worth more in an
interview than the wins).

---

## 8. Dev environment (host is macOS — plan for it now)

epoll and netem are Linux-only. **Primary target: Linux; develop inside a VM or
container on the Mac.** Options, pick one in week 1 session 1:
- **Lima** (recommended): `brew install lima && limactl start` → ubuntu VM with the
  repo mounted; full `tc`/netns/veth capability; runs clang/cmake natively.
- **Docker Desktop / OrbStack**: dev container with `--cap-add=NET_ADMIN` (required
  for tc/netns). Fine for CI-parity; slightly clunkier for netns work.
- CLion/VS Code remote into the VM; `compile_commands.json` for clangd.

CI (GitHub Actions, ubuntu-latest) mirrors it: jobs = {Debug + ASan + UBSan tests,
Release tests, clang-tidy, 60 s fuzz smoke, sim scenario suite}. netem soak stays
local (CI runners allow it with sudo, but keep CI fast; a weekly scheduled soak job
is a nice touch if time allows).

Toolchain: clang 17+, C++20, `-Wall -Wextra -Werror`, clang-format enforced,
CMake ≥ 3.24 + FetchContent (ENet, and GoogleTest or doctest for tests).

---

## 9. Week-by-week plan (exit criteria are binary — green or not)

Budget assumption: 3 sessions/week × ~3.5 h. Each week ends with something demoable;
the project is never in a state where stopping means having nothing.

**[CORE]** markers below now mean: run the full §2 module loop — design brief →
Laksh's recorded decisions → Claude implements → walkthrough → comprehension gate.
A failed gate stops the schedule; the schedule never overrides a gate. (AI-implemented
code is much faster to produce, so the hour budgets shift from typing to gates and
benchmarks — the calendar stays the same, the depth requirement goes up, not down.)

### Week 0 — the gate (≤ 2 h, before any code)
- Take the commitment test (§12) on paper. **< 3/5 derivable → build the fallback
  project instead** (§13) — no shame, better artifact for you.
- Read: RFC 6298 (it's 9 pages), the SWIM paper §§1–4, skim KCP's README (their
  latency-vs-bandwidth trade is your thesis stated by someone else), skim ENet docs.

### Week 1 — foundations (codec + fuzz + event loop skeleton)
- S1: repo init, CMake, sanitizer presets, CI skeleton, Lima/Docker env working;
  `docs/DESIGN-codec.md`: finalize the §5.2 layout (your call on every field).
- S2: **[CORE]** `crc32c` (software impl vs known vectors, then HW path) and
  `codec` encode/decode; golden vectors hand-written first; unit tests green.
- S3: `fuzz_decode` running (harness may be Claude-built; crashes are yours to fix);
  **[CORE]** epoll loop skeleton + `UdpTransport`; two processes echo datagrams.
- **Exit:** fuzzer ≥ 1 CPU-hour clean under ASan; echo demo runs under 20 % netem
  loss (drops fine — no reliability yet); CI green.

### Week 2 — reliability core ⇒ THE CHECKPOINT
- S1: **[CORE]** timer min-heap + send-buffer ring; stop-and-wait ARQ (window=1)
  delivering reliably at 5 % sim loss.
- S2: **[CORE]** sliding window (64), cumulative acks, RTO per §5.5 with Karn;
  SimNet scenario tests for invariants 1/3/4/5.
- S3: `send_file`/`recv_file` demos; netns soak runs; fix what it finds.
- **Exit = HARD CHECKPOINT:** 10 MB file, veth + `netem loss 5% delay 30ms 10ms`,
  sha256-identical, no hang, **20 consecutive runs**, by end of week 2.
- **Miss ⇒ abandon protocol:** (1) polish + publish the codec/fuzzer as a micro-lib
  (honest tiny artifact), (2) pivot to the 2-week fallback (§13). Do not enter week 3
  "nearly there". The rule exists because "nearly there" is how the 70 %-done corpse
  happens.

### Week 3 — defensibility: SACK, flow control, classes, benchmarks
- S1: **[CORE]** SACK bitmap + fast retransmit; class 0/1/2 semantics on rx path.
- S2: **[CORE]** flow control + zero-window probe; sim scenarios: stalled receiver,
  probe-loss deadlock check (invariant 3, 5).
- S3: bench binaries + baselines + `run_matrix.sh`; first full latency-vs-loss curves
  incl. TCP and ENet; commit CSVs + plots.
- **Exit:** headline graph exists with all three lines + the two "we lose here"
  plots; `docs/BENCHMARKS.md` drafted; defense drill W3 passed (§10).

### Week 4 — SWIM (cut-first candidate)
- S1: **[CORE]** SWIM state machine on SimNet: ping / ping-req(k=3) / suspect /
  incarnation refutation; scenario: partition + heal reconverges.
- S2: piggyback plumbing + `mesh_node` demo; 5 nodes, live membership table.
- S3: chaos soak: netem + `iptables` partition mid-run; measure detect/reconverge
  times; invariant 6.
- **Exit:** 5-node demo survives a 30 s partition with correct final membership and
  no false-dead of reachable nodes. *(If week 3 slipped: skip SWIM entirely, spend
  week 4 finishing week 3 — transport + honest benchmarks is a complete project.)*

### Week 5 — ship it
- S1: profile (perf/flamegraph in the VM); apply only cheap wins — `recvmmsg`/
  `sendmmsg` batching is the canonical one; re-run benchmarks if anything changed.
- S2: README for a 90-second skim: thesis → diagram → 3 graphs → limitations
  (congestion control, security, single-thread) → build/run instructions.
  Stretch only if ahead: XOR-parity FEC (1 parity per 8 data packets, class 1) —
  recovers losses with zero retransmit RTT; one more curve if built.
- S3: final defense drill (§10 mock); fill the resume bullet metrics (§11); execute
  the resume-swap checklist (§11); tag v0.1.0.

---

## 10. Interview defense drills (Claude runs these; advancing is gated on them)

End of each week, in-session, no notes, Laksh answers out loud / in prose. Any miss →
review before new code. **Under the AI-implemented model (§2), these drills plus the
per-module gates are the ONLY thing that makes the resume claim defensible — treat a
failed drill exactly like a build break.** Question banks:

- **W1:** Why CRC32C over checksum/Adler32/xxHash — what does it catch and not catch?
  Walk your header byte-by-byte; why 1200 B; what breaks at MTU 1280? What did the
  fuzzer actually find (name a real crash you fixed)? Level- vs edge-triggered epoll?
- **W2:** Derive Jacobson's RTO from "mean + k·deviation". Why is a retransmit's RTT
  sample ambiguous (Karn)? Your ring's exact contents when acks 1-3,6-8 of 8 arrive —
  what retransmits and when? Why does your rto_floor=25 ms beat TCP, and why can't
  TCP do that on the internet?
- **W3:** Why does goodput collapse superlinearly with cumulative-only acks; what
  exactly did SACK+fast-retransmit change in your p99 curve (point at the graph)?
  Zero-window deadlock: walk the persist-timer state machine. Where does taut lose
  to TCP/ENet and mechanically why?
- **W4:** Why indirect probes (what network condition do they disambiguate)? What do
  incarnation numbers prevent — give the exact stale-rumor interleaving. What did
  pylon's 5 s reaper do wrong that suspicion fixes? Detection-time math for T=1 s,
  k=3, N=5.
- **W5 (mock):** 30 min: "You have a lossy 30 ms link and TCP's p99 is 400 ms — design
  a fix" (design taut from scratch on a whiteboard), then file-blind walkthroughs:
  Claude names any CORE file, Laksh explains its state and edge cases from memory.

---

## 11. Resume output

**Bullet (fill blanks from BENCHMARKS.md, keep one line if possible):**
> Built **taut**, a reliable-UDP transport library in C++20 for lossy small-message
> meshes — epoll event loop, sliding-window ARQ with SACK, adaptive RTO
> (Jacobson/Karn), per-message reliability classes, SWIM failure detection;
> libFuzzer-hardened codec, deterministic network-simulation tests, netem fault
> injection; **__×** lower p99 latency than kernel TCP at 5 % loss (at **__×**
> bandwidth overhead), within **__ %** of ENet throughput.

**Interview-honesty note:** if asked how it was built, "AI-assisted implementation —
the design decisions, verification strategy, and analysis are mine" is a perfectly
fine 2026 answer, *provided the §2 gates were genuinely passed*, because the very
next question will test exactly that.

**Resume-swap checklist (same day as v0.1.0):**
- [ ] taut in; **Winnow bullet out** (or reworded to "team hackathon — built the
      Next.js demo UI + voice capture").
- [ ] Vigil reframed: "(hackathon, team of 3) — built deployment pipeline, live
      incident SSE UI, sponsor integrations."
- [ ] Pylon reframed to what's yours: the signature-library-vs-detector evaluation
      module — or pointed at taut as its successor.
- [ ] agentbench: remove/fix the PyPI + CI badges that point at repos you don't own.
- [ ] LooseAPI: push the real code or unlink the empty repo.
- [ ] SitRep bullet: claim your actual surface ("~50 % of the API layer, password
      reset, migrations"), not the teammates' auth core/CI.

---

## 12. Commitment test (week 0 gate — on paper, no lookups; derivations count)

1. Eight packets in flight, window 8; receiver got 1-3 and 6-8. With cumulative-only
   acks, what does the sender know, what retransmits, and what changes with a SACK
   bitmap? What data structure tracks in-flight packets and deadlines?
2. Why is an RTT sample from a retransmitted packet poisoned (derive the ambiguity),
   what does using it do to RTO over time, and what's the fix?
3. Receiver's app stops draining: what stops the sender, where does that signal live
   in the header, and — when the reopening ack is lost — what prevents deadlock?
4. At 10 % loss your goodput drops far more than 10 % with cumulative acks + RTO-only
   recovery. Walk the stall anatomy; name the single change that recovers most of it.
5. In SWIM, why probe a suspect through k other members before declaring death; what
   do incarnation numbers prevent; what goes wrong with a bare 5 s timeout reaper
   when a healthy node's CPU is briefly pegged?

**Rule: fewer than 3 derivable now → build §13 instead.** (Score honestly; the point
of the gate is to protect five weeks of your life.)

## 13. Fallback project (only if §12 fails or the week-2 checkpoint is missed)

**Durable workflow engine in Java** ("Temporal-lite", composes with the Xyntopia
orchestration story): segmented write-ahead log with CRC-framed records and explicit
fsync discipline, snapshots, deterministic replay, idempotent side-effect API, and a
kill -9 crash-injection harness proving zero lost/duplicated steps. Same rigor
pattern as taut (WAL golden tests, crash-loop instead of netem, throughput vs a
SQLite-backed queue baseline). If activated, write its own PLAN.md on this template.
Secondary hedge (week-2 miss, want C++ anyway): Bitcask-style KV store — CRC-framed
append-only log + in-RAM hash index + compaction + kill -9 recovery, benchmarked
against LevelDB point ops. Two weeks, ships something real.

---

## 14. Risk register

| risk | mitigation |
|---|---|
| Briefings become passive scrolling; comprehension stays shallow | §2 gates are blocking: file-blind explain-backs, predictions before test runs, paper hand-traces, planted-bug hunts; a failed gate halts implementation |
| Claude generates whole subsystems in one dump, gates get batched and rushed | CLAUDE.md forbids bulk generation; one module per loop, small commits, gate before next module — even if Laksh asks to skip ahead |
| Timer/retransmit heisenbugs eat week 3 | SimNet (§6.3) makes every failure a seed; build it before the netem soak, not after |
| netem on loopback behaves oddly / offloads pollute TCP baseline | veth + netns recipe with tso/gso/gro off (§6.5) |
| macOS host has no epoll/netem | Lima/Docker Linux env from day one (§8); never develop the loop against kqueue "temporarily" |
| Benchmark accidentally strawmans TCP | TCP_NODELAY, offloads off, framing overhead counted, publish losing axes (§7) |
| Scope creep (crypto, congestion control, io_uring) | §1 non-goals; README "limitations" section is where those live |
| Week 4 SWIM overruns | cut-first rule (§9 W4); transport + benchmarks is a complete project |
| Life happens, a week vanishes | every week ends shippable; the week-2 checkpoint is the only hard gate |
