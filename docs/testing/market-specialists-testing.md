# Market-specialists testing

Run the repository-required isolated environment:

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make format-check
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make lint
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make test
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make benchmark
```

The deterministic seed is `20260828`. Fixtures under
`python/tests/fixtures/market_specialists/` are fictional and repository-owned;
they contain no provider payload, licensed data, endpoint, or credential.

Coverage includes:

- closing-auction and rebalance-day replay with result-digest verification;
- positive, zero, and negative rebalance weight deltas;
- active and inactive rebalance state in auction estimates;
- queue-unknown and pro-rata allocation assumptions;
- bid- and ask-side hidden-liquidity inference, sparse evidence, stale evidence,
  no-impact observations, and weak evidence that clamps probability to zero;
- symbol disablement, low data quality, unauthenticated sources, stale inputs,
  insufficient history, and insufficient evidence;
- point-in-time leakage, missing provenance, source mismatch, invalid ranges,
  invalid timestamp domains, incompatible schemas, oversized collections, and
  malformed publish/abstain results; and
- 100% statement and branch coverage for both specialist modules.

The benchmark report is
`build/reports/benchmarks/market-specialists.json`. It records p50/p95/p99
evaluation latency and throughput for each pure evaluator on the current host.
It is a smoke baseline, not a production objective or economic claim.
