# Mixture-of-experts gate testing

Use the required isolated environment:

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make schemas-check
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make format-check
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make lint
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make test
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make test-sanitizers
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make benchmark
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make docs-check
```

The deterministic property-test seed is `20260828`. Coverage includes:

- TimesFM/timeseries downweighting during breaking news;
- halt, invalid feed, stale forecast, disabled model, missing provenance,
  calibration, OOD, data-quality, scope, and control-generation exclusions;
- increased disagreement/effective uncertainty for divergent experts;
- all-invalid abstention and an empty serialized contributor vector;
- exact PPM weight sum, configured per-expert cap, infeasible cap, ties, and
  canonical permutation parity;
- rule, configurable linear, and quantized learned gates using the same masks;
- proof that a maximum learned bias cannot bypass a hard state mask;
- strict equality abstention at cost plus penalty plus safety margin;
- in-memory and audit-contract hash/range/relationship tampering; and
- 1,000 fixed-seed randomized valid mixtures with invariant validation.

The benchmark evaluates 2, 8, and 16 experts and reports latency, throughput,
and the allocation counter. `steady_state_allocations` must remain zero. Host
results are diagnostic baselines, not production acceptance thresholds; tail
qualification belongs on the deployment target with isolated cores and the
production compiler/configuration.
