# Time-series forecast testing

Tests use seed `20260828`, injected monotonic clocks, synthetic integer series,
and immutable identifiers. They make no network call, require no checkpoint or
GPU, and state `economic_value_claim=false`.

Coverage includes malformed and leaking point-in-time contexts, insufficient
history, missing future covariates, all targets and units, multi-horizon and
quantile outputs, bounded batching, queue saturation, GPU failure with CPU
retry, adapter failure with explicitly attributed fallback, deadline miss,
stale cache rejection, service health/readiness/configuration hash, common
`ModelForecast` publication, and deterministic walk-forward baseline reports.
The repository Python gate currently exercises 115 tests at 100% statement and
branch coverage; this count is evidence from the 2026-08-30 implementation run,
not a threshold substitute.

The walk-forward harness evaluates returns, realized volatility, volume, and
spread separately. Raw price is not a target. Each split uses only samples whose
availability is at or before that split's point-in-time cutoff.

Run the focused suite with the required environment:

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts \
  /scratch/djy8hg/env/aegis_mx_contracts/bin/python -m pytest \
  python/tests/test_timeseries_forecasting.py
```

Repository gates remain `make format-check`, `make lint`, `make test`,
`make schemas-check`, `make docs-check`, `make dependency-scan`, and
`make benchmark` with `AEGIS_PYTHON_ENV` set to the same external environment.

`make benchmark` writes `build/reports/benchmarks/timeseries.json`. Categories
are canonical context decode and deterministic reference-adapter batches of 1,
8, and 32 contexts. Results report p50/p95/p99 and throughput, are host-specific,
and are not predictive-value or trading-latency acceptance claims.
