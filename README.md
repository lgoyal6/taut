# taut

A purpose-built reliable-UDP transport for small-message meshes on lossy networks:
sliding-window ARQ, adaptive RTO (Jacobson/Karn), per-message reliability classes, and
SWIM failure detection — fuzz-hardened, fault-injected with `netem`, and benchmarked
honestly against kernel TCP (`TCP_NODELAY`) and ENet.

## Thesis

General-purpose transports carry obligations a small-telemetry mesh can drop. TCP must
deliver bytes strictly in order (head-of-line blocking), must be fair (congestion
control), and won't retransmit faster than a ~200 ms minimum RTO. taut serves one niche —
small messages on lossy links — and deliberately trades **bandwidth for tail latency**:

- a **25 ms RTO floor** instead of TCP's ~200 ms Linux minimum (safe on a closed mesh,
  unsafe on the open internet — that distinction is the whole point);
- **per-message reliability classes**, so a retransmitted packet doesn't have to block the
  messages queued behind it (class 1);
- a fixed window and no congestion control, on purpose (see *Limitations*).

The expected result is **much lower p99 message latency than TCP at 5–10 % loss, at a
measured cost in bandwidth overhead and clean-link throughput.** Both sides of that trade
get plotted — a graph where taut wins at everything would mean the benchmark is broken.

> **Status: in active development** (5-week build). See [`PLAN.md`](PLAN.md) for the full
> scope and [`docs/PROGRESS.md`](docs/PROGRESS.md) for exactly what is implemented today
> vs. planned. Per-module design notes live in [`docs/`](docs/).

## Architecture

Single-threaded by design: one event loop, no locks (§5.7). All protocol logic talks to a
`UdpTransport` interface, which has two implementations — real sockets, and an in-process
deterministic simulator (`SimNet`) used by the tests and the protocol-state fuzzer.

```
        application
   send(class, bytes) │ ▲ on_message(class, bytes)
                      ▼ │
   ┌──────────────────────────────────────────────┐
   │  Session  (per peer)                          │
   │   send ring · cumulative acks · RTO retransmit│   reliability core
   │   RttEstimator (RFC 6298 + Karn)              │   (§5.4–§5.6)
   │   reassembly buffer (class 2 in-order)        │
   └──────────────────────────────────────────────┘
                      │ Packet
                      ▼
   ┌──────────────────────────────────────────────┐
   │  codec   header + CRC32C, little-endian wire  │   §5.2
   └──────────────────────────────────────────────┘
                      │ datagram bytes
                      ▼
   ┌──────────────────────────────────────────────┐
   │  UdpTransport   send / recv / now / fd        │
   │   ├── RealUdpTransport  (nonblocking UDP)      │
   │   └── SimNet            (seeded, virtual clock)│   §6.3
   └──────────────────────────────────────────────┘
                      │ fd
                      ▼
   ┌──────────────────────────────────────────────┐
   │  EventLoop   level-triggered epoll (§5.7)     │   Linux
   └──────────────────────────────────────────────┘
```

**Reliability classes** (§5.3) — the semantic win over TCP:

| class | guarantee | retransmit? | receive behavior |
|---|---|---|---|
| 0 Unreliable | may drop / reorder, no dup | no | dedup window, deliver immediately |
| 1 ReliableUnordered | exactly-once, any order | yes | deliver on arrival (the latency star) |
| 2 ReliableOrdered | exactly-once, in send order | yes | reassembly buffer, deliver in seq order |

## Build

Requires clang 17+, CMake ≥ 3.24, and Ninja. `epoll` and `netem` are Linux-only, so on
macOS develop inside a Linux VM (Lima — see [PLAN §8](PLAN.md)).

```bash
cmake --preset dev            # Debug + ASan/UBSan
cmake --build --preset dev
ctest --preset dev
cmake --preset release        # Release build
cmake --build --preset release
```

## Run: file-transfer reliability demo

`send_file` / `recv_file` move a file over class 2 (ReliableOrdered) and print the sha256
at both ends. The file is chunked into messages, streamed through one `Session`, and
reassembled in order on the receiver.

```bash
# receiver (bind its own address, tell it the sender's address to ack to)
recv_file --bind 127.0.0.1:7001 --peer 127.0.0.1:7000 --out /tmp/out.bin
# sender
send_file --bind 127.0.0.1:7000 --peer 127.0.0.1:7001 --in /path/to/file
```

## Run: netem soak + the Week-2 checkpoint

The soak runs the transfer across a `veth` pair spanning two network namespaces, with
`netem` impairment on both directions and segmentation offloads disabled (the §6.5 recipe).
The hard checkpoint is **20 consecutive 10 MB transfers, sha256-identical, at 5 % loss**.

```bash
# bring the fixture up / tear it down manually (root needed for netns + tc)
sudo bench/scripts/netns_setup.sh up        # loss 5% delay 30ms±10ms reorder 1% dup 0.5%
sudo bench/scripts/netns_setup.sh status
sudo bench/scripts/netns_setup.sh down

# the checkpoint (20× 10 MB at 5 % loss) + the {0,1,5,10,20}% loss sweep
sudo bench/scripts/soak.sh                   # uses build/release/demo by default
```

## Testing

- **Unit + golden vectors** — per-module tests; the codec's wire format is pinned to a
  hand-computed golden byte vector (`ctest --preset dev`).
- **Deterministic simulation** — protocol tests run over `SimNet` with a seeded RNG and a
  virtual clock: same seed → byte-identical run, so every failure reproduces (§6.3).
- **Fuzzing** — `fuzz_decode` drives random bytes into the decoder under ASan/UBSan; a
  build flag patches a valid CRC so coverage reaches the parser past the checksum guard.
  ```bash
  ./fuzz/run_fuzz.sh decode 60      # smoke; 3600 for a long run
  ```

## Benchmarks

> _Graphs forthcoming — the benchmark harness and its report live in
> [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md) (see PLAN §7)._

The headline experiment plots p99/p999 message latency vs. loss rate for three lines —
kernel TCP (`TCP_NODELAY`), taut class 2, taut class 1 — alongside the prices paid:
bandwidth-overhead ratio and clean-link throughput, where TCP and ENet are expected to
win. Real baselines, fixed seeds, raw CSVs committed.

<!-- feat/bench: drop the latency-vs-loss and overhead plots here -->
<!-- ![p99 latency vs loss](docs/img/p99_vs_loss.png) -->
<!-- ![bandwidth overhead vs TCP](docs/img/overhead.png) -->

The `netem` soak already shows the mechanism the benchmark will quantify: with
cumulative-ack + RTO-only recovery (pre-SACK), goodput falls off superlinearly as loss
rises — which is precisely what SACK + fast retransmit (Week 3) exist to fix.

## Limitations (deliberate — see PLAN §1 non-goals)

- **No congestion control.** Fixed window, no Reno/CUBIC/BBR. Two taut flows sharing a
  bottleneck would stomp each other and everyone else. This is a closed-mesh transport;
  adding AIMD is understood but out of scope (§5.8).
- **No security.** No encryption, no authentication, no anti-amplification. The optional
  keyed-CRC flag is integrity-only, not a MAC. Do not expose taut to an untrusted network.
- **Single-threaded, single process per node.** One event loop, no locks. Throughput is
  bounded by one core; the design leans on that to stay simple and correct.
- **IPv4 only**, minimal 2-way handshake, no `TIME_WAIT` — connection restart is ambiguous
  by design; documented rather than solved.

## License

MIT — see [`LICENSE`](LICENSE).
