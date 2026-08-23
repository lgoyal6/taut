# DESIGN - RTO (Jacobson/Karn)

`RttEstimator` implements RFC 6298 (§5.5). The `Session` owns one per peer.

## Formula
```
first sample:  SRTT = R;              RTTVAR = R/2
later:         RTTVAR = 3/4 RTTVAR + 1/4 |SRTT - R'|
               SRTT   = 7/8 SRTT   + 1/8 R'
RTO = clamp(SRTT + max(G, 4 RTTVAR), floor, cap)     G = 1 ms, cap = 2 s
```

## The knobs / deviations
- **floor = `Config::rto_floor` (default 25 ms)** - the thesis knob, well below TCP's
  ~200 ms Linux minimum. Safe on a closed low-latency mesh; unsafe on the open internet
  (spurious-retransmit storms, fairness).
- **Initial RTO before the first sample = 200 ms**, a deliberate deviation from RFC 6298's
  1 s - this is a low-latency mesh, and 1 s would stall the very first packet's recovery.

## Karn's rule
The estimator is dumb about ambiguity by design; the `Session` enforces Karn: it samples
RTT only from slots with `transmit_count == 1` (never retransmitted). One sample is taken
per cumulative ack (the highest cleanly-acked packet). Retransmits still back off
(`rto = min(rto*2, 2 s)`) independently of the estimator.
