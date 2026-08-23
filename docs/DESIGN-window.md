# DESIGN - window / ARQ (send path + reliable session)

The reliability engine (`Session`): per-peer send-buffer ring, cumulative acks + SACK,
timer-driven and fast retransmit, the three reliability classes on the receive path, and
receiver-driven flow control - wired to codec + timers + transport. This started as
stop-and-wait (window=1) and now runs the full sliding window with RTO estimation,
SACK/fast-retransmit (below), classes (below), and flow control (docs/DESIGN-flow.md).

## Send-buffer ring (§5.4)
`std::deque<Slot>` of in-flight reliable packets ordered by seq. Each `Slot` holds the seq, its
RTO timer id, the current RTO, a transmit count, first-send time (for RTT), SACK/fast-retransmit
flags, and the **fully-encoded datagram** (so a retransmit is a straight resend - no re-encode,
CRC/header already correct). Capacity is bounded by `effective_window = min(window_pkts,
adv_window)`. Messages the app hands to `send()` are parked in a bounded `pending_` queue and
moved into the ring by `pump()` as the window allows; `send` returns false (backpressure) when
that queue is full. Class 0 skips all of this - it's sent immediately, fire-and-forget.

## Acks - cum_ack semantics (deviation from §5.2)
The wire field is carried verbatim by the codec; the *session* interprets `cum_ack` as
**"next expected sequence"** (the lowest seq not yet received) - the TCP ack convention  - 
rather than §5.2's "highest received with no gaps below." Reason: "next expected" handles
the "nothing received yet" and gap cases with no sentinel value, and makes ack processing a
clean `remove all in-flight slots with seq < cum_ack`. Acks ride on every packet; the
receiver also sends a pure ACK per DATA (delayed/coalesced acks are a later optimization).

## Receive path - reliability classes (§5.3, feat/core)

`handle_data` dispatches on the packet's class. Reliable classes (1,2) share the cum_ack /
SACK / ring machinery; class 0 is entirely separate.

- **Class 0 (Unreliable):** its own seq space (`next_unreliable_seq_`), never retransmitted,
  excluded from cum_ack/SACK/ring. The receiver dedups with a 64-seq sliding **anti-replay
  window** (`ur_base_` + `ur_seen_` bitmap, IPsec-style): a seq below the window or with its
  bit already set is a duplicate and dropped; otherwise it's delivered immediately. Not acked
  (the sender never waits on it). Losing class-0 traffic can never stall reliable delivery  - 
  the whole point of the separate counter (DESIGN-codec #3).
- **Class 1 (ReliableUnordered):** reliable, but delivered **on arrival** - an out-of-order
  packet is handed to the app right away (the latency win: a retransmit doesn't block its
  successors). It still records a payload-less **marker** in `reasm_` so cum_ack advances, the
  SACK bitmap reports it, and its retransmits are deduped.
- **Class 2 (ReliableOrdered):** `rcv_next_` is the next seq to deliver. S == rcv_next_ →
  deliver and flush the contiguous run in `reasm_`; S > rcv_next_ → buffer the payload in
  `reasm_`; S < rcv_next_ → duplicate, drop. Exactly-once (invariant 1) and in-order
  (invariant 2).

Dedup for reliable classes is unified: a seq is "received" iff it is below `rcv_next_` or
present in `reasm_`, so a retransmit of either kind is dropped exactly once - for both class 1
and class 2. Reliable data is always acked.

## Acks + SACK
Every outgoing packet carries `cum_ack = rcv_next_` and `adv_window` (see docs/DESIGN-flow.md).
When out-of-order data is buffered (`reasm_` non-empty) and SACK is enabled, the packet also
sets `flags.SackPresent` and a bitmap where **bit i = seq `rcv_next_ + 1 + i` received** - every
`reasm_` key is strictly above `rcv_next_` (the run below it has been delivered), so
`i = key − rcv_next_ − 1`.

## Retransmit / RTO / fast retransmit
`tick()` pops due RTO timers; a still-in-flight, not-yet-SACKed slot is resent with
`rto = min(rto*2, 2 s)` (exponential backoff, §5.4). RTO comes from the RttEstimator
(Jacobson/Karn); a retransmitted slot yields no RTT sample (Karn).

On an inbound SACK the sender (`process_sack`): (1) marks each SACKed in-flight slot, cancels
its RTO (a received packet must never be retransmitted), and takes its RTT sample now rather
than at the later cum_ack (which would over-measure); (2) **fast-retransmits** any un-SACKed
slot with ≥ 3 SACKed slots *above* it - the Reno 3-dup-ack idea expressed on the bitmap. The
ring is ascending, so a single back-to-front walk counts SACKed slots seen and fires on the
first un-SACKed gap once (`fast_retransmitted` guards against repeats). Fast retransmit does
**not** back off the RTO (it isn't a timeout); it just refreshes the timer so the RTO path
still backs it up. Measured effect on a 5 %-loss sim transfer: SACK cut retransmits ~17× and
iterations ~1.8× vs cumulative-ack-only (see `sack_test.cc`).

## Not yet (tracked)
- Seq wraparound handling at the u32 boundary (irrelevant at test cardinalities; noted).
