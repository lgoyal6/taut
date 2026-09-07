#!/usr/bin/env python3
"""Exhaust the two-ACK state space described by FlowControl.tla."""

from __future__ import annotations

import argparse
from collections import deque
from dataclasses import dataclass


@dataclass(frozen=True)
class Ack:
    cum_ack: int
    ack_seq: int
    window: int


@dataclass(frozen=True)
class State:
    network: tuple[Ack, ...]
    sender_window: int
    last_ack_seq: int
    has_ack_seq: bool
    reopened_delivered: bool


CLOSED = Ack(cum_ack=0, ack_seq=1, window=0)
REOPENED = Ack(cum_ack=0, ack_seq=2, window=1)


def step(state: State, index: int, policy: str) -> State:
    ack = state.network[index]
    newer = not state.has_ack_seq or ack.ack_seq > state.last_ack_seq
    accept = policy == "seeded" or newer
    network = state.network[:index] + state.network[index + 1 :]
    if not accept:
        return State(
            network,
            state.sender_window,
            state.last_ack_seq,
            state.has_ack_seq,
            state.reopened_delivered or ack.ack_seq == 2,
        )
    return State(network, ack.window, ack.ack_seq, True, state.reopened_delivered or ack.ack_seq == 2)


def violates(state: State) -> bool:
    return state.reopened_delivered and state.sender_window == 0


def check(policy: str) -> tuple[int, list[Ack] | None]:
    initial = State((CLOSED, REOPENED), 0, 0, False, False)
    queue = deque([(initial, [])])
    seen = {initial}
    explored = 0
    while queue:
        state, trace = queue.popleft()
        explored += 1
        if violates(state):
            return explored, trace
        for index, ack in enumerate(state.network):
            successor = step(state, index, policy)
            if successor not in seen:
                seen.add(successor)
                queue.append((successor, trace + [ack]))
    return explored, None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--policy", choices=("seeded", "fixed"), required=True)
    args = parser.parse_args()
    explored, trace = check(args.policy)
    if args.policy == "seeded":
        if trace is None:
            print(f"seeded policy: no counterexample in {explored} states")
            return 1
        rendered = " -> ".join(
            f"ack(cum={ack.cum_ack},seq={ack.ack_seq},window={ack.window})" for ack in trace
        )
        print(f"seeded policy: counterexample after {len(trace)} deliveries: {rendered}")
        print(f"explored_states={explored}")
        return 0
    if trace is not None:
        print(f"fixed policy: invariant violated after {len(trace)} deliveries")
        return 1
    print(f"fixed policy: NoStaleClose holds across {explored} states")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
