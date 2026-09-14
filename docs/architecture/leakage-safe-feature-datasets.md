# Leakage-safe feature and label datasets

## Scope and safety state

The Prompt 57 builder is offline-only. It reads immutable canonical one-minute
partitions for resolved entries in `ticker.txt`, derives features and labels,
and writes training artifacts. It has no dependency on risk, OMS, router, or a
gateway. It cannot submit an order and makes no economic-value claim.

The real-data build uses the fixed current-universe cohort semantics in
[ADR 0060](../adr/0060-now-known-reference-evidence-for-the-bounded-poc.md).
Current mappings are not represented as historically known. Records remain
degraded for incomplete halt and historical membership coverage, so the POC
may validate infrastructure but cannot establish survivorship-bias-free model
value.

Accepted Prompt 56 partitions are consumed by `aegis-real-features`. The
command is dry-run by default, verifies every partition hash and schema,
requires its record count to match the promotion report, and publishes the
feature dataset manifest last. It performs no network access.

## Inputs and trust boundaries

- The ticker universe is bound by its source-file and universe-snapshot
  SHA-256 values.
- Each Prompt 56 Parquet input must be a regular non-symlink file whose SHA-256,
  Arrow metadata, schema version, row count, and uniform Zstandard compression
  are verified before use.
- Calendar endpoints come only from the immutable `ExchangeCalendar` contract.
- Sector revisions are bitemporal and require both business effectiveness and
  availability at the feature knowledge cutoff.
- Cross-sectional market and sector aggregates carry the latest processing
  time of every contributing bar. A row may use an aggregate only when all of
  its contributors were available by that row's knowledge cutoff.
- News and macro inputs are advisory flags. Only revisions available by the
  exchange-minute replay cutoff can contribute; the later bulk-download
  processing time cannot reveal future event revisions. Document text is never
  evaluated here.
- Structural corporate actions are used only to invalidate crossing labels;
  they are not predictive features.

## Bounded derivation

The builder uses repeatable multi-pass reads. At most one configured instrument
history (default cap: 1,000,000 rows) is resident while features and future
endpoint lookups are calculated. Aggregate state is capped at 5,000,000
minute/sector points. Parquet output is emitted in bounded row groups and
content-addressed by SHA-256.

Feature order is fixed in Parquet metadata:

1. 1, 5, 15, 30, and 60 trading-minute returns in PPM;
2. integer share volume and 20-minute relative volume;
3. 30-minute realized volatility and bar high-low range in PPM;
4. session gap, minute index, and session position;
5. market/sector return and relative return;
6. prior-five-session return and volume statistics;
7. point-in-time news and macro flags; and
8. canonical data-quality flags.

Unavailable rolling or cross-sectional inputs remain null and make the row
`WARMUP` or `DEGRADED`; NaN and infinity have no representation.

## Labels and temporal rules

The required labels are `5m`, `10m`, `15m`, `30m`, `60m`, `2h`, `5h`, `1d`,
`1w`, `2w`, `1mo`, and `2mo`. Intraday targets count eligible regular-session
minutes; session horizons select future session closes. A label is valid only
when its target row exists, is strictly later than the feature cutoff, and has
the same tick grid and corporate-action lineage. No missing target is filled.

Chronological splits are 70/15/remaining percentages after removing two exact
42-session purge intervals. Seed `20260831` is recorded, although split
membership itself is deterministic and does not use random shuffling.
Normalization uses integer count, sum, and sum-of-squares values from TRAIN
only. VALIDATION and TEST never alter fitted state.

## Storage and acceptance

The builder estimates output at 1,024 bytes per canonical input row plus fixed
metadata overhead. Admission applies the repository's 800 GB hard root limit,
20 GB temporary limit, authoritative quota, and 50 GB free reserve before any
write. An output that exceeds its admitted estimate fails without a manifest.

Feature Parquet is partitioned by split and stable instrument ID. Session
summaries are derived only from minute records and stored separately under
`derived/`; they are not a second vendor daily dataset. The self-hashed dataset
manifest records all object hashes, source partition IDs, split membership,
normalization statistics, per-ticker/per-horizon coverage, and leakage result.
It is published last.

## Accepted real build

The 2026-09-13 verification closed the earlier zero-canonical-data blocker:
4,000 accepted canonical partitions yielded 7,684,385 feature rows, 36,520
session summaries, and 306 immutable objects. The leakage scan passed every
sample. All 78 resolved symbols have at least 100 valid labels for each of the
twelve horizons; unsupported OTC symbol `KRKNF` remains an abstention.

The training gate selects the 16 observed, nonconstant price, volume,
volatility, session, market-relative, and rolling-session features. It excludes
the unavailable sector columns and constant empty event/data-quality columns;
zero event flags are not treated as affirmative no-event evidence. See the
[training-readiness contract](training-readiness.md) and the
[real coverage report](../testing/forecasting-poc-label-coverage.md).

Training admission additionally scans all feature Parquet in bounded batches.
After intersecting complete selected features with valid labels, every horizon
has more than 1.75 million TRAIN rows from at least 71 symbols and nonzero
VALIDATION and TEST rows. Six resolved symbols do not contribute complete
TRAIN rows under the fixed chronological split; that limitation is preserved
rather than hidden through randomized splitting or synthetic history.

See [ADR 0059](../adr/0059-multi-pass-leakage-safe-feature-datasets.md), the
[horizon contract](forecast-horizons.md), the
[point-in-time contract](point-in-time-data-contract.md), and the
[recovery runbook](../operations/feature-dataset-recovery.md).
