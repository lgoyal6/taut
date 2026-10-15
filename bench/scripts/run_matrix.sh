#!/usr/bin/env bash
# Runs the retained TCP/Taut/optional ENet controls through netns or Docker.
# run_paced_study.py owns the matched-offer and competing-flow comparisons.
# Requires a fresh private DATA_DIR so an earlier receipt is never overwritten.
set -euo pipefail

TOPOLOGY="${TOPOLOGY:-netns}"
if [[ "${TOPOLOGY}" == "netns" && ${EUID} -ne 0 ]]; then
    echo "netns mode requires an explicitly authorized root invocation" >&2
    exit 1
fi

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
BIN_DEFAULT="${ROOT}/build/release/bench"
if [[ "${TOPOLOGY}" == "docker" ]]; then BIN_DEFAULT="${ROOT}/build/docker-bench/bench"; fi
BIN="${BIN:-${BIN_DEFAULT}}"
DATA="${DATA_DIR:?Set DATA_DIR to a new private directory outside the checkout}"

DOCKER_IMAGE="${DOCKER_IMAGE:-taut-linux}"
RUN_ID="$(python3 -c 'import uuid; print(uuid.uuid4().hex[:12])')"
DOCKER_A="b5-bench-a-${RUN_ID}"
DOCKER_B="b5-bench-b-${RUN_ID}"
DOCKER_NET="b5-matrix-net-${RUN_ID}"
DOCKER_VOLUME="b5-matrix-data-${RUN_ID}"
CONTAINER_BIN=/opt/bench
CONTAINER_DATA=/bench-data

NS_A=benchA
NS_B=benchB
VA=vbenchA
VB=vbenchB
IP_A=10.9.0.1
IP_B=10.9.0.2
if [[ "${TOPOLOGY}" == "docker" ]]; then
    NS_A="${DOCKER_A}"
    NS_B="${DOCKER_B}"
    VA=eth0
    VB=eth0
    # Docker reserves the first address for its bridge gateway.
    IP_A=10.9.0.2
    IP_B=10.9.0.3
    BIN="${BIN:-${ROOT}/build/docker-bench/bench}"
fi
DELAY=$((RTT / 2)) # netem one-way delay; RTT = 2 * DELAY

# Keep checkout paths relative in the retained command output.
rel() { printf '%s' "${1#"${ROOT}"/}"; }

TAUT_PORT=9000
TCP_PORT=9100
ENET_PORT=9200

DATA="$(python3 - "${DATA}" "${ROOT}" <<'PYDATA'
from pathlib import Path
import sys
output, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
if output == root or root in output.parents:
    raise SystemExit("DATA_DIR must be outside the checkout")
output.mkdir(parents=True, exist_ok=False)
print(output)
PYDATA
)"
LAT_CSV="${DATA}/latency.csv"
SEND_CSV="${DATA}/send.csv"
WIRE_CSV="${DATA}/wire.csv"
THRU_CSV="${DATA}/throughput.csv"

have_enet=0
[[ -x "${BIN}/enet_baseline" ]] && have_enet=1

echo "run_matrix: losses=[${LOSSES}] runs=${RUNS} dur=${DURATION}s rate=${RATE}/s rtt=${RTT}ms classes=[${TAUT_CLASSES}] enet=${have_enet}"
echo "run_matrix: binaries in $(rel "${BIN}"), CSVs to $(rel "${DATA}")"

created_a=0
created_b=0
created_network=0
created_volume=0
cleanup_docker() {
    local status=$?
    if [[ "${TOPOLOGY}" == "docker" ]]; then
        # Retain available CSVs even when setup or a measurement exits early.
        if (( created_a )); then
            for csv in latency send throughput; do
                docker cp "${NS_A}:${CONTAINER_DATA}/${csv}.csv" "${DATA}/${csv}.csv" 2>/dev/null || true
            done
        fi
        if (( created_a )); then docker rm -f "${DOCKER_A}" >/dev/null 2>&1 || true; fi
        if (( created_b )); then docker rm -f "${DOCKER_B}" >/dev/null 2>&1 || true; fi
        if (( created_network )); then docker network rm "${DOCKER_NET}" >/dev/null 2>&1 || true; fi
        if (( created_volume )); then docker volume rm "${DOCKER_VOLUME}" >/dev/null 2>&1 || true; fi
    fi
    # Copying from Docker may create root-owned files, including on failure.
    if [[ -n "${SUDO_USER:-}" ]]; then chown -R "${SUDO_USER}:$(id -gn "${SUDO_USER}")" "${DATA}"; fi
    return "${status}"
}
trap cleanup_docker EXIT

