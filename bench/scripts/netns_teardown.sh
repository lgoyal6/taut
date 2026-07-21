#!/usr/bin/env bash
# Remove the benchmark veth/netns topology created by netns_setup.sh. Deleting the netns
# also deletes the veth pair. Re-execs under sudo.
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then exec sudo -E "$0" "$@"; fi

ip netns del benchA 2>/dev/null || true
ip netns del benchB 2>/dev/null || true
echo "netns_teardown: benchA/benchB removed"
