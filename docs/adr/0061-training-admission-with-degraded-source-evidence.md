# ADR 0061: Training admission with degraded source evidence

- Status: Accepted for the bounded offline POC
- Date: 2026-09-13
- Scope: Prompt 57 datasets admitted to Prompt 58 training
- Live-trading capability: None

## Context

The real Prompt 57 build completed with 7,684,385 feature rows and a leakage
result of `PASS`. Every required horizon has valid labels for all 78 resolved
exchange-listed symbols. `KRKNF` is an explicitly unsupported OTC instrument.

The dataset intentionally retains limitations that cannot be repaired from the
authorized Alpaca Basic IEX history: current-cohort reference semantics,
incomplete historical halt evidence, unobserved original publication times,
and one complete source gap on 2025-03-10. The build also had no accepted
point-in-time sector, GDELT, or ALFRED input. Sector columns are null and the
news/macro columns are constant zero. Treating those columns as observed input
would fabricate evidence.

## Decision

Introduce a fail-closed, immutable training-readiness report between dataset
publication and training. Admission requires:

- a clean Git worktree and exact hashes for the builder sources and dependency
  lock;
- valid self-hashes for the backfill, canonical promotion, and feature dataset;
- an authenticated, unexpired private source approval whose hash, source/feed,
  timeframe, academic purpose, internal-only distribution, data root, universe,
  and model-training authorization match the backfill and assessment time;
- SHA-256 and size verification for every feature and summary object;
- exact canonical-partition lineage;
- leakage `PASS`;
- the complete ticker/horizon matrix;
- at least 100 valid labels for every resolved symbol and horizon; and
- the required core price, volume, volatility, session, market-relative, and
  rolling-session features with nonzero TRAIN variance.

Label coverage alone is insufficient. A streaming Parquet pass must also prove
at least 100,000 rows and 50 contributing symbols per horizon after
intersecting valid labels with the complete selected feature vector. Every
horizon must retain nonzero VALIDATION and TEST intersections. The pass rejects
mixed ticker/split objects, wrong label order/cardinality, unknown degradation
reasons, and record-count mismatch.

Feature eligibility is deterministic. A feature is excluded if it has no TRAIN
observations, zero TRAIN variance, or depends on an empty sector/event snapshot.
For the current dataset this excludes sector returns, news and macro flags, and
the two constant data-quality columns. Training consumes only the published
feature indices and drops a sample when a selected feature or selected label is
null. Unsupported symbols abstain.

The fixed chronological split leaves six recent or sparse symbols with no
complete TRAIN contribution, and leaves `TDY` without a 30m or 60m TRAIN
contribution. This is not repaired through shuffling or future leakage. The
pooled corpus still has 71 or 72 contributing symbols and more than 1.75
million qualifying TRAIN rows at every horizon. These symbols cannot support a
per-symbol fitted-model claim; later inference remains subject to feature and
OOD abstention.

Explicit missing labels and action-crossing invalid labels are not filled.
They are acceptable for infrastructure validation only when every resolved
symbol/horizon exceeds the minimum count. Unknown degradation codes remain
ineligible. The current known degradation allowlist is recorded in the report
and cannot be expanded by a model.

## Consequences

Prompt 58 may validate deterministic training, export, inference parity,
calibration, OOD, and artifact lifecycle without waiting for optional sector,
news, or macro sources. It may not claim predictive economic value,
survivorship-bias-free evaluation, consolidated-market coverage, or production
readiness.

Adding a sector/event source produces a new dataset identity and a new
readiness report; it does not mutate this dataset. A paid Alpaca subscription is
not required to train from the already accepted local dataset, and training
performs no provider API call. The exact source approval and policy hashes
remain binding, and expiry is re-evaluated for each admission. Commercial use,
SIP use, OTC coverage, redistribution, or
broader retention requires an independent source and rights review.

## Evidence

- [Training-readiness contract](../architecture/training-readiness.md)
- [Leakage-safe feature datasets](../architecture/leakage-safe-feature-datasets.md)
- [ADR 0060](0060-now-known-reference-evidence-for-the-bounded-poc.md)
- [Forecasting POC quality gates](../testing/forecasting-poc-quality-gates.md)