# Fresh topology + fresh CSVs for a clean, reproducible matrix.
if [[ "${TOPOLOGY}" == "netns" ]]; then
    bash "${SCRIPT_DIR}/netns_bench.sh" >/dev/null
elif [[ "${TOPOLOGY}" == "docker" ]]; then
    command -v docker >/dev/null || { echo "run_matrix: docker CLI required for TOPOLOGY=docker" >&2; exit 1; }
    docker network create --internal --subnet 10.9.0.0/24 --gateway 10.9.0.1 "${DOCKER_NET}" >/dev/null
    created_network=1
    docker volume create "${DOCKER_VOLUME}" >/dev/null
    created_volume=1
    docker run -d --name "${DOCKER_A}" --network "${DOCKER_NET}" --ip "${IP_A}" \
        --cpus=1 --memory=1g --cap-add=NET_ADMIN -v "${DOCKER_VOLUME}:${CONTAINER_DATA}" \
        "${DOCKER_IMAGE}" sleep infinity >/dev/null
    created_a=1
    docker run -d --name "${DOCKER_B}" --network "${DOCKER_NET}" --ip "${IP_B}" \
        --cpus=1 --memory=1g --cap-add=NET_ADMIN -v "${DOCKER_VOLUME}:${CONTAINER_DATA}" \
        "${DOCKER_IMAGE}" sleep infinity >/dev/null
    created_b=1
    docker exec "${DOCKER_A}" mkdir -p "${CONTAINER_BIN}" "${CONTAINER_DATA}"
    docker exec "${DOCKER_B}" mkdir -p "${CONTAINER_BIN}" "${CONTAINER_DATA}"
    for name in latency_bench tcp_baseline enet_baseline; do
        if [[ -x "${BIN}/${name}" ]]; then
            docker cp "${BIN}/${name}" "${DOCKER_A}:${CONTAINER_BIN}/${name}"
            docker cp "${BIN}/${name}" "${DOCKER_B}:${CONTAINER_BIN}/${name}"
        fi
    done
    docker exec "${DOCKER_A}" ethtool -K eth0 tso off gso off gro off
    docker exec "${DOCKER_B}" ethtool -K eth0 tso off gso off gro off
else
    echo "run_matrix: TOPOLOGY must be netns or docker" >&2
    exit 2
fi

if [[ ! -f "${WIRE_CSV}" ]]; then
    echo "transport,cls,mode,loss_pct,rtt_ms,rate,run,seed,wire_tx_bytes,wire_rx_bytes" >"${WIRE_CSV}"
fi

apply_netem() {
    local loss=$1
    local spec="delay ${DELAY}ms"
    if [[ "${loss}" != "0" ]]; then spec="loss ${loss}% ${spec}"; fi
    if [[ "${TOPOLOGY}" == "docker" ]]; then
        docker exec "${NS_A}" tc qdisc replace dev "${VA}" root netem ${spec}
        docker exec "${NS_B}" tc qdisc replace dev "${VB}" root netem ${spec}
    else
        ip netns exec "${NS_A}" tc qdisc replace dev "${VA}" root netem ${spec}
        ip netns exec "${NS_B}" tc qdisc replace dev "${VB}" root netem ${spec}
    fi
}

wire_bytes() {
    if [[ "${TOPOLOGY}" == "docker" ]]; then
        docker exec "${NS_A}" cat "/sys/class/net/${VA}/statistics/$1"
    else
        ip netns exec "${NS_A}" cat "/sys/class/net/${VA}/statistics/$1"
    fi
}

