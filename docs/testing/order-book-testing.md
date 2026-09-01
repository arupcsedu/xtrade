# Order-book testing and benchmarks

Order-book tests use the repository seed `20260828`. Time and sequence values are
explicit; no test sleeps or reads a system clock for correctness.

## Coverage

- order add, cancel, partial cancel, replace, execute, partial execute, and
  delete;
- price-level set, delete, side clear, and book clear;
- FIFO queue priority and deterministic priority loss on replace;
- sorted depth, aggregate sums/counts, top cache, capacity, sequence, cross, and
  malformed-update failures;
- status, halt override, auction imbalance, and tape-only trade behavior;
- validated active/staging snapshot replacement and corrupt snapshot rejection;
- session and corporate-action resets;
- wrong-thread mutation and query rejection;
- venue/session-wide duplicate order IDs and multi-venue consolidation;
- randomized order-level differential testing against an independent map-based
  oracle after every operation; and
- randomized price-level ordering and aggregate properties.

## Commands

Activate the required repository environment first:

```bash
source /scratch/djy8hg/env/aegis_mx_contracts/bin/activate
source tools/toolchain.sh
```

Run the focused test binary:

```bash
cmake --preset ci
cmake --build --preset ci --target aegis_order_book_tests
build/ci/cpp/order_book/aegis_order_book_tests --gtest_color=no
```

Run the release benchmarks:

```bash
cmake --preset release
cmake --build --preset release --target aegis_benchmark_smoke
build/release/cpp/benchmarks/aegis_benchmark_smoke \
  --benchmark_filter='benchmark_mutation|benchmark_depth_query|benchmark_throughput'
```

The mutation benchmarks report p50, p95, p99, and p99.9 samples. All order-book
benchmarks report `steady_state_allocations`; a nonzero result is a failure of the
allocation-free claim. Throughput is measured at 32, 256, 1,024, and 2,048 live
orders. Results are host-specific and are not acceptance thresholds until a
hardware-capacity ADR establishes baselines and tolerances.
