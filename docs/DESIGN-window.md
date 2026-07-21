# DESIGN — window / ARQ (send path + reliable session)

The reliability engine (`Session`): per-peer send-buffer ring, cumulative acks, and
timer-driven retransmit, wired to codec + timers + transport. Week 2 delivers stop-and-wait
(window=1); the sliding window (64), RTO estimation, and SACK/fast-retransmit follow.

## Send-buffer ring (§5.4)
`std::deque<Slot>` of in-flight packets ordered by seq. Each `Slot` holds the seq, its RTO
timer id, the current RTO, a transmit count, and the **fully-encoded datagram** (so a
retransmit is a straight resend — no re-encode, and the CRC/header are already correct).
Capacity = `Config::window_pkts` (1 for stop-and-wait; `send` returns false = backpressure
when full, upholding invariant 3: `in_flight ≤ window`).

## Acks — cum_ack semantics (deviation from §5.2)
The wire field is carried verbatim by the codec; the *session* interprets `cum_ack` as
**"next expected sequence"** (the lowest seq not yet received) — the TCP ack convention —
rather than §5.2's "highest received with no gaps below." Reason: "next expected" handles
the "nothing received yet" and gap cases with no sentinel value, and makes ack processing a
clean `remove all in-flight slots with seq < cum_ack`. Acks ride on every packet; the
receiver also sends a pure ACK per DATA (delayed/coalesced acks are a later optimization).

## Receive path (class 2, ordered)
`next_expected_` is the next seq to deliver. On DATA seq S: S == next_expected → deliver and
flush any contiguous run buffered in `reasm_`; S > next_expected → buffer in `reasm_`
(reassembly); S < next_expected → duplicate, drop. Always ack. This gives exactly-once
(invariant 1) and in-order (invariant 2) delivery; duplicates from retransmit or the network
are absorbed.

## Retransmit / RTO
`tick()` pops due timers; a still-in-flight slot is resent with `rto = min(rto*2, 2 s)`
(exponential backoff, §5.4). RTO is a fixed initial 50 ms for now — **Jacobson/Karn
estimation is the next module**, at which point Karn's rule (no RTT sample from a
retransmitted packet) applies.

## Not yet (tracked)
- Sliding window > 1, and thus true pipelining / reordering across the window.
- SACK bitmap + fast retransmit (Reno-style, on the bitmap) — Week 3.
- Seq wraparound handling at the u32 boundary (irrelevant at test cardinalities; noted).
