# DESIGN — SimNet (deterministic simulation transport)

The in-process `UdpTransport` used by protocol tests and `fuzz_session` (§6.3). Its whole
reason for existing: make timer/retransmit heisenbugs reproducible. Same seed + same
operations => byte-identical run, so every failure repros from `--seed N`.

## Model
- `SimNet` owns a seeded `std::mt19937_64`, an `Impairments` config, and a **virtual clock**
  (`now_`). Time only moves when the test calls `advance()`.
- `SimNet::endpoint(addr)` hands out `SimEndpoint`s (each a `UdpTransport`). `fd()` is -1:
  sim endpoints aren't epoll-pollable — sim tests pump the protocol manually rather than
  running the real event loop.
- `send` → `deliver`: draw loss, then (if not dropped) enqueue a copy into the destination
  inbox at `now + delay(+jitter)`; draw dup, and if hit, enqueue a second copy with its own
  jitter (which is where reordering comes from).
- `recv` → `receive`: return the earliest **due** datagram (min `deliver_at`, `seqno`
  tiebreak), or nullopt if none has reached its delivery time yet.

## Determinism discipline
RNG draws happen in a fixed order (loss, then delay-jitter, then dup, then dup's jitter),
drawn unconditionally where outcome-independent, so the RNG stream doesn't depend on
branch outcomes. Determinism is guaranteed *within a build* (a given libstdc++/libc++), which
is what "repro from --seed on this build" needs; cross-implementation stability is not
claimed.

## Not modeled (deviations)
- No bandwidth/queue limits or congestion — out of scope (§5.8).
- Reorder is emergent from delay jitter rather than an explicit reorder-distance knob;
  equivalent for our invariant tests and simpler.
