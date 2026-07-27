# DESIGN — SWIM membership (§5.9)

Failure detection + membership for the mesh, per the SWIM paper (Das, Gupta, Motivala, 2002).
One `Swim` instance per node over a `UdpTransport`; single-threaded, driven by `poll()`
(inbound) and `tick()` (the time-based state machine off `tx_.now()`) — the same shape as
`Session`, so the epoll loop and the deterministic SimNet harness both just call those.

## What it does

- **Protocol period `T = 1 s`.** Each period pick one target by *randomized round-robin*
  (walk a shuffled member list; reshuffle on wrap — guarantees every live member is probed
  once per `N-1` periods with no fixed skip pattern) and send a direct **PING** (`probe_id`).
- **Direct timeout `300 ms`.** No PONG by then → send **PING_REQ** to `k = 3` other members;
  each relays a PING to the target and forwards the target's PONG back to us.
- **End of period, still unacked → SUSPECT.** Suspicion is gossiped.
- **Suspect timeout `3 s` → DEAD.** Dead is gossiped and is terminal (basic SWIM: no rejoin).
- **Incarnation numbers / refutation.** Every membership record carries an incarnation. Only
  the *accused* node refutes a suspicion about itself, by re-announcing `Alive` at
  `incarnation + 1`. This is exactly what pylon's naive 5 s reaper lacked.

## Data structures (`swim.h`)

- `members_ : key → Member{addr, state, incarnation, suspect_deadline}` — the membership
  table, excluding self. Key = `(addr_be << 16) | port_be` (same packing SimNet uses).
- `rumors_ : key → Rumor{state, incarnation, tx_count}` — the dissemination buffer, **one
  entry per subject**, overwritten (budget reset) whenever a newer update for that subject is
  adopted. Gossiped least-transmitted-first, dropped once `tx_count ≥ budget`.
- `relays_ : relay_probe_id → PendingRelay{requester, req_probe, target, deadline}` — in-flight
  indirect probes we are relaying on someone else's behalf.
- `probe_order_ / probe_index_` — the shuffled round-robin cursor.
- Per-period scalars: `probe_target_`, `probe_id_`, `ping_deadline_`, `period_deadline_`,
  `next_period_`, and the `probing_ / indirect_sent_ / probe_acked_` flags.

## The state-merge rule (the correctness core)

`apply_rumor(subject, state, inc)` is the single entry point for every state change — wire
gossip, our own conclusions, and tests all go through it. `overrides()` implements the SWIM
§4.2 precedence:

| incoming ↓ vs local → | Alive(j) | Suspect(j) | Dead(j) |
|---|---|---|---|
| **Alive(i)**  | i > j | i > j | never |
| **Suspect(i)**| i ≥ j | i > j | never |
| **Dead(i)**   | always | always | never |

Two rules carry all the weight:
1. **Suspect beats Alive at equal incarnation** — a suspicion sticks until the accused itself
   produces a *higher* incarnation. A plain `Alive` at the same incarnation cannot clear it.
2. **Only the accused can produce that higher incarnation** (`refute()` bumps
   `my_incarnation_` past the suspected value). Hence a live node that can still hear gossip
   always out-runs a suspicion before the 3 s timer — **invariant 6**.

`Dead` is terminal (overrides any non-dead, at any incarnation; nothing overrides it).

### What incarnation numbers prevent (the stale-rumor interleaving)

A node `Q` briefly can't reach `P` (a lost ping, a CPU spike) and gossips `Suspect(P, i)`. `P`
is alive: it hears the suspicion, bumps to `i+1`, and floods `Alive(P, i+1)`, which overrides
`Suspect(P, i)` everywhere (`i+1 > i`). The *original* `Suspect(P, i)` is still floating around
and arrives at some node **after** it already holds `Alive(P, i+1)` — and is silently dropped,
because `i ≯ i+1`. Without incarnations that stale rumor would re-kill a healthy node; with
them it is inert. `tests/unit/swim_test.cc::IncarnationNumbersRejectStaleSuspicion` reproduces
exactly this interleaving (and the contrast: a suspicion at the *current* incarnation does
apply, which is why refutation must always bump).

## Gossip / dissemination

Every SWIM packet (PING/PING_REQ/PONG/JOIN) piggybacks a batch of pending rumors, chosen
least-transmitted-first. Per-rumor **send budget** `b = ceil(gossip_factor · ln N)` with
`gossip_factor = 3` — for `N = 5`, `b = ceil(3 · ln 5) = ceil(4.83) = 5`. This is the classic
infection-style dissemination: each update rides ~`b` outgoing packets, giving whp delivery in
`O(log N)` rounds while bounding per-update traffic.

