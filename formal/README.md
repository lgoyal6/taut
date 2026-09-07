# Bounded flow-control model

`FlowControl.tla` models one narrow production rule: how a sender accepts
receiver-window updates when two standalone ACKs carry the same cumulative ACK.
The model uses `Packet.seq` as an ACK-generation counter, matching `Session` and
the existing wire layout.

The checked bounds are deliberately small and explicit: one sender, one
receiver, window values 0 and 1, ACK generations 1 and 2, and exactly two ACKs.
Generation zero is reserved for legacy peers. The receiver creates a closed-window
update followed by a reopened-window update; the network can deliver either one next.
Weak fairness on delivery says a queued
ACK is eventually delivered. The `NoStaleClose` safety invariant does not depend
on that fairness assumption.

The seeded policy is the production rule before this fix: every equal
`cum_ack` overwrites the sender window. Breadth-first exploration visits five
states and finds the shortest counterexample in two deliveries:

```text
ack(cum=0,seq=2,window=1) -> ack(cum=0,seq=1,window=0)
```

That is a newer reopening ACK followed by a reordered older close. The old rule
ends at window 0. The fixed policy visits four states with no invariant failure:
it accepts an equal-`cum_ack` standalone ACK only when its generation is newer.
`counterexample.trace` is this exact delivery order. `conformance.cc` encodes
those packets with Taut's real codec, injects them into a real `Session`, and
checks that the final peer window remains open.

Run the whole check with no credentials or services:

```bash
./formal/run.sh
```

The Python program is a dependency-free explicit-state checker for the bounded
transition system, so the default run does not require installing TLC. The TLA+
module remains the reviewable specification. To run the official parser and TLC
checks as part of the same command, point `TLA2TOOLS_JAR` at an installed
`tla2tools.jar`:

```bash
TLA2TOOLS_JAR=/path/to/tla2tools.jar ./formal/run.sh
```

That path requires the seeded config to violate `NoStaleClose` and the fixed
config to complete without an error.

This result is intentionally limited. It proves one invariant over two ACKs and
two window values. It does not prove the data, SACK, retransmission, ACK-counter
wraparound, or multi-peer state machines. The C++ suite covers those mechanisms;
the serial-number comparison handles wraparound, but wraparound is outside this
model's state bound.
