# OMS testing

## Deterministic suites

The OMS suite covers exact risk binding, the full transition matrix, duplicate
receipts and acknowledgements, fill-before-ack, fill/cancel races, replace
acknowledgement and rejection, forbidden-event mutation safety, primary/drop-
copy reconciliation, conflicting executions, stale fencing, authority rotation,
unknown orders, snapshots, journal corruption, restart inhibition, ambiguous
absence, and canonical v1.8 serialization.

An independent `ReferenceOmsModel` uses ordinary maps and a separate transition
implementation. Forty-eight randomized lifecycle scenarios compare its order
state and quantities with production using seed `20260828`. The libFuzzer target
also records seed `20260828` and checks arbitrary state triples and malformed
fixed-layout inputs.

## Commands

Run from the repository root with the required isolated environment:

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make format
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make lint
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make test
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make test-fuzz
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make test-sanitizers
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make benchmark
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make schemas-check
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make docs-check
```

Focused C++ execution is available with `ctest --test-dir build/dev -R
aegis_oms_tests --output-on-failure`. Benchmark categories are dispatch,
acknowledgement, fill, immutable snapshot read, and restart replay. Transition
benchmarks assert zero dynamic allocations after fixture construction.

## Interpretation

Functional latency measurements are infrastructure baselines, not economic or
live-readiness claims. Sanitizer runtime failures caused by a host limitation
must be reported separately from successful sanitizer compilation and from
actual test failures; thresholds are never weakened silently.

