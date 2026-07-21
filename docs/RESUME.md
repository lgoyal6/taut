# Resume output (PLAN §11)

Draft — reword in your own voice before it goes on the resume. Every number here traces to a
committed CSV in `bench/data/` or the soak log; don't inflate them.

## Bullet (metrics filled from `bench/data/summary_*.csv`)

> **taut** — reliable-UDP transport library in C++20 for lossy small-message meshes.
> Single-threaded epoll event loop; sliding-window ARQ with a 64-bit SACK bitmap + Reno-style
> fast retransmit; adaptive RTO (Jacobson/Karn, RFC 6298) with a 25 ms floor; three per-message
> reliability classes; SWIM failure detection with incarnation-number refutation. Hardened with
> a libFuzzer codec fuzzer (ASan/UBSan-clean), a deterministic seeded network simulator for
> reproducible protocol tests, and a veth+netem soak (20/20 green 10 MB transfers, sha256-identical,
> at 5 % loss). **~6–12× lower p99 message latency than kernel TCP (`TCP_NODELAY`) at 5–20 % loss**
> — 25 ms RTO floor vs TCP's ~200 ms — at **~1.2× bandwidth overhead**, a deliberate
> tail-latency-for-throughput trade (no congestion control, by design).

Tighter one-liner if space is short:

> Built **taut**, a C++20 reliable-UDP transport (epoll, SACK ARQ, Jacobson/Karn RTO, reliability
> classes, SWIM) — fuzzed, netem-soaked, and benchmarked against kernel TCP + ENet; **~6–12× lower
> p99 latency than TCP under 5–20 % loss** at ~1.2× bandwidth overhead.

## Honest interview framing
- If asked how it was built: "AI-assisted implementation — the design decisions, verification
  strategy, and analysis are mine." (True only to the extent you can defend it — see the gap below.)
- **Where it loses, say so first:** bulk throughput. Kernel TCP does ~235 Mbit/s clean-link vs
  taut's ~8.7 (one datagram at a time, no `sendmmsg`/`recvmmsg` batching); no congestion control.
  This is the deliberate trade, and owning it is stronger than hiding it.

## Numbers (from committed CSVs — do not round up)
- p99 RR latency @ 5 % loss: taut c1 62 ms / c2 92 ms vs TCP 409 ms vs ENet 148 ms.
- p99 RR latency @ 20 % loss: taut ~280 ms vs TCP 3363 ms vs ENet 2997 ms.
- Bandwidth overhead (sustained load): taut ~1.13→1.27× (0→10 % loss) vs TCP ~1.06–1.17×.
- Clean-link throughput: TCP ~235 Mbit/s, ENet ~16, taut ~8.7.
- Week-2 soak: 20/20 consecutive 10 MB transfers sha256-identical at 5 % loss.

## Resume-swap checklist (PLAN §11 — do on your actual resume, same day as v0.1.0)
- [ ] taut in; **Winnow bullet out** (or reworded to "team hackathon — Next.js demo UI + voice capture").
- [ ] Vigil reframed as a team project (deployment pipeline, incident SSE UI, sponsor integrations).
- [ ] Pylon reframed to your actual surface, or pointed at taut as its successor.
- [ ] agentbench: remove PyPI/CI badges pointing at repos you don't own.
- [ ] LooseAPI: push the real code or unlink the empty repo.
- [ ] SitRep: claim your actual ~50 % API surface, not the teammates' auth core/CI.

## The defensibility gap (be honest with yourself)
The weeks 2–5 comprehension gates and design-decision rationales were dropped (full-drop mode).
The code is real and the numbers are real, but **an interviewer will probe the design** — the 25
blank `Rationale (Laksh)` lines in `docs/DECISIONS.md` and a walkthrough of `session.cc`/`swim.cc`
from memory are the homework before you put this on a resume and defend it cold.
