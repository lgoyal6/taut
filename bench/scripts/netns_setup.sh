#!/usr/bin/env bash
# netns_setup.sh — the PLAN §6.5 fault-injection fixture.
#
# Builds a veth pair spanning two network namespaces and applies a symmetric netem
# impairment (loss / delay+jitter / reorder / duplicate) to BOTH directions, with
# tso/gso/gro disabled so a TCP baseline (feat/bench) is measured fairly rather than
# against segmentation offloads. Loopback netem has quirks; a veth pair is realistic
# and isolated (§6.5), which is why we go through namespaces instead of `lo`.
#
# Topology:
#   ns taut-a  (10.9.0.1)  vA  <== veth ==>  vB  (10.9.0.2)  ns taut-b
# netem sits on each veth's root (egress) qdisc, so vA impairs A->B and vB impairs B->A.
#
# Usage (run as root, e.g. via sudo inside the Lima VM):
#   netns_setup.sh up     [--loss P] [--delay MS] [--jitter MS] [--reorder P] [--dup P]
#   netns_setup.sh netem  [--loss P] [--delay MS] [--jitter MS] [--reorder P] [--dup P]
#   netns_setup.sh down
#   netns_setup.sh status
#
# `up` (re)creates the whole fixture idempotently; `netem` only rewrites the qdiscs on a
# live fixture (used by the loss sweep, so the sweep doesn't tear the link down each point).
set -euo pipefail

NS_A=taut-a
NS_B=taut-b
VETH_A=vA
VETH_B=vB
IP_A=10.9.0.1
IP_B=10.9.0.2
PREFIX=24

# Defaults match the §9 hard checkpoint: loss 5%, delay 30ms +/- 10ms, light reorder/dup.
LOSS=5
DELAY=30
JITTER=10
REORDER=1
DUP=0.5

require_root() {
    if [[ "$(id -u)" -ne 0 ]]; then
        echo "netns_setup.sh: must run as root (use sudo)" >&2
        exit 1
    fi
}

parse_netem_flags() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --loss) LOSS="$2"; shift 2 ;;
            --delay) DELAY="$2"; shift 2 ;;
            --jitter) JITTER="$2"; shift 2 ;;
            --reorder) REORDER="$2"; shift 2 ;;
            --dup) DUP="$2"; shift 2 ;;
            *) echo "netns_setup.sh: unknown flag '$1'" >&2; exit 1 ;;
        esac
    done
}

# Build the netem argument list. reorder needs a nonzero delay to have anything to reorder,
# so we only add reorder/duplicate when they're > 0 and a delay is present.
netem_args() {
    local args="loss ${LOSS}%"
    if [[ "${DELAY}" != "0" ]]; then
        args+=" delay ${DELAY}ms"
        if [[ "${JITTER}" != "0" ]]; then
            args+=" ${JITTER}ms distribution normal"
        fi
        if [[ "${REORDER}" != "0" ]]; then
            args+=" reorder ${REORDER}%"
        fi
    fi
    if [[ "${DUP}" != "0" ]]; then
        args+=" duplicate ${DUP}%"
    fi
    echo "${args}"
}

apply_netem() {
    local mode="$1" # add | change
    # shellcheck disable=SC2046
    ip netns exec "${NS_A}" tc qdisc "${mode}" dev "${VETH_A}" root netem $(netem_args)
    # shellcheck disable=SC2046
    ip netns exec "${NS_B}" tc qdisc "${mode}" dev "${VETH_B}" root netem $(netem_args)
}

offloads_off() {
    # Disable segmentation/aggregation offloads on the veths so a length-prefixed TCP
    # baseline isn't measured against GSO/GRO. Some features are fixed on veth; tolerate
    # per-feature failures rather than aborting the whole fixture.
    ip netns exec "${NS_A}" ethtool -K "${VETH_A}" tso off gso off gro off 2>/dev/null || \
        echo "netns_setup.sh: warning: could not disable all offloads on ${VETH_A}" >&2
    ip netns exec "${NS_B}" ethtool -K "${VETH_B}" tso off gso off gro off 2>/dev/null || \
        echo "netns_setup.sh: warning: could not disable all offloads on ${VETH_B}" >&2
}

do_up() {
    require_root
    parse_netem_flags "$@"
    do_down >/dev/null 2>&1 || true # idempotent: start from a clean slate

    ip netns add "${NS_A}"
    ip netns add "${NS_B}"
    ip link add "${VETH_A}" type veth peer name "${VETH_B}"
    ip link set "${VETH_A}" netns "${NS_A}"
    ip link set "${VETH_B}" netns "${NS_B}"

    ip netns exec "${NS_A}" ip addr add "${IP_A}/${PREFIX}" dev "${VETH_A}"
    ip netns exec "${NS_A}" ip link set "${VETH_A}" up
    ip netns exec "${NS_A}" ip link set lo up
    ip netns exec "${NS_B}" ip addr add "${IP_B}/${PREFIX}" dev "${VETH_B}"
    ip netns exec "${NS_B}" ip link set "${VETH_B}" up
    ip netns exec "${NS_B}" ip link set lo up

    offloads_off
    apply_netem add

    echo "netns_setup: up — ${NS_A}(${IP_A}) <-> ${NS_B}(${IP_B}), netem [$(netem_args)] each way"
}

do_netem() {
    require_root
    parse_netem_flags "$@"
    apply_netem change
    echo "netns_setup: netem -> [$(netem_args)] each way"
}

do_down() {
    require_root
    ip netns del "${NS_A}" 2>/dev/null || true
    ip netns del "${NS_B}" 2>/dev/null || true
    echo "netns_setup: down"
}

do_status() {
    for ns in "${NS_A}" "${NS_B}"; do
        if ip netns list | grep -q "^${ns}"; then
            echo "== ${ns} =="
            ip netns exec "${ns}" ip -brief addr show 2>/dev/null || true
            ip netns exec "${ns}" tc qdisc show 2>/dev/null || true
        else
            echo "== ${ns} == (absent)"
        fi
    done
}

cmd="${1:-}"
shift || true
case "${cmd}" in
    up) do_up "$@" ;;
    netem) do_netem "$@" ;;
    down) do_down ;;
    status) do_status ;;
    *)
        echo "usage: netns_setup.sh {up|netem|down|status} [--loss P] [--delay MS] [--jitter MS] [--reorder P] [--dup P]" >&2
        exit 1
        ;;
esac
