#!/usr/bin/env bash
# Build and run a taut fuzzer. Usage: ./fuzz/run_fuzz.sh <target> <seconds>
#   ./fuzz/run_fuzz.sh decode 60      # smoke
#   ./fuzz/run_fuzz.sh decode 3600    # long run
set -euo pipefail

target="${1:-decode}"
seconds="${2:-60}"
build="${BUILD_DIR:-build/fuzz}"

cmake -S . -B "${build}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ \
    -DTAUT_SANITIZE=address,undefined -DTAUT_BUILD_FUZZERS=ON -DTAUT_BUILD_TESTS=OFF >/dev/null
cmake --build "${build}" --target "fuzz_${target}" >/dev/null

corpus="fuzz/corpus/${target}"
mkdir -p "${corpus}"
exec "${build}/fuzz/fuzz_${target}" -max_total_time="${seconds}" "${corpus}"
