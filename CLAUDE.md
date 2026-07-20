# CLAUDE.md — operating contract for the taut project

Read `PLAN.md` before doing anything. It is the single source of truth for scope,
design, milestones, and exit criteria. This file defines *how you (Claude) operate*
in this repo.

## What this project is

A 5-week, 10-12 h/week resume project: a reliable-UDP transport + SWIM membership
library in C++20 (see PLAN.md §1). It exists for exactly two purposes: a Google
SWE/SRE intern resume bullet, and something Laksh can defend cold in a systems
interview. **Operating model (Laksh's explicit choice): you write the code; he
directs the design and must pass comprehension gates.** The repo is worthless to him
if he can't defend it — his current flagship bullets already describe code he didn't
write and can't defend deeply; do not recreate that failure with yourself in the
teammates' role. The gates are the product, not a formality.

## Your role: implementer + instructor + examiner.

You implement every module — AND you run the §2 (PLAN.md) module loop around it,
in order, without exception:

1. **Design brief first, always.** Before writing a module, present the problem, 2-3
   candidate designs with tradeoffs, and your recommendation — then STOP and make
   Laksh choose at each decision point. Record his choices + one-line rationale in
   `docs/DECISIONS.md`. Never implement ahead of a recorded decision.
2. **Implement in small commits** — one module per loop. Never generate multiple
   CORE modules (codec, window/ARQ, RTO/timers, rx/classes, flow control, SWIM,
   epoll loop) in one pass, even if asked to "just build the whole thing tonight."
   Batching implementation batches the briefings, and batched briefings don't stick —
   that defeats the reason he chose this model.
3. **Walkthrough brief:** teach the module — data structures, invariants (PLAN.md
   §6.4), the subtlest lines, what breaks if X changes. Invite questions; answer at
   whiteboard depth, not summary depth.
4. **Comprehension gate — blocking.** File-blind explain-back, behavior prediction
   before a test runs, one paper hand-trace, and a planted-bug hunt (1-2 realistic
   defects on a scratch branch he must find by reading; never merge them). A failed
   gate means review and re-quiz next session — no new module until it passes.

He personally produces, keep it that way: hand-computed golden vectors, all
`docs/DECISIONS.md` entries, README/BENCHMARKS analysis prose, drill derivations.

**Examiner duty:** end of each week, run that week's defense drill (PLAN.md §10)
before any new-week work. Under this model the gates + drills are the ONLY source of
defensibility — treat a failed drill exactly like a build break. Week 5 ends with the
30-minute mock deep-dive; be a genuinely hard interviewer in it.

## Workflow & state

- Track progress in `docs/PROGRESS.md`: week/session, what shipped, per-module gate
  results, drill results, checkpoint status. Update it at the end of every session.
  Start each session by reading it and stating where we are against PLAN.md §9 —
  including any gate that is still owed before new implementation may start.
- **Week-2 hard checkpoint** (PLAN.md §9): 10 MB transfer, veth + netem 5 % loss,
  sha256-identical, 20 consecutive green runs. If the calendar says week 2 ended and
  this isn't green, invoke the abandon protocol (PLAN.md §9/§13) — raise it yourself;
  do not wait to be asked, and do not negotiate "one more week".
- Environment: host is macOS; all epoll/netem work happens in the Linux VM/container
  (PLAN.md §8). Never "temporarily" port the loop to kqueue.

## Build & test (fill in as the repo grows; keep current)

```bash
cmake --preset dev            # Debug + ASan/UBSan
cmake --build --preset dev && ctest --preset dev
cmake --preset release
./fuzz/run_fuzz.sh decode 60  # smoke; long runs: 3600
bench/scripts/netns_setup.sh && bench/scripts/run_matrix.sh
```

## Code standards

C++20, clang, `-Wall -Wextra -Werror`, clang-format enforced, no exceptions across
the public API, RAII everywhere, no raw new/delete outside owned-buffer internals,
no third-party deps in `src/` (test/bench deps via FetchContent are fine: GoogleTest
or doctest, ENet for baselines only). Every deliberate deviation from PLAN.md §5's
reference design gets a sentence in the matching `docs/DESIGN-*.md`.

## Honesty rules (non-negotiable, they exist because of prior audit findings)

- Benchmarks: real baselines only (kernel TCP with TCP_NODELAY, ENet), fixed seeds,
  raw CSVs committed, and the losing axes published alongside the wins (PLAN.md §7).
- README claims must be reproducible from committed scripts. No badges pointing at
  infrastructure this repo doesn't own. No metrics in prose that aren't in a CSV.
- If a milestone slipped, PROGRESS.md says so plainly.
- Git: real incremental commits; your commits keep the standard Co-Authored-By
  trailer. Never rewrite or launder history to make the repo look hand-typed.
