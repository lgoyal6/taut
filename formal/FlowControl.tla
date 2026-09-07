---------------------------- MODULE FlowControl ----------------------------
EXTENDS Integers, Sequences, TLC

(******************************************************************************
 A bounded model of Taut's equal-cumulative-ACK flow-control rule.

 The receiver emits a closed-window ACK and then a reopened-window ACK without
 advancing cum_ack. The network may reorder them. AckSeq is the generation
 counter carried in Packet.seq for standalone ACK packets.

 Bounds: one sender, one receiver, Window \in 0..1, AckSeq \in 1..2, and two
 ACK packets. New senders reserve zero for legacy peers. Sequence-number wrap
 is outside this bounded check and is handled by the C++ serial comparison.

 Fairness: weak fairness on Deliver. If an ACK remains in the network, it is
 eventually delivered. The checked property is safety, so fairness does not
 hide the finite counterexample.
******************************************************************************)

CONSTANT Policy
ASSUME Policy \in {"seeded", "fixed"}

VARIABLES network, senderWindow, lastAckSeq, hasAckSeq, reopenedDelivered
vars == <<network, senderWindow, lastAckSeq, hasAckSeq, reopenedDelivered>>

Closed == [cumAck |-> 0, ackSeq |-> 1, window |-> 0]
Reopened == [cumAck |-> 0, ackSeq |-> 2, window |-> 1]

RemoveAt(sequence, i) ==
    SubSeq(sequence, 1, i - 1) \o SubSeq(sequence, i + 1, Len(sequence))

Init ==
    /\ network = <<Closed, Reopened>>
    /\ senderWindow = 0
    /\ lastAckSeq = 0
    /\ hasAckSeq = FALSE
    /\ reopenedDelivered = FALSE

Newer(seq) == ~hasAckSeq \/ seq > lastAckSeq

Apply(ack) ==
    IF Policy = "seeded" \/ Newer(ack.ackSeq)
    THEN <<ack.window, ack.ackSeq, TRUE>>
    ELSE <<senderWindow, lastAckSeq, hasAckSeq>>

Deliver(i) ==
    /\ i \in 1..Len(network)
    /\ LET ack == network[i]
           next == Apply(ack)
       IN /\ senderWindow' = next[1]
          /\ lastAckSeq' = next[2]
          /\ hasAckSeq' = next[3]
          /\ reopenedDelivered' = IF ack.ackSeq = 2 THEN TRUE ELSE reopenedDelivered
    /\ network' = RemoveAt(network, i)

Next == \E i \in 1..Len(network) : Deliver(i)

TypeOK ==
    /\ senderWindow \in 0..1
    /\ lastAckSeq \in 0..2
    /\ hasAckSeq \in BOOLEAN
    /\ reopenedDelivered \in BOOLEAN

NoStaleClose == ~(reopenedDelivered /\ senderWindow = 0)

Spec == Init /\ [][Next]_vars /\ WF_vars(\E i \in 1..Len(network) : Deliver(i))

=============================================================================
