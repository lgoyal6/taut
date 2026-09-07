#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$tmp/old"
git -C "$root" archive v0.2.1 | tar -x -C "$tmp/old"

install_package() {
  source_dir="$1"
  build_dir="$2"
  cmake -S "$source_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release \
    -DTAUT_INSTALL=ON -DTAUT_BUILD_TESTS=OFF -DTAUT_BUILD_DEMOS=OFF
  cmake --build "$build_dir" --target taut
  cmake --install "$build_dir" --prefix "$tmp/prefix"
}

run_consumer() {
  expected="$1"
  build_dir="$2"
  cmake -S "$root/tests/compat/consumer" -B "$build_dir" \
    -DCMAKE_PREFIX_PATH="$tmp/prefix" -DEXPECTED_TAUT_VERSION="$expected"
  cmake --build "$build_dir"
  "$build_dir/taut_compat_consumer"
}

install_package "$tmp/old" "$tmp/build-old"
run_consumer 0.2.1 "$tmp/consumer-old"

install_package "$root" "$tmp/build-current"
run_consumer 0.2.2 "$tmp/consumer-current"

if cmake -S "$root/tests/compat/incompatible" -B "$tmp/incompatible" \
  -DCMAKE_PREFIX_PATH="$tmp/prefix" >/dev/null 2>&1; then
  echo "incompatible 0.3 consumer unexpectedly accepted the 0.2 package" >&2
  exit 1
fi

echo "taut package upgrade v0.2.1 to v0.2.2 passed"
