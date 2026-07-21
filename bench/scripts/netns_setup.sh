#!/usr/bin/env bash
# Build the veth-across-two-netns topology used by the benchmark matrix (PLAN §6.5).
# Isolated names (benchA/benchB, vbenchA/vbenchB) so this does not collide with the infra
# soak's namespaces. Idempotent: tears down any previous bench topology first. Re-execs
# under sudo (NET_ADMIN required). netem itself is applied per-point by run_matrix.sh.
#
#   sender side  : ns benchA, 10.9.0.1 on vbenchA
#   receiver side: ns benchB, 10.9.0.2 on vbenchB
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then exec sudo -E "$0" "$@"; fi

NS_A=benchA
NS_B=benchB
VA=vbenchA
VB=vbenchB
IP_A=10.9.0.1
IP_B=10.9.0.2
PREFIX=24

ip netns del "${NS_A}" 2>/dev/null || true
ip netns del "${NS_B}" 2>/dev/null || true

ip netns add "${NS_A}"
ip netns add "${NS_B}"
ip link add "${VA}" type veth peer name "${VB}"
ip link set "${VA}" netns "${NS_A}"
ip link set "${VB}" netns "${NS_B}"

ip netns exec "${NS_A}" ip addr add "${IP_A}/${PREFIX}" dev "${VA}"
ip netns exec "${NS_A}" ip link set "${VA}" up
ip netns exec "${NS_A}" ip link set lo up
ip netns exec "${NS_B}" ip addr add "${IP_B}/${PREFIX}" dev "${VB}"
ip netns exec "${NS_B}" ip link set "${VB}" up
ip netns exec "${NS_B}" ip link set lo up

# Disable segmentation/receive offloads so the TCP baseline isn't flattered by GSO/GRO
# coalescing that taut (userspace, one datagram per packet) can't use (§6.5).
ip netns exec "${NS_A}" ethtool -K "${VA}" tso off gso off gro off 2>/dev/null || true
ip netns exec "${NS_B}" ethtool -K "${VB}" tso off gso off gro off 2>/dev/null || true

echo "netns_setup: benchA(${IP_A})/vbenchA <-> benchB(${IP_B})/vbenchB ready (no netem yet)"
