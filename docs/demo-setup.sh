#!/usr/bin/env bash
# Builds the SWIM mesh demo that docs/demo.tape records against. It runs on the
# in-process SimNet and a virtual clock, so it is portable and deterministic:
# the same numbers appear on macOS and Linux.
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build/demo -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build/demo -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" --target mesh_node >/dev/null
echo "ready: $(pwd)/build/demo/mesh_node"
echo "now run: vhs docs/demo.tape"
