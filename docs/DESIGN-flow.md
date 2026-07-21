# DESIGN — flow control + zero-window persist probe

Receiver-driven flow control (§5.6) lives in `Session` alongside the send ring and the
receive path. It bounds how much the sender may have outstanding so a slow or stalled
receiver can never be overrun, and it breaks the classic lost-window-update deadlock with a
persist probe.

## Advertised window: what `adv_window` counts

`adv_window` is carried on every outgoing packet (acks ride on everything) and is the
receiver's **free buffer, in packets**:

```
adv_window = capacity − rcv_buf_.size()          capacity = Config::window_pkts
```

`rcv_buf_` is the **app-delivery queue**: messages that are ready for the application but that
it has not yet drained (see `set_receiving`). Crucially, out-of-order data held in `reasm_`
for class-2 reassembly is **not** subtracted here. That is deliberate, not an oversight:

- Out-of-order buffered packets have seqs in `[cum_ack, next_seq)` — i.e. they are exactly the
  packets the sender already counts as *in flight*. The sender bounds in-flight by
  `adv_window`, so that region is already reserved. Subtracting `reasm_` again would
  double-count it against the same budget.
- With `adv_window = capacity − rcv_buf_`, and the sender obeying `in_flight ≤ adv_window`,
  the total the receiver ever holds is `rcv_buf_ + reasm_ ≤ rcv_buf_ + in_flight ≤
  rcv_buf_ + (capacity − rcv_buf_) = capacity`. No overflow, and no separate reservation math.

This mirrors TCP's `rcv_wnd`, which is measured from `rcv_nxt` (== the sender's `snd_una`) and
already excludes only the *unread in-order* bytes; out-of-order data lives inside the window.

## Sender admission

`pump()` moves queued messages into the ring only while
`ring_.size() < min(window_pkts, peer_adv_window_)`. Messages the app hands to `send()` for a
reliable class are parked in a bounded `pending_` queue first (backpressure = that queue full);
`pump()` drains it as the window allows. The internal queue exists so the session *knows it has
data to send* even when the window is shut — which is what drives the persist timer below.

Window updates are accepted only from a non-stale ack: `peer_adv_window_` is refreshed only
when `cum_ack ≥ last_ack_` (cum_ack is monotonic), so a reordered older ack can't clobber the
window with a stale value (RFC 793's WL rule, simplified).

## Invariant 3, stated precisely

PLAN §6.4 invariant 3 is `in_flight ≤ min(window_pkts, adv_window)`. Two halves:

- **Class 2 (ordered):** it holds *literally, at all times*. Out-of-order data goes to
  `reasm_`, which does not reduce `adv_window`; in-order delivery to a stalled app grows
  `rcv_buf_` in lockstep with `cum_ack` advancing (each shrinks the window by one and frees one
  in-flight slot). The sim test asserts it every tick.
- **Class 1 (unordered):** a packet received out of order is delivered to the app queue
  immediately (the latency win), so it lands in `rcv_buf_` while its seq is *also* still
  in the in-flight window — the two accountings transiently overlap and `in_flight ≤
  adv_window` can be momentarily untrue. The invariant that still holds, and is the one that
  matters, is **no overflow**: receive-buffer occupancy (`rcv_buf_ + reasm_`) never exceeds
  `capacity`. It is guaranteed structurally by a hard receiver-side admission guard —
  `has_buffer_space()` refuses to buffer a new packet at capacity (the sender simply
  retransmits it once the app drains). Tests assert occupancy ≤ capacity in every scenario.

## Zero-window persist probe

When the window shuts to zero and there is nothing in flight whose ack would re-advertise it,
a lost window-reopening ack would deadlock the connection forever. The persist timer prevents
that:

- Arm when `!pending_.empty() && peer_adv_window_ == 0 && ring_.empty()`. (With packets still
  in flight, their RTO retransmits already elicit fresh window advertisements, so no separate
  probe is needed — the timer is specifically for the *nothing-in-flight* case.)
- On expiry send a dedicated `PROBE_WINDOW` packet; the receiver replies with an ack carrying
  its current `adv_window`. Back off the interval ×2 each time, start 100 ms, cap 1 s.
- Disarm the moment any ack reopens the window (`peer_adv_window_ > 0`), then `pump()`.

The receiver also sends an immediate window-update ack when the app resumes draining
(`set_receiving(true)`). If that update is lost, the persist probe still recovers — the sim
test drops exactly that ack and asserts the transfer completes anyway.

## Deviations from the §5.6 reference

- The reference sketch has `send()` block/backpressure directly; we add a small bounded
  internal send queue (`pending_`) so the persist state machine has a truthful "app has data"
  signal. Backpressure is unchanged in spirit (the queue is bounded to `window_pkts`).
- `adv_window` counts only the app-delivery queue, not out-of-order reassembly — see the
  double-count argument above. This is the one accounting choice worth defending out loud.
