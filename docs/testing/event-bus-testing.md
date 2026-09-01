# Event Bus and Shared-State Testing

## Commands

Use the required repository-owned Python environment and the checked-in build
presets:

```bash
export AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts
make format-check
make lint
make test
make test-sanitizers
make benchmark
make docs-check
```

For a focused deterministic run:

```bash
source tools/toolchain.sh
cmake --preset dev
cmake --build --preset dev --target aegis_event_bus_tests
build/dev/cpp/event_bus/aegis_event_bus_tests --gtest_color=no
```

The fixed project seed is `20260828`. Concurrent tests use monotonic sequence
payloads rather than random scheduling assumptions and do not use wall-clock
sleeps for correctness.

## Coverage map

| Requirement | Evidence |
| --- | --- |
| SPSC empty, full, wrap, lag, fail-closed | `SpscRingTest` |
| MPSC empty, full, wrap, producer ordering | `MpscQueueTest`, `EventBusConcurrencyTest` |
| Sustained producer/consumer races | 500,000 SPSC events and 300,000 MPSC events per run |
| Immutable snapshots and stale readers | pinned-version, no-free-slot, checksum, and concurrent torn-read tests |
| Bounded symbol state | duplicate, capacity, unknown, and independent snapshot tests |
| Crash, restart, heartbeat, split brain | `ProcessEpochTest` and anonymous `MAP_SHARED` child-process crash test |
| ABI/schema/region validation | forecast-cache attach negative tests |
| Late, corrupt, and wrong-epoch forecasts | forecast-cache read/publish negative tests |
| Partial publication recovery | abandoned odd-sequence takeover test |
| Performance and allocation | SPSC enqueue/dequeue, snapshot read, and contended MPSC benchmarks |

ThreadSanitizer is required for concurrency changes. If the host kernel or
address-space policy prevents the TSan runtime from starting, the build result
and exact runtime diagnostic must be reported as a limitation; it is not a
passing TSan run. CI must execute the same test binary on a supported runner.

## Benchmark interpretation

The benchmark executable reports SPSC enqueue/dequeue time, immutable snapshot
read time, steady-state allocation count, contended MPSC throughput, queue
rejections and maximum lag, plus enqueue p50/p95/p99/p99.9/maximum latency under
four producers. Runs use fixed message counts. Retain JSON output under
`build/reports/benchmarks/`.

These smoke results detect gross regression only. They do not establish a
production service-level objective until a representative host is isolated,
affinity and NUMA placement are recorded, and an accepted capacity ADR defines
the workload and threshold.
