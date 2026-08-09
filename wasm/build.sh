#!/bin/bash
# Browser demo build: real Sessions over the deterministic SimNet (no sockets,
# no threads), so no cross-origin isolation is needed.
set -euo pipefail
cd "$(dirname "$0")/.."

mkdir -p docs/demo
em++ src/codec.cc src/crc32c.cc src/rto.cc src/session.cc src/sim_net.cc src/timers.cc \
  wasm/taut_wasm.cc \
  -Iinclude \
  -std=c++20 -O2 \
  -sMODULARIZE=1 -sEXPORT_NAME=TautModule \
  -sENVIRONMENT=web \
  -sALLOW_MEMORY_GROWTH=1 \
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap \
  -o docs/demo/taut.js

ls -la docs/demo/
