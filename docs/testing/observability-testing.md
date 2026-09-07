# Observability Testing

Use the required isolated Python environment and pinned native toolchain:

```bash
export AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts
source tools/toolchain.sh
cmake --preset dev
cmake --build --preset dev --target aegis_observability_tests
ctest --test-dir build/dev -R '^aegis_observability_tests$' --output-on-failure
```

The focused suite verifies fixed metric aggregation, bounded slots, fixed-stage
latency buckets and p50/p95/p99/p99.9 output, full-queue rejection/drop counts,
component snapshot adapters, structured log correlation/hashes, off-path OTLP
encoding, complete model/ensemble/risk/venue explanations, abstention, and
tamper rejection. Test inputs are deterministic; no random seed is used.

Run the publisher smoke benchmark with:

```bash
cmake --preset release
cmake --build --preset release --target aegis_benchmark_smoke
build/release/cpp/benchmarks/aegis_benchmark_smoke \
  --benchmark_filter=benchmark_observability_bounded_publish \
  --benchmark_min_time=0.01s
```

The benchmark times only one bounded producer enqueue; draining is paused from
measurement. Results are host-specific capacity evidence, not a universal
latency guarantee.

Additional gates:

```bash
make format-check
make lint
make docs-check
make test
make test-sanitizers
```

ThreadSanitizer exercises the existing bounded MPSC queue implementation used
by telemetry, logs, traces, and explanations. Alert rules and dashboards are
checked as text/JSON locally; deployment CI should additionally run `promtool
check rules` and provision dashboards into a disposable Grafana instance.
