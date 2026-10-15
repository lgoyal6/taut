#!/usr/bin/env bash
# Runs the retained closed-loop aioquic control with verified local TLS.
# run_paced_study.py supplies the matched-offer comparison and shared bottleneck.
# Requires fresh private outputs; cleanup is registered before Docker resources exist.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DATA="${DATA_DIR:?Set DATA_DIR to a new private directory outside the checkout}"
IMAGE="${QUIC_IMAGE:-b5-quic-tools:temporary}"
RUNS="${RUNS:-5}"
DURATION="${DURATION:-3}"
RTT="${RTT:-30}"
RATE="${RATE:-500}"
SEED0="${SEED0:-1}"
LOSSES="${LOSSES:-0 5 20}"
PORT=9300
RUN_ID="$(python3 -c 'import uuid; print(uuid.uuid4().hex[:12])')"
NET="b5-quic-net-${RUN_ID}"
VOLUME="b5-quic-data-${RUN_ID}"
A="b5-quic-a-${RUN_ID}"
B="b5-quic-b-${RUN_ID}"
IP_A=10.9.0.2
IP_B=10.9.0.3
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
CERT_DIR="$(mktemp -d "${TMPDIR:-/tmp}/b5-quic-cert.XXXXXX")"
created_a=0
created_b=0
created_network=0
created_volume=0
cleanup() {
  if (( created_a )); then docker rm -f "${A}" >/dev/null 2>&1 || true; fi
  if (( created_b )); then docker rm -f "${B}" >/dev/null 2>&1 || true; fi
  if (( created_network )); then docker network rm "${NET}" >/dev/null 2>&1 || true; fi
  if (( created_volume )); then docker volume rm "${VOLUME}" >/dev/null 2>&1 || true; fi
  rm -rf "${CERT_DIR}"
}
trap cleanup EXIT
docker build -f "${ROOT}/bench/Dockerfile.quic" -t "${IMAGE}" "${ROOT}"
docker network create --internal --subnet 10.9.0.0/24 --gateway 10.9.0.1 "${NET}" >/dev/null
created_network=1
docker volume create "${VOLUME}" >/dev/null
created_volume=1
docker run -d --name "${A}" --network "${NET}" --ip "${IP_A}" --cpus=1 --memory=1g --cap-add=NET_ADMIN -v "${VOLUME}:/bench-data" "${IMAGE}" sleep infinity >/dev/null
created_a=1
docker run -d --name "${B}" --network "${NET}" --ip "${IP_B}" --cpus=1 --memory=1g --cap-add=NET_ADMIN -v "${VOLUME}:/bench-data" "${IMAGE}" sleep infinity >/dev/null
created_b=1
docker cp "${ROOT}/bench/quic_baseline.py" "${A}:/opt/bench/quic_baseline.py"
docker cp "${ROOT}/bench/quic_baseline.py" "${B}:/opt/bench/quic_baseline.py"
docker exec "${A}" ethtool -K eth0 tso off gso off gro off
docker exec "${B}" ethtool -K eth0 tso off gso off gro off
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 1 -keyout "${CERT_DIR}/quic.key" -out "${CERT_DIR}/quic.crt" -subj '/CN=localhost' -addext "subjectAltName=DNS:localhost,IP:${IP_B}" >/dev/null 2>&1
docker cp "${CERT_DIR}/quic.key" "${B}:/opt/bench/quic.key"
docker cp "${CERT_DIR}/quic.crt" "${B}:/opt/bench/quic.crt"
docker cp "${CERT_DIR}/quic.crt" "${A}:/opt/bench/quic.crt"
printf 'transport,loss_pct,rtt_ms,run,seed,wire_tx_bytes,wire_rx_bytes\n' >"${DATA}/quic_wire.csv"
printf 'image=%s\n' "$(docker image inspect "${IMAGE}" --format '{{.Id}}')"
printf 'aioquic=%s\n' "$(docker exec "${A}" python3 -c 'import aioquic; print(aioquic.__version__)')"
printf 'topology=two internal-bridge containers, separate network namespaces, symmetric egress netem, TSO/GSO/GRO disabled\n'
printf 'resource_caps=1 CPU and 1 GiB per endpoint; no host networking, bind mounts, or privileged mode\n'
printf 'payload=512 bytes with existing timestamp/sequence/filler layout; one outstanding request; stream echo; localhost TLS certificate verified by client\n'
printf 'runs=%s duration_s=%s rtt_ms=%s rate_label=%s losses=%s\n' "${RUNS}" "${DURATION}" "${RTT}" "${RATE}" "${LOSSES}"
printf 'mode=closed-loop request/reply, one request in flight; rate is a recorded scenario label and does not pace RR offers\n'
for loss in ${LOSSES}; do
  delay=$((RTT / 2))
  spec="delay ${delay}ms"
  if [[ "${loss}" != 0 ]]; then spec="loss ${loss}% ${spec}"; fi
  docker exec "${A}" tc qdisc replace dev eth0 root netem ${spec}
  docker exec "${B}" tc qdisc replace dev eth0 root netem ${spec}
  echo "=== quic rr loss=${loss}% rtt=${RTT}ms ==="
  for run in $(seq 1 "${RUNS}"); do
    seed=$((SEED0 + run - 1))
    tx0=$(docker exec "${A}" cat /sys/class/net/eth0/statistics/tx_bytes)
    rx0=$(docker exec "${A}" cat /sys/class/net/eth0/statistics/rx_bytes)
    docker exec "${B}" python3 /opt/bench/quic_baseline.py server --host "${IP_B}" --port "${PORT}" --cert /opt/bench/quic.crt --key /opt/bench/quic.key >"${DATA}/quic_server.log" 2>&1 &
    server_exec=$!
    sleep 0.5
    docker exec "${A}" python3 /opt/bench/quic_baseline.py client --host "${IP_B}" --port "${PORT}" --ca /opt/bench/quic.crt --duration "${DURATION}" --response-timeout 20 --loss "${loss}" --rtt "${RTT}" --rate "${RATE}" --run "${run}" --seed "${seed}" --out /bench-data/quic_latency.csv
    wait "${server_exec}"
    tx1=$(docker exec "${A}" cat /sys/class/net/eth0/statistics/tx_bytes)
    rx1=$(docker exec "${A}" cat /sys/class/net/eth0/statistics/rx_bytes)
    printf 'quic,%s,%s,%s,%s,%s,%s\n' "${loss}" "${RTT}" "${run}" "${seed}" "$((tx1 - tx0))" "$((rx1 - rx0))" >>"${DATA}/quic_wire.csv"
  done
done
docker cp "${A}:/bench-data/quic_latency.csv" "${DATA}/quic_latency.csv"
