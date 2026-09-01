# Macroeconomic release specialist testing

The deterministic seed is `20260828`. All fixtures are fictional synthetic
replay data and contain no licensed calendar, consensus, release, or market
content.

Run the specialist suite in the required isolated environment:

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts \
  /scratch/djy8hg/env/aegis_mx_contracts/bin/pytest \
  python/tests/test_macro_specialist.py
```

The suite covers hand-calculated headline/core/subcomponent surprises,
zero-dispersion epsilon, revisions, empirical percentile, every initial event
type, cross-asset normalization, common forecast decoding, scheduled/delayed
states, strict abstention, consensus freezing, correction lineage, canonical
replay, timestamp ordering, source/evidence graphs, malformed bounds, and every
schema/result invariant.

Required replay fixtures:

- `normal_release_v1.json` — complete, on-time, consistent release;
- `delayed_release_v1.json` — actual publication after scheduled time;
- `partial_release_v1.json` — missing required core field;
- `correction_v1.json` — linked second revision with frozen consensus;
- `conflicting_provider_values_v1.json` — provider differs from official; and
- `timestamp_disorder_v1.json` — receipt precedes publication and is rejected.

All are under `python/tests/fixtures/macro/`. Tests reconstruct immutable input
contracts from each fixture, compare expected phase/decision/reasons, evaluate
twice, and verify the detailed result digest through the replay API.

`make benchmark` writes host-dependent observations to
`build/reports/benchmarks/macro-specialist.json` for complete-release evaluation
and canonical result hashing. These are diagnostics, not real-time thresholds,
calibration evidence, predictive-value evidence, or profitability claims.
