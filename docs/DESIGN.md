# taut design specification

The reference design for the protocol, the testing strategy that verifies it, and
the benchmark honesty contract. Section numbers are preserved because the `§5.x`
citations throughout `src/`, `include/` and `docs/` point at them.

Per-module rationale lives alongside this file in `docs/DESIGN-*.md`; the decision
record is `docs/DECISIONS.md`.

## 5. Design specification

Everything in §5 is a **reference design**: strong defaults, chosen so the time went
into the protocol rather than into bikeshedding. Where the implementation deviates,
the relevant `docs/DESIGN-*.md` says why.

### 5.1 Public API

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
0      2    magic          0x7A 0x75  ("zu", pick your own)
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
21     [8]  sack_bitmap    iff flags.bit0, bits = (cum_ack+1 .. cum_ack+64) rx'd
[+]    [n]  membership     iff flags.bit1, up to 3 × 12 B entries:
                           {addr4, port u16, incarnation u32, state u8, pad u8}
[+]    ...  payload        (payload_len bytes)
```

Decisions embedded here (know why for each - they get asked):
- **Single seq space per peer-pair**, ordering enforced only for class 2 at the
  receiver. Simpler than per-class spaces; document the duplicate-detection
  consequence for class 0.
- **Cumulative ack + 64-bit SACK bitmap** rather than SACK ranges: fixed size, O(1),
  covers a 64-packet window exactly.
- **CRC32C** (Castagnoli): hardware instruction on x86 (SSE4.2) and ARM; write the
  software fallback yourself, verify against known vectors, then enable the HW path.
- **Acks ride on everything**: every outgoing packet carries current cum_ack/window;
  pure ACK packets are sent when there's no reverse traffic (delayed up to 10 ms or
  every 2nd packet, whichever first - classic delayed-ack shape).
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
- **rto_floor default 25 ms** - deliberate deviation from TCP's ~200 ms Linux minimum.
  This single constant is a large part of why taut beats TCP's tail latency under
  loss; be able to explain why TCP *can't* safely do this on the open internet
  (spurious retransmit storms, fairness) and why a closed mesh can.
- Karn's rule: never sample RTT from a retransmitted packet.

### 5.6 Flow control

- `adv_window` = free slots in the receiver's reassembly/delivery buffer.
- Zero-window: sender stops; a **PROBE_WINDOW** packet goes out on a persist timer
  (start 100 ms, ×2 backoff, cap 1 s) so the window-reopening ack being lost cannot
  deadlock the connection. (Commitment-test Q3 - you'll implement your own answer.)

### 5.7 Event loop & threading

- **One thread. No locks in v1.** `epoll_wait` (level-triggered first - simpler to
  reason about; document the edge-triggered upgrade as future work) over: the UDP
  socket, a `timerfd` armed to the timer-heap's min deadline, and an `eventfd` for
  cross-thread sends if a demo needs one.
- Timer store: **hand-built binary min-heap** keyed by deadline (lazy deletion via
  generation counters). A timer wheel is the classic follow-up question - know the
  tradeoff, don't build it.
- Per-tick order: drain socket → process acks/data → fire due timers → flush pending
  acks → re-arm timerfd.

### 5.8 Explicit non-feature: congestion control

Fixed window, no Reno/CUBIC/BBR. Say so in the README, and be able to whiteboard
what would happen if two taut flows shared a bottleneck (they'd stomp each other and
everyone else) and roughly what adding AIMD would look like. "I know exactly what I
didn't build and why" is a stronger interview position than a half-built CUBIC.

### 5.9 SWIM membership

Per the SWIM paper (Das, Gupta, Motivala, 2002):
- Protocol period T = 1 s. Each period: pick one member round-robin from a shuffled
  list, direct PING (timeout 300 ms); on miss, PING_REQ via **k = 3** other members;
  on total miss → **suspect**.
- Suspect timeout 3 s → dead. Suspicion, alive, and dead updates are gossiped by
  **piggybacking** (§5.2 flags.bit1) with a per-update budget of ~3·log(N) sends.
- **Incarnation numbers**: only the accused node can refute suspicion, by re-announcing
  itself with incarnation+1. This is what prevents a flapping node from being killed
  by stale rumors - and it's what pylon's naive 5 s reaper got wrong.
- Failure-detection demo metric: time-to-detect and time-to-reconverge after an
  `iptables` partition heals, at N = 5 nodes.

---

## 6. Testing & verification strategy

### 6.1 Unit + golden tests
Every CORE module gets plain unit tests as it's built. The codec additionally gets
**golden vectors**: hand-computed byte arrays committed in `tests/golden/` (encode →
exact bytes; decode → exact struct; bit-flip → CRC reject). Write the first vectors
by hand on paper - that exercise is where the format becomes yours.

### 6.2 Fuzzing (libFuzzer + ASan/UBSan)
- `fuzz_decode`: raw bytes → decoder. Must never crash/overflow/UB; malformed input
  returns an error. Trick: the CRC check rejects most random input before it reaches
  interesting code - add a build flag where the fuzzer computes/patches a valid CRC
  (or a keyed-CRC bypass) so coverage reaches the parser guts. Mention this in the
  README; it's a sophisticated touch.
- `fuzz_session`: fuzzer bytes interpreted as a *script* of events (deliver packet /
  advance time / app-send) driven into a connection pair over the sim transport.
  Asserts internal invariants (§6.4) after every step. This is protocol-state fuzzing,
  not just parser fuzzing - rare in student projects, cheap to build on top of §6.3.
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
won't kill this one - and it's a legitimately impressive artifact of its own.

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

**Baselines - configured fairly or the whole exercise is a strawman:**
- **Kernel TCP** with `TCP_NODELAY` (never benchmark latency against Nagle),
  length-prefixed message framing, offloads disabled on the veth (above).
- **ENet** (vendored via CMake FetchContent), reliable channel, same workload.

**Headline experiment:** p99/p999 message latency vs loss rate (0→20 %) at RTT 30 ms,
three lines: TCP, taut class 2, taut class 1. Expected shape: TCP's tail explodes
past ~200 ms (RTO_min + head-of-line); taut class 2 bounded near RTT + rto_floor;
class 1 lowest. **Also published, same page:** the prices paid  - 
bandwidth overhead ratio (bytes-on-wire / goodput bytes) vs TCP, and clean-link
(0 % loss) throughput where TCP and ENet should beat taut. If taut wins everything,
the benchmark is broken - find the bug in the harness, not the victory lap.

**Report:** `docs/BENCHMARKS.md` - method, exact commands, hardware, plots, and a
"why we lose where we lose" analysis (that analysis section is worth more in an
interview than the wins).

---
