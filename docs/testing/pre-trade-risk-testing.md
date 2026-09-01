# Pre-trade risk testing

## Determinism and coverage

Risk tests cover boundary equality, checked integer overflow, authorization and
safety precedence, all 30 rule families, rate and duplicate tables, stale and
unavailable state, revision rollback/conflict, journal exhaustion, concurrent
fills/orders, split-brain authority epochs, and all five kill scopes. The replay
property test uses seed `20260828` and compares 1,000 decisions and hashes from
independent engines.

The schema v1.6 tests cover older/newer compatibility, semantic audit validation,
and risk-decision serialization. Generic audit-envelope fuzzing uses the same
recorded seed.

## Commands

```bash
export AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts
source tools/toolchain.sh
make format-check
make lint
make test
make schemas-check
make docs-check
make test-fuzz
make test-sanitizers
cmake --build build/dev --target aegis_benchmark_smoke
build/dev/cpp/benchmarks/aegis_benchmark_smoke \
  --benchmark_filter='benchmark_risk_(evaluation|latency_distribution|kill_switch_to_rejection)'
```

The distribution benchmark reports p50, p95, p99, and p99.9 nanoseconds from
2,048 unique prebuilt intents and separately reports steady-state allocation
count. The kill benchmark measures a firm kill command followed by the first
applicable rejection. Results are host-specific evidence, not a portable
latency guarantee.

## Current release evidence

Recorded 2026-08-30 on the shared 40 × 2.5 GHz build host at load average
13.80/14.58/14.53:

| Measurement | Result |
| --- | ---: |
| Risk evaluation mean wall time | 2,468 ns |
| Throughput | 406,203 evaluations/s |
| Steady-state evaluation allocations | 0 |
| p50 | 2,417 ns |
| p95 | 3,732 ns |
| p99 | 3,849 ns |
| p99.9 | 7,483 ns |
| Firm kill command plus first rejection | 2,389 ns |

These values establish a baseline only. Production thresholds require pinned
cores, representative hardware, controlled load, and an accepted performance
budget.
