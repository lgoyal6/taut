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

> **Status: v0.1.2.** The library is complete and **powers
> [tautq](https://github.com/lgoyal6/tautq)**, a 5-node distributed webhook-delivery
> service whose replication, failover, and membership plane all ride taut — its chaos
> suite found the v0.1.1 (SWIM rejoin) and v0.1.2 (post-Dead partition-heal) protocol
> fixes here. See [`docs/PROGRESS.md`](docs/PROGRESS.md) for the full history and
> per-module design notes in [`docs/`](docs/).

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

Request–reply p99 latency vs loss (RTT 30 ms, 512 B messages, median of 5 runs). TCP's
tail explodes past its ~200 ms `RTO_min` under loss while taut recovers on its 25 ms floor:

![p99 request-reply latency vs loss](bench/data/latency_vs_loss.png)

| loss % | taut c1 | taut c2 | kernel TCP | ENet |
|---|---|---|---|---|
| 5  | 62 ms  | 92 ms  | 409 ms  | 148 ms  |
| 20 | 281 ms | 278 ms | 3363 ms | 2997 ms |

The price, same fixture: at 0 % loss kernel TCP saturates at ~235 Mbit/s vs taut's ~8.7 —
TCP wins bulk throughput by ~27× (taut sends one datagram at a time, no batching):

![clean-link throughput](bench/data/throughput_cleanlink.png)

taut's other price is bandwidth: under sustained load its bytes-on-wire climb from ~1.13× to
~1.27× (at 10 % loss) as it retransmits, vs TCP's ~1.06–1.17× — ~15–20 % more wire bytes for
the tail-latency win (`bench/data/overhead_vs_loss.png`).

Real baselines (`TCP_NODELAY`, ENet), fixed seeds, raw CSVs committed; full method, tables,
and the "why we lose where we lose" analysis are in [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md).
(In this one-outstanding RR workload class 1 ≈ class 2; class 1's head-of-line-blocking win
needs a pipelined load with several messages in flight.)

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
