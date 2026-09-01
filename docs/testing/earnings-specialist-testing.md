# Earnings specialist testing

The default deterministic seed is `20260828`. All fixtures are fictional and
contain no licensed issuer text, estimates, option data, transcripts, or market
data.

Run the specialist suite in the required isolated environment:

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts \
  /scratch/djy8hg/env/aegis_mx_contracts/bin/pytest \
  python/tests/test_earnings_specialist.py
```

Coverage includes hand-calculated standardized surprises, zero-dispersion
epsilon, GAAP/non-GAAP incompatibility, missing consensus/prior guidance,
corrections, exact evidence resolution, all point-in-time leakage checks,
accounting diagnostics, probability/range invariants, common forecast decoding,
canonical replay, malformed contracts, invalid market features, expiry overflow,
and every lifecycle transition/rejection.

Fixtures:

- `python/tests/fixtures/earnings/synthetic_earnings_v1.json` is a seeded rich
  event with estimates, segments, adjustment, option move, history, transcript,
  and post-release features.
- `python/tests/fixtures/earnings/manual_curated_earnings_v1.json` is a hand-
  curated sparse event proving incompatible GAAP/non-GAAP estimates remain
  uncomputed.

`make benchmark` records host-dependent specialist evaluation observations in
`build/reports/benchmarks/earnings-specialist.json`. They are diagnostics, not
latency acceptance thresholds, calibration evidence, profitability evidence,
or predictive-value claims.
