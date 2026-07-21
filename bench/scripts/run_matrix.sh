#!/usr/bin/env bash
# Latency-vs-loss benchmark sweep (PLAN §7). For each loss point it runs, over the veth/netns
# topology (netns_setup.sh), the identical 512 B / Poisson / N-second workload across:
#   - kernel TCP (TCP_NODELAY)          tcp_baseline
#   - ENet reliable channel             enet_baseline   (skipped if not built)
#   - taut, each class in TAUT_CLASSES  latency_bench
# and captures per-run latency percentiles (bench/data/latency.csv), send-side counts
# (send.csv) and interface-level bytes-on-wire (wire.csv). Also runs a 0%-loss saturating
# throughput point (throughput.csv) — the clean-link axis where TCP/ENet are expected to win.
#
# Re-execs under sudo (netns/tc need NET_ADMIN). The binaries do not need root; they inherit
# it only because `ip netns exec` runs them in the namespace.
#
# Knobs (env): LOSSES RUNS DURATION RATE RTT TAUT_CLASSES MSG SEED0 BIN
# Quick smoke:  LOSSES="0 5 20" RUNS=1 DURATION=3 bench/scripts/run_matrix.sh
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then exec sudo -E "$0" "$@"; fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

LOSSES="${LOSSES:-0 1 5 10 20}"
RUNS="${RUNS:-5}"
DURATION="${DURATION:-60}"
RATE="${RATE:-500}"
RTT="${RTT:-30}"
TAUT_CLASSES="${TAUT_CLASSES:-2}"
MSG="${MSG:-512}"
SEED0="${SEED0:-1}"
BIN="${BIN:-${ROOT}/build/release/bench}"
DATA="${ROOT}/bench/data"

NS_A=benchA
NS_B=benchB
VA=vbenchA
VB=vbenchB
IP_A=10.9.0.1
IP_B=10.9.0.2
DELAY=$((RTT / 2)) # netem one-way delay; RTT = 2 * DELAY

TAUT_PORT=9000
TCP_PORT=9100
ENET_PORT=9200

mkdir -p "${DATA}"
LAT_CSV="${DATA}/latency.csv"
SEND_CSV="${DATA}/send.csv"
WIRE_CSV="${DATA}/wire.csv"
THRU_CSV="${DATA}/throughput.csv"

have_enet=0
[[ -x "${BIN}/enet_baseline" ]] && have_enet=1

echo "run_matrix: losses=[${LOSSES}] runs=${RUNS} dur=${DURATION}s rate=${RATE}/s rtt=${RTT}ms classes=[${TAUT_CLASSES}] enet=${have_enet}"
echo "run_matrix: binaries in ${BIN}, CSVs to ${DATA}"

# Fresh topology + fresh CSVs for a clean, reproducible matrix.
bash "${SCRIPT_DIR}/netns_setup.sh" >/dev/null
rm -f "${LAT_CSV}" "${SEND_CSV}" "${WIRE_CSV}" "${THRU_CSV}"
if [[ ! -f "${WIRE_CSV}" ]]; then
    echo "transport,cls,mode,loss_pct,rtt_ms,rate,run,seed,wire_tx_bytes,wire_rx_bytes" >"${WIRE_CSV}"
fi

apply_netem() {
    local loss=$1
    local spec="delay ${DELAY}ms"
    if [[ "${loss}" != "0" ]]; then spec="loss ${loss}% ${spec}"; fi
    ip netns exec "${NS_A}" tc qdisc replace dev "${VA}" root netem ${spec}
    ip netns exec "${NS_B}" tc qdisc replace dev "${VB}" root netem ${spec}
}

wire_bytes() { ip netns exec "${NS_A}" cat "/sys/class/net/${VA}/statistics/$1"; }