## Detection-time math (T = 1 s, k = 3, N = 5)

Let a node crash at `t = 0`. Detection has three stages.

**1. Time to first probe of the dead node.** With `N-1 = 4` independent detectors, each
probing one of its 4 peers per period, the dead node is targeted by *some* detector with
per-period probability `1 − (3/4)^4 ≈ 0.684`, so
`E[periods to first probe] ≈ 1/0.684 ≈ 1.46` periods. Randomized round-robin bounds the worst
case at `N-1 = 4` periods (a single detector is guaranteed to reach it within one shuffle).

**2. Probe → SUSPECT.** A probe that starts at a period boundary fails direct (`300 ms`),
fails all `k = 3` indirect relays, and is concluded SUSPECT at that period's end — i.e. within
`T = 1 s` of being targeted. So
`E[t_suspect] ≈ E[periods to probe] · T ≈ 1.5 s`; worst case `≈ (N-1)·T = 4 s`.
(The demo measures **~1.0 s** at its seed — one period.)

**3. SUSPECT → DEAD.** Fixed `suspicion_timeout = 3 s`.

`E[t_detect] = E[t_suspect] + 3 s ≈ 4.5 s`; worst case `≈ 4 s + 3 s = 7 s`. Dissemination of
the verdict adds `O(log N) ≈ 2–3` periods to reach all survivors.

**Why `k = 3` indirect probes (they change accuracy, not latency).** Indirect probing does
*not* speed detection — it suppresses false positives. A single dropped direct packet or a
momentarily busy target would falsely suspect a healthy node; requiring the direct probe *and*
`k` independent relay paths to all fail drives the false-suspicion probability to roughly
`p_fail^(1+k)`, disambiguating "the target is dead" from "my one path to it was unlucky." `k`
buys accuracy for a few extra messages per failed period. Its cost/benefit is why `k` is a
tunable, not a constant.

**Detection ordering caveat (a real property, surfaced by testing).** For the *reconvergence*
test to deterministically see SUSPECT before any DEAD, detection (worst case `≈ (N-1)·T`) must
finish before the earliest possible DEAD verdict (`≈ T + suspicion_timeout`). That needs
`suspicion_timeout > (N-2)·T`. At `N=5, T=1 s` the spec's 3 s sits exactly on that boundary, so
the reconvergence unit test compresses the clock (period 500 ms, suspicion 4 s) to make the
ordering robust across seeds; the mechanism it exercises is identical to the T=1 s demo.

## Deviations from the reference design (each earns its sentence, per §5)

- **Gossip rides in the packet *payload*, not the §5.2 header piggyback section.** Lane
  constraint for the parallel-worktree split (no codec/flags edits yet); the header-piggyback
  integration is a later merge step. `PacketType::{Ping,PingReq,Pong,Join}` + `Class::Unreliable`
  are used as-is; sender identity comes from the transport's `from`, never the payload.
- **Unresolved `Suspect` rumors are re-injected every period** (anti-entropy), beyond a strict
  single-shot reading of the paper's dissemination. Without it, a rumor's `~3·log N` send
  budget is spent broadcasting into a partition, so after the partition heals the accused never
  hears the suspicion and never refutes — membership would not reconverge. Re-injection keeps
  every open suspicion advertised until it is refuted or confirmed dead.
- **Basic SWIM only.** No Lifeguard/SWIM+ refinements (suspicion-count-weighted timeouts, local
  health multiplier, dogpile). The incarnation machinery is the part that matters for the
  interview story. (v0.1.0 also made Dead terminal; v0.1.1 replaces that — see "Rejoin" below.)

## Rejoin (v0.1.1)

Building tautq (a service that must survive kill-and-restart) exposed that v0.1.0's "Dead is
terminal at any incarnation" rule makes a restarted node permanently unable to rejoin: its
refutation `Alive@k+1` could never outrank `Dead@k`, so the cluster ignored it forever. Two
changes, both protocol-level:

