# Options analytics testing

Run the isolated Python environment required by the engineering contract:

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make format-check
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make lint
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make test
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make benchmark
```

The options tests use deterministic repository-owned contracts and BSM surfaces;
they contain no provider payloads or credentials. The fixed repository seed is
`20260828`.

Coverage includes:

- strict OSI parsing and contract-master revision/corporate-action lineage;
- call/put numerical reference values, arbitrage bounds, IV recovery, Greek
  signs/scales, nonfinite inputs, finite brackets, and iteration exhaustion;
- valid, stale, wide, crossed, expired, unsupported, malformed, and
  arbitrage-inconsistent quote classifications;
- surface sufficiency, call/put monotonicity, strike convexity, calendar total
  variance, skew, term structure, unusual volume, zero-DTE concentration,
  implied move, pinning, confidence, and OOD;
- all three dealer-side assumptions and the observed-versus-inferred split;
- deterministic synthetic surface recovery, abstention followed by corrected
  chain recovery, common forecast publication, and digest-stable replay; and
- service readiness, fixed-cardinality metrics, bounded logs, graceful shutdown,
  and post-shutdown rejection.

The benchmark report is
`build/reports/benchmarks/options-analytics.json`. It records deterministic
full-chain evaluation latency percentiles and throughput on the current host.
It is a baseline, not an accepted performance objective or an economic claim.