# run_point <transport> <bin> <port> <cls> <loss> <run> <seed> <mode>
run_point() {
    local transport=$1 bin=$2 port=$3 cls=$4 loss=$5 run=$6 seed=$7 mode=$8
    local out_csv="${LAT_CSV}"
    [[ "${mode}" == "throughput" ]] && out_csv="${THRU_CSV}"

    local common="--transport ${transport} --mode ${mode} --duration ${DURATION} --rate ${RATE}"
    common+=" --seed ${seed} --run ${run} --loss ${loss} --rtt ${RTT} --msg-size ${MSG}"

    local recv_args sender_args
    case "${transport}" in
    taut)
        recv_args="--role receiver --bind ${IP_B} --addr ${IP_A} --port ${port} --class ${cls} --idle 4000"
        sender_args="--role sender --bind ${IP_A} --addr ${IP_B} --port ${port} --class ${cls} --send-out ${SEND_CSV}"
        ;;
    tcp)
        recv_args="--role receiver --bind ${IP_B} --port ${port} --idle 4000"
        sender_args="--role sender --addr ${IP_B} --port ${port} --send-out ${SEND_CSV}"
        ;;
    enet)
        recv_args="--role receiver --bind 0.0.0.0 --port ${port} --idle 4000"
        sender_args="--role sender --addr ${IP_B} --port ${port} --send-out ${SEND_CSV}"
        ;;
    esac
    # The measuring side gets --out: for rr it is the client (sender role); for open-loop
    # latency/throughput it is the receiver.
    if [[ "${mode}" == "rr" ]]; then
        sender_args+=" --out ${out_csv}"
    else
        recv_args+=" --out ${out_csv}"
    fi

    local tx0 rx0 tx1 rx1
    tx0=$(wire_bytes tx_bytes)
    rx0=$(wire_bytes rx_bytes)

    ip netns exec "${NS_B}" "${bin}" ${common} ${recv_args} &
    local rpid=$!
    sleep 0.5
    ip netns exec "${NS_A}" "${bin}" ${common} ${sender_args} || true
    wait "${rpid}" || true

    tx1=$(wire_bytes tx_bytes)
    rx1=$(wire_bytes rx_bytes)
    echo "${transport},${cls},${mode},${loss}.000000,${RTT}.000000,${RATE}.000000,${run},${seed},$((tx1 - tx0)),$((rx1 - rx0))" >>"${WIRE_CSV}"
    sleep 0.3
}

# run_all_transports <loss> <run> <seed> <mode>
run_all_transports() {
    local loss=$1 run=$2 seed=$3 mode=$4
    run_point tcp "${BIN}/tcp_baseline" "${TCP_PORT}" "" "${loss}" "${run}" "${seed}" "${mode}"
    if [[ ${have_enet} -eq 1 ]]; then
        run_point enet "${BIN}/enet_baseline" "${ENET_PORT}" "" "${loss}" "${run}" "${seed}" "${mode}"
    fi
    for cls in ${TAUT_CLASSES}; do
        run_point taut "${BIN}/latency_bench" "${TAUT_PORT}" "${cls}" "${loss}" "${run}" "${seed}" "${mode}"
    done
}

# ---- headline: closed-loop request-reply latency vs loss (rr) --------------------------
if [[ "${RUN_RR:-1}" == "1" ]]; then
    for loss in ${LOSSES}; do
        apply_netem "${loss}"
        echo "=== rr @ loss ${loss}% (RTT ${RTT}ms) ==="
        for run in $(seq 1 "${RUNS}"); do
            run_all_transports "${loss}" "${run}" "$((SEED0 + run - 1))" rr
        done
    done
fi

# ---- optional: open-loop Poisson sustained-load latency (RUN_OPENLOOP=1) ---------------
# Shows delivery ratio + coordinated-omission-corrected latency; TCP/ENet saturate under
# loss (received << offered) — that divergence is a finding, not a bug (see BENCHMARKS.md).
if [[ "${RUN_OPENLOOP:-0}" == "1" ]]; then
    for loss in ${LOSSES}; do
        apply_netem "${loss}"
        echo "=== open-loop @ loss ${loss}% (RTT ${RTT}ms, rate ${RATE}/s) ==="
        for run in $(seq 1 "${RUNS}"); do
            run_all_transports "${loss}" "${run}" "$((SEED0 + run - 1))" latency
        done
    done
fi

# ---- clean-link (0%) saturating throughput — the axis where TCP/ENet win ---------------
if [[ "${RUN_THROUGHPUT:-1}" == "1" ]]; then
    apply_netem 0
    echo "=== throughput @ 0% loss (RTT ${RTT}ms) ==="
    for run in $(seq 1 "${RUNS}"); do
        run_all_transports 0 "${run}" "$((SEED0 + run - 1))" throughput
    done
fi

# Hand CSV ownership back to the invoking user so git/commit is painless.
if [[ -n "${SUDO_USER:-}" ]]; then chown -R "${SUDO_USER}:$(id -gn "${SUDO_USER}")" "${DATA}"; fi

echo "run_matrix: done. CSVs in ${DATA}"
bash "${SCRIPT_DIR}/netns_teardown.sh" >/dev/null || true
