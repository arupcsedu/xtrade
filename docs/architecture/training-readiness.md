# Bounded forecasting training readiness

## Purpose

`aegis-training-readiness` is the only admission path from an accepted Prompt
57 dataset to Prompt 58 training. It is offline, performs no network operation,
has no order-entry dependency, and is dry-run by default.

The command authenticates the ticker universe, private Alpaca source-approval
record, Alpaca backfill report, canonical promotion, dataset manifest, and
every published Parquet object. The approval must be unexpired at the recorded
UTC assessment time and must match the data root, universe hash, source, feed,
timeframe, academic purpose, internal-only distribution, training permission,
and backfill approval hash. An approved absolute data-root alias is accepted
only when it resolves to the same authenticated filesystem target; path text
alone is not trusted. The report records the UTC check time, normalized expiry,
authorization result, and approval-document hash even when the result is
blocked. It then creates a deterministic training feature mask from
TRAIN-only normalization statistics. A bounded streaming pass intersects that
mask with every horizon label in TRAIN, VALIDATION, and TEST. An immutable
report is published last only when all blocking checks pass.

## Current admitted training scope

The POC can train pooled return models with 17 observed, nonconstant features:

- five return windows;
- volume, relative volume, realized volatility, and high-low range;
- intraday gap, minute of session, and session position;
- market return and market-relative return; and
- rolling five-session return and volume; and
- the SEC-filing-backed news-event flag.

The current dataset excludes five columns from model input:

| Feature | Reason |
| --- | --- |
| Sector return and relative return | No accepted point-in-time sector snapshot and no TRAIN observations |
| Macro flag | No accepted ALFRED event snapshot and no TRAIN observations |
| Data-quality valid flag and reason count | Constant in the fixed degraded-reference corpus |

The news flag is selected only because the accepted SEC snapshot establishes
source coverage and both active-event and observed-no-event values occur in
TRAIN. The macro flag remains null: zero is never substituted for unavailable
ALFRED coverage. Any later source addition requires a new point-in-time event
snapshot, dataset build, and dataset identity.

## Sample admission

A training loader must use the readiness report's exact feature indices. A row
is eligible only when:

1. its split is the immutable dataset split;
2. the chosen label is `VALID`;
3. every selected normalized feature is non-null;
4. its degradation reasons are a subset of the report allowlist; and
5. its dataset, readiness, feature schema, universe, calendar, and source hashes
   match the model-training manifest.

Rows crossing a corporate action remain invalid. Missing target bars, the
2025-03-10 source-wide gap, warm-up periods, and unavailable future calendar
endpoints remain missing and are never imputed as labels.

Because the model is pooled, admission requires at least 100,000 complete
TRAIN rows and 50 contributing symbols at each horizon, plus nonzero complete
VALIDATION and TEST rows. The real corpus has 1,752,982 to 1,915,483 qualifying
TRAIN rows and 71 or 72 contributing symbols per horizon. `AIRJ`, `CBRS`,
`ENLT`, `FRVO`, `SPCX`, and `XE` have no qualifying TRAIN rows under the fixed
chronological split; `TDY` additionally has none at 30m and 60m. This does not
block pooled training, but it forbids a per-symbol training claim. Later
inference must still abstain whenever a ticker lacks a complete current feature
vector or compatible pooled-model support.

## Status meanings

- `READY_FOR_INFRASTRUCTURE_VALIDATION`: deterministic offline model training is
  allowed under the exact feature mask and restrictions.
- `BLOCKED`: training is prohibited. Blocking reasons include dirty source,
  unverified objects, lineage mismatch, leakage, insufficient label coverage,
  an absent, expired, or mismatched source approval, a missing core feature,
  insufficient pooled rows/symbols, an empty
  validation/test horizon, or an unreviewed data-quality reason.

Readiness never means profitable, production-ready, or live-trading-safe.

Training reads only retained local objects and does not call Alpaca or require
an active API subscription. The report nevertheless binds the exact Alpaca
approval and source-policy hashes carried by the authenticated backfill.
Owner-only academic-use, no-redistribution, and reassessment restrictions still
apply to the retained data and every derived artifact.

## Command

```bash
export AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts
export PYTHONPATH=python/research:python/contracts:python/intelligence:python/training:python/model_serving:.

# Bind the exact accepted event-aware dataset; multiple immutable generations
# may coexist and are never selected implicitly.
MANIFEST=/scratch/djy8hg/aegis_mx_poc_data/datasets/feature-poc/manifests/499bfe4294d362e78d76644ac47e0064d3cbd73c9260eb6063d0cfd8ce25a22b.json

# Inspect without publishing.
$AEGIS_PYTHON_ENV/bin/python tools/check_training_readiness.py \
  --manifest "$MANIFEST"

# Re-hash every object and publish the accepted report.
$AEGIS_PYTHON_ENV/bin/python tools/check_training_readiness.py \
  --manifest "$MANIFEST" \
  --execute
```

See [ADR 0061](../adr/0061-training-admission-with-degraded-source-evidence.md)
and the [training-readiness runbook](../operations/training-readiness.md).
