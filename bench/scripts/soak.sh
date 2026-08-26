#!/usr/bin/env bash
# soak.sh - the PLAN §9 Week-2 HARD CHECKPOINT and the loss sweep.
#
# Checkpoint: transfer a 10 MB file over taut class 2 across the veth/netns fixture at the
# §6.5 impairment (loss 5%, delay 30ms +/- 10ms, reorder 1%, duplicate 0.5%), assert the
# received file is sha256-identical to the source, and require 20 CONSECUTIVE green runs.
# Each run uses fresh random content, so 20/20 is 20 independent reliability proofs.
#
# Sweep: repeat the transfer at loss in {0,1,5,10,20}% to show goodput vs loss (the numbers
# that motivate SACK + fast retransmit in Week 3). Sweep uses a smaller file so the high-loss
# points stay tractable under RTO-only recovery.
#
# Integrity is checked with the system `sha256sum` on both files (ground truth, independent
# of the digest the binaries print). Run as root (netns exec needs it), e.g. via sudo.
#
# Usage:
#   sudo bench/scripts/soak.sh [--runs N] [--size BYTES] [--sweep-size BYTES]
#                              [--bin DIR] [--no-sweep] [--keep] [--timeout-ms N]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
NETNS="${SCRIPT_DIR}/netns_setup.sh"

NS_RECV=taut-a
NS_SEND=taut-b
IP_RECV=10.9.0.1
IP_SEND=10.9.0.2
PORT_RECV=7001
PORT_SEND=7000

RUNS=20
SIZE=10485760       # 10 MB checkpoint file
SWEEP_SIZE=2097152  # 2 MB sweep file (keeps 20% loss tractable)
BIN="${TAUT_BIN:-${REPO_ROOT}/build/release/demo}"
TIMEOUT_MS=300000
LINGER_MS=2000
DO_SWEEP=1
KEEP=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --runs) RUNS="$2"; shift 2 ;;
        --size) SIZE="$2"; shift 2 ;;
        --sweep-size) SWEEP_SIZE="$2"; shift 2 ;;
        --bin) BIN="$2"; shift 2 ;;
        --timeout-ms) TIMEOUT_MS="$2"; shift 2 ;;
        --no-sweep) DO_SWEEP=0; shift ;;
        --keep) KEEP=1; shift ;;
        *) echo "soak.sh: unknown flag '$1'" >&2; exit 1 ;;
    esac
done

if [[ "$(id -u)" -ne 0 ]]; then
    echo "soak.sh: must run as root (use sudo) - netns exec needs it" >&2
    exit 1
fi
if [[ ! -x "${BIN}/send_file" || ! -x "${BIN}/recv_file" ]]; then
    echo "soak.sh: send_file/recv_file not found in ${BIN} (build them first, or pass --bin)" >&2
    exit 1
fi

