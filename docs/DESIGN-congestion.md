# Bounded congestion and comparison contract

The default `Config::congestion` is `FixedWindow`, preserving the original window policy.
`PacedAimd` is an opt-in local experiment. Delivery, ACK generation, SACK recovery, receive
flow control and congestion state have separate responsibilities. The wire format is unchanged.

The controller counts reliable datagrams in packet units because each message fits one MTU.
Its initial window is `min(10, window_pkts)`. Slow start grows by one packet per newly
acknowledged datagram up to the configured maximum. After a loss, the threshold and window
halve, with a floor of one; congestion avoidance then adds one packet per window of new ACKs.
A recovery boundary suppresses another decrease for sequence numbers in the same flight.
A SACKed packet is credited once, never again at cumulative removal. RTT samples retain
Karn's rule. New data is paced at smoothed RTT divided by congestion window, without idle
burst credit. Recovery retransmits and control packets retain the existing timer path.
This is a bounded AIMD controller, not a complete TCP Reno implementation or an Internet-ready
transport. Reliable data sequence wrap and same-endpoint restart epochs are unsupported.

`send` backpressures at the bounded pending queue and rejects payloads above either the
configured application MTU or the codec ceiling. `poll` ignores datagrams from other endpoints.
An endpoint check is not cryptographic authentication. Reconnection requires fresh endpoints;
reusing the same endpoint can admit stale datagrams because there is no connection epoch.
The regression suite deliberately retains that adverse control.

## Comparison workload

`paced_bench` uses one native event loop for kernel TCP with `TCP_NODELAY` and Taut ordered
reliability. `quic_baseline.py --mode paced` uses the pinned real aioquic implementation,
a reliable stream, certificate verification, and the same framing and offer rules.
All arms use 512-byte payloads, at most 64 outstanding requests, and byte-exact echo checking.

Paced mode offers sequence `n` at `t0 + n * floor(1e9/rate)` nanoseconds. An offer is rejected
if an interval has already elapsed, its bounded queue is full, or admission fails. It is
never silently deferred into a catch-up burst. Every scheduled offer is counted. Latency
starts at the intended offer time and is conditional on completion; rejected and missing
work must be reported alongside it. Offers, admission decisions, observation times and
completion times are emitted to separate raw CSVs. Successful exit requires every admitted
request to complete correctly before the bounded drain cap. Rejection is an admission
outcome, not a delivery success.

Bulk mode continuously fills the same bounded pipeline during the measurement window.
Goodput counts request payload bytes whose echoes complete before the common window ends.
The reverse echo consumes bandwidth too. This is a specified pipelined request workload,
not a unidirectional stream capacity estimate.

Two competing clients share one endpoint egress qdisc, a common monotonic start and a
common measurement interval. Their servers share the reverse bottleneck. Jain fairness is
computed from each flow's completed payload bytes per measurement second. Separate per-flow
qdiscs would not establish a competing-flow comparison. Low aggregate goodput and queue
rejections remain visible even when the fairness index is high.

## Running and operating locally

Build `paced_bench` with `TAUT_BUILD_BENCH=ON`. Keep raw data outside the checkout. The
runner requires an explicit frozen JSON protocol, native Linux binary and new output directory:

```bash
python3 bench/scripts/run_paced_study.py --protocol "$TAUT_PROTOCOL" \
  --binary "$TAUT_LINUX_BINARY" --output "$TAUT_RESULTS"
python3 bench/scripts/analyze_paced_study.py "$TAUT_RESULTS" --out "$TAUT_ANALYSIS"
```

The protocol `cases` entries name `id`, `group`, `run`, `arms`, `rate`, `duration_s`,
`loss`, `rtt_ms` and `bandwidth`. Arms are `tcp`, `taut-fixed`, `taut-paced`, and `quic`.
A two-arm case runs concurrently on a shared bottleneck. Optional `mode: bulk`,
`reorder: true`, and `rtt_change_ms` select explicit interventions. `loss: burst` selects
the documented Gilbert-Elliott parameters in the runner. A new experiment identity is
required after changing a frozen protocol or source.

The runner creates uniquely named internal Docker network namespaces and two bounded
containers, applies symmetric egress netem, and disables TSO/GSO/GRO. It uses local
NET_ADMIN in those containers, not host networking or privileged mode. No external account
is used. The caller must serialize sustained measurements with other benchmarks.

Startup health is the listener/client readiness line and the authenticated QUIC handshake.
Run identifiers, flow indices and unique resource names correlate commands, rows and logs.
The command log and per-case exit status expose failures. JSON identity records binary,
protocol and source hashes, kernel, aioquic and TCP settings. Wire counters include setup,
drain and teardown and are aggregate when two flows compete. Process CPU includes setup.
A stalled run has a bounded drain and subprocess timeout. Failure never becomes a clean
latency row. Rerun failures under a new result directory without replacing the first receipt.

The legacy `run_matrix.sh` and `run_quic_repeat.sh` require a new `DATA_DIR` outside
the checkout and refuse an existing directory. This prevents an accidental rerun from
overwriting retained receipts. Their Docker cleanup is registered before resource creation.
Legacy netns execution requires an explicit authorized root invocation; it does not escalate.

For rollback, select the default `FixedWindow` policy or run the retained original baseline.
No migration or deployment is performed. The runner removes only its own containers,
network and temporary certificate material in `finally`. Raw inputs, command logs, outputs
and unsuccessful controls remain in the caller's private results directory for review.
A force-killed runner may leave its uniquely named resources; inspect those exact names
before cleanup. Do not prune other containers or shared toolchain caches.

## Retransmission accounting supplement

The native client can report retransmission bytes after a trial. Taut counts UDP payload
bytes accepted by retransmission sends, including its packet header. Linux TCP uses
`TCP_INFO.tcpi_bytes_retrans`, which counts retransmitted TCP stream bytes. An unavailable
kernel field is reported explicitly. Server logs report the same direction-specific counters.

`quic_baseline.py --qlog PATH` enables aioquic's native event logger. The
`retransmission_bytes.py` tool unions sent STREAM offset ranges per stream and connection;
overlapping ranges count retransmitted stream bytes. These include application framing,
but exclude QUIC packet headers and crypto/control data. Qlog capture adds overhead and is
used in a separately frozen accounting supplement, not retroactively added to earlier
performance trials. Total interface-byte measurements remain separate from all three
transport-specific counters.
