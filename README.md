# taut

A purpose-built reliable-UDP transport for small-message meshes on lossy networks:
sliding-window ARQ with SACK, adaptive RTO, per-message reliability classes, and SWIM
failure detection — fuzz-hardened, fault-injected with netem, and benchmarked against
kernel TCP (`TCP_NODELAY`) and ENet.

The thesis: general-purpose transports carry obligations a small-telemetry mesh can
drop (strict in-order delivery, fairness/congestion control, a ~200 ms minimum RTO). By
shedding them we trade bandwidth for tail latency. Both sides of that trade get plotted.

> **Status: in active development.** See [`PLAN.md`](PLAN.md) for scope, design, and
> milestones; [`docs/`](docs/) for per-module design notes and decisions; and
> [`docs/PROGRESS.md`](docs/PROGRESS.md) for current state.

## Build

Requires clang 17+, CMake ≥ 3.24, and Ninja. epoll and netem are Linux-only, so on
macOS develop inside the Linux VM (Lima — see PLAN §8).

```bash
cmake --preset dev            # Debug + ASan/UBSan
cmake --build --preset dev
ctest --preset dev
cmake --preset release        # Release build
cmake --build --preset release
```

## License

MIT — see [`LICENSE`](LICENSE).