- **Precedence is now lexicographic on `(incarnation, state)`** with `Alive < Suspect < Dead`
  (the memberlist/Lifeguard ordering). Within one incarnation, evidence only accumulates toward
  death — Suspect beats Alive, Dead beats both, Dead is unbeatable. A *strictly newer*
  incarnation beats anything older, **including Dead**: only the subject itself can mint a
  higher incarnation, so `Alive@k+1` is first-hand proof of life issued after the `Dead@k`
  verdict's evidence. Death verdicts are declared at the member's then-current incarnation, so
  a genuinely dead node can never outrank its own death. This also fixes a v0.1.0 wart where a
  stale `Dead@old` could kill a member already refuted at a higher incarnation.
- **Rejoin flow needs no new packets:** the restarted node (fresh state, incarnation 0) sends
  JOIN to any introducer. Its `Alive@0` does not beat `Dead@k`, but the full-snapshot reply
  carries the cluster's `Dead@k` belief about it; the ordinary self-refutation path re-announces
  it at `k+1`, which now wins and gossips out. The cluster converges back to Alive with no
  resurrection special case anywhere.
- **JOIN request/reply are distinguished by the subject field** (a request names its sender,
  a reply names the joiner). v0.1.0 answered every JOIN with a JOIN reply — including replies —
  so any join exchange degenerated into an **infinite JOIN ping-pong** (state converged, so
  only counting packets catches it; `JoinExchangeTerminates` now does). `handle_join` also now
  merges the joiner through `apply_rumor` instead of unconditionally adopting/gossiping Alive,
  which previously let the introducer gossip `Alive` while its own table said Dead.

## Post-Dead refutation channel (v0.1.2)

Caught LIVE by tautq's chaos partition scenario (permanently stalled jobs, both sides of a
healed partition holding Dead verdicts forever): once Dead verdicts land, the base protocol
goes silent across the healed link — Dead members are never probed (`pick_target` skips
them), and the accusation's gossip budget was spent INTO the partition, so even incidental
contact would not tell the accused to refute. Rejoin (v0.1.1) doesn't help: nobody
restarted, so nobody JOINs.

Fix, two small mechanics:
- **Dead-probing:** each protocol period, direct-PING one random Dead member with its own
  Dead rumor re-queued (fresh budget) so the accusation rides along. A genuinely dead node
  ignores it (one wasted datagram per period); a live one refutes at `inc+1`, resurrects
  under the v0.1.1 ordering, and normal gossip re-merges the halves.
- **Contact-from-dead:** any SWIM packet arriving from a member we believe Dead re-queues
  its Dead rumor, covering asymmetric heals where only one side still probes.

Verified by `SymmetricPartitionHealsAfterDeadVerdicts` (3 seeds): split {3,4}|{0,1,2} for
20 s virtual (Dead confirmed both ways), heal, reconverges to all-Alive.
- **Partitions are modeled by a `LinkFilter` transport decorator** in the test/demo, not by
  editing the shared `SimNet` (owned by feat/core). Symmetric blocking on both endpoints ==
  a bidirectional partition.

## Verification (all on seeded SimNet, `tests/unit/swim_test.cc`)

- `IncarnationNumbersRejectStaleSuspicion`, `SelfRefutesByBumpingIncarnation`,
  `DeadStickyWithinIncarnationNewerAliveResurrects` — the precedence rules, including the
  exact stale-rumor interleaving above and the v0.1.1 rejoin ordering.
- `JoinExchangeTerminates` — a join is exactly 1 request + 1 reply (counted on the wire);
  regression for the v0.1.0 infinite JOIN ping-pong.
- `RestartedNodeRejoins` (3 seeds) — kill a node, confirm Dead cluster-wide, restart it on the
  same endpoint with fresh state; it rejoins via JOIN → refute@k+1 and the cluster reconverges
  to all-Alive.
- `DetectsCrashedNode` — crash → SUSPECT → DEAD, verdict gossiped to every survivor; asserts
  SUSPECT precedes DEAD by ~`suspicion_timeout`.
- `PartitionHealReconverges` (4 seeds) — isolate a node, detect it, heal; membership
  reconverges to all-Alive and the node is **never** confirmed DEAD (invariant 6 under
  partition).
- `LiveReachableNodeNeverConfirmedDead` (5 seeds, 10 % loss, 30 s virtual) — **invariant 6**:
  with refutations flowing, transient suspicions occur but no reachable node is ever confirmed
  dead.
- `JoinLearnsRoster` — a newcomer knowing only an introducer learns the whole roster (JOIN
  full-snapshot reply) and the cluster learns it (gossip).

The `mesh_node` demo runs the 5-node story end-to-end on the virtual clock and prints the live
membership table plus measured time-to-detect / time-to-reconverge.