WORK="$(mktemp -d /tmp/taut_soak.XXXXXX)"
cleanup() {
    rm -rf "${WORK}"
    if [[ "${KEEP}" -eq 0 ]]; then
        "${NETNS}" down >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

# One transfer. Args: <size-bytes> <label>. Echoes "<wall_s> <goodput_kBps>" on success;
# returns 0 iff the received file is sha256-identical to the source.
run_one() {
    local size="$1" label="$2"
    local in="${WORK}/in.bin" out="${WORK}/out.bin"
    rm -f "${in}" "${out}"
    head -c "${size}" /dev/urandom > "${in}"
    local in_sha out_sha
    in_sha="$(sha256sum "${in}" | cut -d' ' -f1)"

    ip netns exec "${NS_RECV}" "${BIN}/recv_file" \
        --bind "${IP_RECV}:${PORT_RECV}" --peer "${IP_SEND}:${PORT_SEND}" \
        --out "${out}" --timeout-ms "${TIMEOUT_MS}" --linger-ms "${LINGER_MS}" \
        >/dev/null 2>"${WORK}/recv.err" &
    local recv_pid=$!
    sleep 0.3

    local start end
    start="$(date +%s.%N)"
    if ! ip netns exec "${NS_SEND}" "${BIN}/send_file" \
        --bind "${IP_SEND}:${PORT_SEND}" --peer "${IP_RECV}:${PORT_RECV}" \
        --in "${in}" --timeout-ms "${TIMEOUT_MS}" >/dev/null 2>"${WORK}/send.err"; then
        echo "  [${label}] FAIL: send_file error: $(cat "${WORK}/send.err")" >&2
        kill "${recv_pid}" 2>/dev/null || true
        wait "${recv_pid}" 2>/dev/null || true
        return 1
    fi
    # Transfer is complete once the sender holds all acks; stop the clock here so the
    # receiver's fixed linger (last-ack-loss insurance) doesn't inflate the goodput number.
    end="$(date +%s.%N)"
    if ! wait "${recv_pid}"; then
        echo "  [${label}] FAIL: recv_file error: $(cat "${WORK}/recv.err")" >&2
        return 1
    fi

    out_sha="$(sha256sum "${out}" | cut -d' ' -f1)"
    if [[ "${in_sha}" != "${out_sha}" ]]; then
        echo "  [${label}] FAIL: sha256 mismatch (in ${in_sha:0:12} != out ${out_sha:0:12})" >&2
        return 1
    fi
    local wall kbps
    wall="$(echo "${end} - ${start}" | bc)"
    kbps="$(echo "scale=1; ${size} / 1024 / ${wall}" | bc)"
    echo "${wall} ${kbps}"
    return 0
}

echo "=== taut netem soak - bin=${BIN} ==="
echo

# --- Checkpoint: 20 consecutive green at the full §6.5 impairment (loss 5%). ---
echo ">>> CHECKPOINT: ${RUNS} consecutive $(awk "BEGIN{printf \"%.1f\", ${SIZE}/1048576}") MB runs at loss 5% (delay 30ms +/-10ms, reorder 1%, dup 0.5%)"
"${NETNS}" up --loss 5 --delay 30 --jitter 10 --reorder 1 --dup 0.5 >/dev/null

green=0
for i in $(seq 1 "${RUNS}"); do
    if result="$(run_one "${SIZE}" "run ${i}/${RUNS}")"; then
        green=$((green + 1))
        printf "  run %2d/%d: PASS  (%ss, %s kB/s)\n" "${i}" "${RUNS}" \
            "$(echo "${result}" | cut -d' ' -f1)" "$(echo "${result}" | cut -d' ' -f2)"
    else
        printf "  run %2d/%d: FAIL - streak broken\n" "${i}" "${RUNS}"
        break
    fi
done

echo
if [[ "${green}" -eq "${RUNS}" ]]; then
    echo "CHECKPOINT RESULT: ${green}/${RUNS} GREEN - PASS"
    checkpoint_ok=1
else
    echo "CHECKPOINT RESULT: ${green}/${RUNS} green - FAIL (need ${RUNS} consecutive)"
    checkpoint_ok=0
fi

# --- Sweep: goodput vs loss. ---
if [[ "${DO_SWEEP}" -eq 1 ]]; then
    echo
    echo ">>> SWEEP: $(awk "BEGIN{printf \"%.1f\", ${SWEEP_SIZE}/1048576}") MB at loss {0,1,5,10,20}%"
    echo "    (kB/s here is rough soak goodput to show the loss trend - NOT the §7 benchmark,"
    echo "     which uses proper methodology + TCP/ENet baselines and is owned by feat/bench)"
    printf "  %-6s %-8s %-10s %-8s\n" "loss%" "result" "wall_s" "kB/s"
    for loss in 0 1 5 10 20; do
        "${NETNS}" netem --loss "${loss}" --delay 30 --jitter 10 --reorder 1 --dup 0.5 >/dev/null
        if result="$(run_one "${SWEEP_SIZE}" "loss ${loss}%")"; then
            printf "  %-6s %-8s %-10s %-8s\n" "${loss}" "PASS" \
                "$(echo "${result}" | cut -d' ' -f1)" "$(echo "${result}" | cut -d' ' -f2)"
        else
            printf "  %-6s %-8s %-10s %-8s\n" "${loss}" "FAIL" "-" "-"
        fi
    done
fi

echo
[[ "${checkpoint_ok}" -eq 1 ]] && exit 0 || exit 1