# run_point <transport> <bin> <port> <cls> <loss> <run> <seed> <mode>
run_point() {
    local transport=$1 bin=$2 port=$3 cls=$4 loss=$5 run=$6 seed=$7 mode=$8
    local out_csv="${LAT_CSV}"
    [[ "${mode}" == "throughput" ]] && out_csv="${THRU_CSV}"
    local container_bin="${bin}"
    local container_out="${out_csv}"
    local container_send="${SEND_CSV}"
    if [[ "${TOPOLOGY}" == "docker" ]]; then
        container_bin="${CONTAINER_BIN}/$(basename "${bin}")"
        container_out="${CONTAINER_DATA}/$(basename "${out_csv}")"
        container_send="${CONTAINER_DATA}/send.csv"
    fi

    local common="--transport ${transport} --mode ${mode} --duration ${DURATION} --rate ${RATE}"
    common+=" --seed ${seed} --run ${run} --loss ${loss} --rtt ${RTT} --msg-size ${MSG}"

    local recv_args sender_args
    case "${transport}" in
    taut)
        recv_args="--role receiver --bind ${IP_B} --addr ${IP_A} --port ${port} --class ${cls} --idle 4000"
        sender_args="--role sender --bind ${IP_A} --addr ${IP_B} --port ${port} --class ${cls} --send-out ${container_send}"
        ;;
    tcp)
        recv_args="--role receiver --bind ${IP_B} --port ${port} --idle 4000"
        sender_args="--role sender --addr ${IP_B} --port ${port} --send-out ${container_send}"
        ;;
    enet)
        recv_args="--role receiver --bind 0.0.0.0 --port ${port} --idle 4000"
        sender_args="--role sender --addr ${IP_B} --port ${port} --send-out ${container_send}"
        ;;
    esac
    # The measuring side gets --out: for rr it is the client (sender role); for open-loop
    # latency/throughput it is the receiver.
    if [[ "${mode}" == "rr" ]]; then
        sender_args+=" --out ${container_out}"
    else
        recv_args+=" --out ${container_out}"
    fi

    local tx0 rx0 tx1 rx1
    tx0=$(wire_bytes tx_bytes)
    rx0=$(wire_bytes rx_bytes)

    if [[ "${TOPOLOGY}" == "docker" ]]; then
        docker exec "${NS_B}" "${container_bin}" ${common} ${recv_args} &
    else
        ip netns exec "${NS_B}" "${bin}" ${common} ${recv_args} &
    fi
    local rpid=$!
    sleep 0.5
    local sender_exit=0 receiver_exit=0
    if [[ "${TOPOLOGY}" == "docker" ]]; then
        docker exec "${NS_A}" "${container_bin}" ${common} ${sender_args} || sender_exit=$?
    else
        ip netns exec "${NS_A}" "${bin}" ${common} ${sender_args} || sender_exit=$?
    fi
    wait "${rpid}" || receiver_exit=$?

    tx1=$(wire_bytes tx_bytes)
    rx1=$(wire_bytes rx_bytes)
    echo "${transport},${cls},${mode},${loss}.000000,${RTT}.000000,${RATE}.000000,${run},${seed},$((tx1 - tx0)),$((rx1 - rx0))" >>"${WIRE_CSV}"
    if (( sender_exit || receiver_exit )); then
        echo "run_matrix: failed ${transport}/${mode}: sender_exit=${sender_exit} receiver_exit=${receiver_exit}; stopping before another case" >&2
        if (( sender_exit )); then return "${sender_exit}"; fi
        return "${receiver_exit}"
    fi
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
# loss (received << offered) - that divergence is a finding, not a bug (see BENCHMARKS.md).
if [[ "${RUN_OPENLOOP:-0}" == "1" ]]; then
    for loss in ${LOSSES}; do
        apply_netem "${loss}"
        echo "=== open-loop @ loss ${loss}% (RTT ${RTT}ms, rate ${RATE}/s) ==="
        for run in $(seq 1 "${RUNS}"); do
            run_all_transports "${loss}" "${run}" "$((SEED0 + run - 1))" latency
        done
    done
fi

# ---- clean-link (0%) saturating throughput - the axis where TCP/ENet win ---------------
if [[ "${RUN_THROUGHPUT:-1}" == "1" ]]; then
    apply_netem 0
    echo "=== throughput @ 0% loss (RTT ${RTT}ms) ==="
    for run in $(seq 1 "${RUNS}"); do
        run_all_transports 0 "${run}" "$((SEED0 + run - 1))" throughput
    done
fi

# Docker CSVs are retained by the EXIT trap before its private volume is removed.
echo "run_matrix: done. CSVs in $(rel "${DATA}")"
if [[ "${TOPOLOGY}" == "netns" ]]; then bash "${SCRIPT_DIR}/netns_teardown.sh" >/dev/null || true; fi
