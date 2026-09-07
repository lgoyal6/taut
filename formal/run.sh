#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${TAUT_FORMAL_BUILD_DIR:-${repo_root}/build/formal}"

python3 "${repo_root}/formal/check_model.py" --policy seeded
python3 "${repo_root}/formal/check_model.py" --policy fixed

if [[ -n "${TLA2TOOLS_JAR:-}" ]]; then
    tlc_dir="$(mktemp -d "${TMPDIR:-/tmp}/taut-tlc.XXXXXX")"
    trap 'rm -rf "${tlc_dir}"' EXIT
    cp "${repo_root}/formal/FlowControl.tla" \
        "${repo_root}/formal/FlowControl.cfg" \
        "${repo_root}/formal/FlowControlSeeded.cfg" "${tlc_dir}"
    java -cp "${TLA2TOOLS_JAR}" tla2sany.SANY "${tlc_dir}/FlowControl.tla" >/dev/null
    if java -XX:+UseParallelGC -jar "${TLA2TOOLS_JAR}" -deadlock \
        -config "${tlc_dir}/FlowControlSeeded.cfg" -metadir "${tlc_dir}/seeded" \
        "${tlc_dir}/FlowControl.tla" >"${tlc_dir}/seeded.log" 2>&1; then
        echo "TLC seeded policy unexpectedly passed" >&2
        exit 1
    fi
    grep -q "Invariant NoStaleClose is violated" "${tlc_dir}/seeded.log"
    echo "TLC seeded policy: NoStaleClose violated as expected"
    java -XX:+UseParallelGC -jar "${TLA2TOOLS_JAR}" -deadlock \
        -config "${tlc_dir}/FlowControl.cfg" -metadir "${tlc_dir}/fixed" \
        "${tlc_dir}/FlowControl.tla" >"${tlc_dir}/fixed.log"
    grep -q "Model checking completed. No error has been found." "${tlc_dir}/fixed.log"
    echo "TLC fixed policy: model checking completed with no error"
fi

cmake -S "${repo_root}" -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DTAUT_BUILD_TESTS=OFF \
    -DTAUT_BUILD_DEMOS=OFF \
    -DTAUT_BUILD_FORMAL=ON
cmake --build "${build_dir}" --target taut_formal_conformance
"${build_dir}/taut_formal_conformance" "${repo_root}/formal/counterexample.trace"
