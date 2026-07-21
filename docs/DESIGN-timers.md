# DESIGN — timers

Hand-built binary min-heap of deadlines (§5.7). Backs the ARQ's per-packet RTO deadlines
and the flow-control persist timer. Implemented Week 2.

## Structure
- `std::vector<Entry>` as an implicit binary heap ordered by `deadline` (min at root);
  `sift_up`/`sift_down` are hand-written.
- Each timer gets a monotonic `TimerId`.

## Lazy deletion
`cancel(id)` records the id in a `cancelled_` set — O(1), no search. A cancelled entry stays
in the heap until it reaches the root, at which point `prune_root` discards it (and erases
it from the set). So `next_deadline`/`pop_due`/`empty` all prune first. This trades a little
transient memory for O(1) cancel and avoids the bookkeeping of tracking each entry's heap
index. (PLAN §5.7's stated approach; the alternative — a timer *wheel* — is the classic
follow-up question, deliberately not built.)

## Complexity
schedule O(log n), pop_due O(log n) amortized (plus pruning of dead roots), cancel O(1),
next_deadline O(log n) worst case when pruning a run of cancelled roots.

## Not built (deviation notes)
- No per-entry decrease-key / reschedule; the ARQ reschedules by cancelling + scheduling a
  new timer, which is fine at these cardinalities (≤ window_pkts live timers).
- The generation-counter variant in §5.7 is realized here as a cancelled-id set, which is
  equivalent for our single-shot timers and simpler.
