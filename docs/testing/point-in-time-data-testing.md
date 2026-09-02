# Point-in-time data testing

Use the required isolated environment:

```bash
export AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts
"$AEGIS_PYTHON_ENV/bin/python" -m pip install \
  --no-deps --no-build-isolation --editable .
"$AEGIS_PYTHON_ENV/bin/pytest" python/tests/test_point_in_time.py
```

The deterministic fixtures cover all eight record families, inclusive and
strict knowledge cutoffs, revision history, correction/amendment links,
half-open validity, old/new symbols, delisted historical members, and
point-in-time corporate-action history.

Adversarial manifests intentionally introduce and reject:

- future macro revisions;
- future or ineffective constituents;
- post-event analyst estimates;
- unavailable or prematurely applied corporate actions;
- current-universe survivorship bias against a historical delisted member;
- feature/label and cross-split label overlap;
- randomized and nonchronological train/test partitions; and
- missing immutable provenance.

Run the fixed-seed offline benchmark with:

```bash
"$AEGIS_PYTHON_ENV/bin/python" tools/benchmark_point_in_time.py \
  --iterations 100 --records 256 \
  --output build/reports/benchmarks/point-in-time.json
```

It reports p50/p95/p99/maximum latency and throughput for `as_known_at` and a
clean two-split leakage validation. Results are host-specific smoke evidence,
not a hot-path threshold.
