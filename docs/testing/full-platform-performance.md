# Full-platform performance benchmarks

The comprehensive suite produces a versioned summary plus immutable raw samples for
all 14 required stages. It runs only in simulation or paper mode and contains no
real exchange transport.

## Commands

Use the required external Python environment:

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make benchmark-platform-smoke
```

The smoke command pins the C++ sampler, warms each case, runs a bounded sample set,
and writes:

- `build/reports/benchmarks/platform-report.json`;
- `build/reports/benchmarks/platform-samples.ndjson`;
- `build/reports/benchmarks/platform-sampler.json`.

A qualification run uses at least 10,000 observations per measured latency case:

```bash
AEGIS_PLATFORM_BENCHMARK_SAMPLES=10000 \
AEGIS_PLATFORM_BENCHMARK_WARMUP=4096 \
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts \
make benchmark-platform
```

Pin the process to the same isolated CPU and NUMA node used to approve the baseline.
The sampler verifies CPU affinity itself. Caches are warmed before measurement.
Do not run a qualification while frequency scaling, unrelated workloads, thermal
throttling, or host migration can affect the result.

## Scenarios

The scenario matrix is ordinary, 2x peak, 3x peak, burst, multi-symbol, news event,
macro event, halt, and degraded feed. The last two must fail closed. Consequently,
`tick-to-intent` and `tick-to-paper-send` are `SAFETY_BLOCKED` in those cases and
have no fabricated latency distribution.

Load multiples are back-to-back offered batches. Each raw latency observation is
the worst single-operation latency within its batch, while throughput counts all
operations. This prevents a 3x batch from being reported as one event. Multi-symbol
cases cycle eight preallocated synthetic symbol-state working-set lanes before the
measured operation; their checksums are retained in case notes. These are software
capacity workloads, not a claim about a production instrument universe.

## Interpreting evidence

`IN_PROCESS_SYNTHETIC` covers decode and downstream software costs but excludes a
physical NIC, driver, interrupt, DMA, and production feed adapter. A real-NIC run
must use `REAL_NIC_CAPTURE`; the regression tool refuses to compare the two.

CPU utilization is thread CPU time divided by elapsed monotonic time. Allocation
counts cover the measured steady-state region. Queue high-watermark and packet
drops are observed counters, not inferred from latency. Cache misses are nullable:
the report records why `perf_event_open` was unavailable rather than substituting
zero. `shared-memory-read` measures the immutable edge snapshot store;
`forecast-cache-read` measures the versioned, epoch-validated shared-memory ABI
cache with an in-process reader. Neither number includes an RPC.

## Regression gate

An approved baseline is never created automatically. Compare a candidate with:

```bash
AEGIS_BENCHMARK_BASELINE=/approved/aegis-platform-report.json \
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts \
make benchmark-regression
```

The gate checks report integrity, complete stage/scenario coverage, minimum sample
count, environment comparability, tail latency, throughput, CPU use, allocations,
queue occupancy, and ordinary-load drops. It writes
`build/reports/benchmarks/regression.json` and exits nonzero on any violation.

Cache misses are reported but are not a default regression threshold because their
availability and multiplexing vary by kernel policy. A site may tighten the signed
policy; thresholds must never be silently weakened.
