# Contributing to taut

Thanks for looking. taut is small enough to hold in your head in an afternoon, and
that is a deliberate feature. Contributions that keep it that way are very welcome.

## Getting oriented

- The per-module notes in `docs/` (`DESIGN-codec.md`, `DESIGN-window.md`,
  `DESIGN-rto.md`, and the rest) are the source of truth for protocol decisions.
  The `§5.x` section numbers referenced throughout the code point at
  [`docs/DESIGN.md`](docs/DESIGN.md).
- `include/taut/` is the public surface. `src/` implements it.
- `tests/unit/` runs the protocol over `SimNet`, a deterministic in-process network
  with a virtual clock. Same seed, same result: every failure reproduces exactly.
- The live browser demo (https://lgoyal6.github.io/taut/) runs the real Session code
  over SimNet compiled to WebAssembly; `wasm/` has the build.

## Building and testing

```
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Fuzzing (libFuzzer): `fuzz/run_fuzz.sh`. Sanitizer presets are in CMakePresets.json;
please run ASan/UBSan before opening a PR that touches the codec or session logic.

## What makes a good PR here

- One concern per PR, with a test that fails before and passes after.
- Protocol-behavior changes need a SimNet test at a hostile setting (loss 20%+,
  jitter enabled) and a note in the matching `docs/DESIGN-*.md`.
- Determinism is sacred: nothing in `src/` may read wall-clock time or randomness
  except through the injected transport/clock. If your change breaks
  same-seed-same-bytes reproducibility, it will not land.
- Benchmarks: numbers in the README come from `bench/` runs under netem. If your
  change moves them, include before/after output rather than editing the numbers.

## Good first areas

Check the issues labeled `good-first-issue` and `help-wanted`. The event loop is
Linux-only (epoll) today; a kqueue backend for macOS/BSD is the most-wanted
contribution and has a self-contained surface (`src/loop.cc`, `include/taut/loop.h`).

## Conduct

Be kind, assume good faith, argue with benchmarks and packet traces rather than
adjectives.
