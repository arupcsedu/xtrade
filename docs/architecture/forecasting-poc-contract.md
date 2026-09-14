# Aegis-MX bounded forecasting POC contract

| Field | Value |
| --- | --- |
| Status | Accepted and normative for the forecasting POC |
| Effective date | 2026-09-09 |
| Scope | Prompts 45 through 65 |
| Execution class | Offline research and asynchronous model serving |
| Trading capability | None; PAPER integration remains advisory only |
| Governing contract | [Aegis-MX engineering contract](engineering-contract.md) |
| Decision record | [ADR 0045](../adr/0045-bounded-minute-data-forecasting-poc.md) |

## Purpose

This contract defines a storage-bounded proof of concept for point-in-time,
multi-horizon equity forecasting. The POC is intended to validate data
engineering, leakage controls, reproducible training, forecast publication, and
honest evaluation before more storage or licensed data is purchased. It does
not establish predictive economic value, production readiness, or trading
authorization.

This contract inherits the [point-in-time data contract](point-in-time-data-contract.md),
[time-series service boundary](timeseries-forecast-service.md),
[licensed integration boundaries](licensed-integration-boundaries.md), and
[quality gates](../testing/quality-gates.md). A later prompt or implementation
may narrow this scope but cannot silently weaken its safety, provenance,
licensing, temporal, or storage rules.

## Binding universe

The only source of requested forecast symbols is:

```text
/scratch/djy8hg/xtrade/ticker.txt
```

The universe loader must parse this file on each explicit snapshot operation;
the symbol count must never be compiled or hardcoded. Empty lines may be
ignored. Source order is retained for operator reports, while a canonical sorted
representation is used for hashing and deterministic processing. Duplicate,
malformed, overlength, or ambiguous symbols fail validation. Unsupported,
unresolved, delisted, and insufficient-history symbols remain visible with
machine-readable reason codes; they are never silently discarded or replaced.

The 2026-09-09 audit observed 79 unique, non-empty, syntactically valid symbols.
The source file SHA-256 at audit time was
`c4bfd957725741d76f847405294258f4a8d31d16dff58b0a7c033bafbb446f79`.
These are audit observations, not constants. A universe snapshot binds the
actual source digest, ordered symbols, resolved `InstrumentId` values, resolution
status, and its own canonical digest.

## Forecast targets and horizons

The primary modeled target is future return in integer parts per million. A
future price in integer ticks may be derived from the current validated price
and the forecast return using checked, documented rounding. Raw future price is
not the modeled success target, and every display of implied price must retain
the source price, tick scale, corporate-action version, target timestamp, and
return forecast that produced it.

The required forecast horizon set is:

| CLI label | Semantic horizon | Calendar rule |
| --- | --- | --- |
| `5m` | 5 eligible trading minutes | Count regular-session minutes only |
| `10m` | 10 eligible trading minutes | Count regular-session minutes only |
| `15m` | 15 eligible trading minutes | Count regular-session minutes only |
| `30m` | 30 eligible trading minutes | Count regular-session minutes only |
| `60m` | 60 eligible trading minutes | Count regular-session minutes only |
| `2h` | 120 eligible trading minutes | Count regular-session minutes only |
| `5h` | 300 eligible trading minutes | Count regular-session minutes only |
| `1d` | 1 future trading-session endpoint | Use the versioned exchange calendar |
| `1w` | 5 future trading-session endpoints | A trading-session convention, not seven wall days |
| `2w` | 10 future trading-session endpoints | A trading-session convention, not fourteen wall days |
| `1mo` | 21 future trading-session endpoints | A POC convention, not a calendar month |
| `2mo` | 42 future trading-session endpoints | A POC convention, not two calendar months |

Every forecast and label carries both a versioned `HorizonSpec` and an explicit
future target timestamp. Weekends, holidays, overnight closures, and early
closes do not count as trading minutes. Halt treatment is configuration-driven
and explicit. A scalar `horizon_ns` may remain for compatible elapsed-time
consumers but cannot be the authoritative meaning of a session horizon.

## Data scope

### Included

- Authorized one-minute OHLCV observations for the validated universe.
- Regular market sessions only.
- At most the approved two-year POC window for minute data.
- Session summaries derived reproducibly from canonical minute data.
- Bounded instrument reference data, calendars, symbol history, and corporate
  actions needed for point-in-time interpretation.
- Bounded SEC filing metadata, selected forms and structured facts, subject to
  the approved source policy.
- Bounded GDELT metadata relevant to resolved issuers, without assuming rights
  to publisher full text.
- A small, explicitly reviewed set of FRED/ALFRED macro vintages.
- Source, partition, dataset, model, evaluation, and evidence manifests.

### Excluded

- Separately downloaded daily bars.
- Quotes, NBBO, tick trades, order-level feeds, and full-depth books.
- Options data and inferred dealer positioning.
- Extended-hours observations.
- International-market expansion or a broad global-news mirror.
- Unapproved licensed payloads, credentials, proprietary specifications, and
  unrestricted publisher content.
- Tick-level execution, queue-position, passive-fill, or market-impact claims
  derived from minute bars.
- Any live-order, OMS, router, risk, or gateway integration.

Absence of an included optional source reduces feature coverage and may cause
abstention. It never permits fabricated observations or future-informed values.

## Temporal and point-in-time rules

Every source record preserves event, publication, receive, processing, revision,
and validity times as defined by the
[point-in-time data contract](point-in-time-data-contract.md). Provider fields
must not be assigned a timestamp meaning that the provider does not document.
Historical records lacking an actually observed local receipt time must carry an
explicit historical-ingest limitation; receipt latency must not be invented.

Features use only records available by their feature cutoff. Labels begin after
the feature interval and resolve through eligible trading minutes or sessions.
Chronological train, validation, and test partitions use a 42-session purge and
embargo. Future revisions, constituents, estimates, symbol mappings, corporate
actions, news corrections, and macro vintages are prohibited. Missing
provenance or ambiguous temporal semantics rejects the affected sample.

## Storage and resource envelope

All policy quantities are serialized as exact integer bytes. POC-root limits
use decimal GB; the externally imposed scratch quota retains its authoritative
binary value. Reports label decimal and binary presentations separately.

| Control | Limit | Failure behavior |
| --- | ---: | --- |
| Administrative scratch allocation | 10,995,116,277,760 bytes (10 TiB) | Treat the `hdquota -s` soft limit as an external ceiling |
| Target POC data-root footprint | 800 GB | Stop expansion and produce a cleanup plan before exceeding target |
| Hard POC data-root footprint | 800 GB | Refuse new writes or downloads |
| Minimum filesystem reserve | 50 GB | Refuse admission if projected peak would cross the reserve |
| Temporary workspace | 20 GB | Refuse the operation; never spill without a bound |

The recommended external data root is
`/scratch/djy8hg/aegis_mx_poc_data`. Downloaded data must not be committed to
Git. Admission accounts for current data-root bytes, partial objects, temporary
files, outputs, retry overhead, and projected peak usage. Filesystem-wide free
space is not proof of a per-user quota. Unknown quota, current usage, object
size, or projected peak state blocks remote mutation. No component may delete
data automatically to make an operation fit.

The authoritative 2026-09-11 audit used `/opt/rci/bin/hdquota -s` and the
cluster's matching `statvfs` calculation. It reported a 10 TiB soft scratch
quota, 608,990,093,312 bytes used, and 10,386,126,184,448 bytes available at
`2026-09-11T00:12:13.122793Z`. This quota evidence establishes capacity only;
it does not authorize a data source or download.

## Source authorization and licensed boundaries

All remote sources default to disabled. Before a provider-specific adapter or
download is allowed, a reviewed source policy must identify the exact dataset,
API/specification revision, credentials mechanism, user classification,
history/market coverage, timestamp semantics, adjustments, rate limits,
retention, deletion, attribution, persistent-storage rights, model-training
rights, and derived-data rights.

Free access is not equivalent to open data or permission to train. Ambiguous
rights fail closed. Source authorization is separate for market data, SEC,
GDELT, and each FRED/ALFRED series. The unavailable licensed details and
completion classes remain governed by
[licensed integration boundaries](licensed-integration-boundaries.md).

Credentials are supplied only through an approved external secret mechanism.
They must not appear in source control, command arguments, logs, reports,
manifests, exception text, signed URLs, or test fixtures. Unit and normal
integration tests use synthetic, filesystem, or mock providers and perform no
remote access.

## Canonical data and provenance

Canonical minute prices use integer ticks and volumes use integer units. Every
record has explicit time domains, source identity, schema version, data-quality
state, immutable record identity, and content hash. Identical duplicates are
idempotent; conflicting duplicates, impossible OHLC relationships, negative
volume, timestamp disorder, unknown scale, and unresolvable corporate-action
state fail validation or enter quarantine. Missing minutes are never forward
filled and are never replaced with zero-volume or synthetic bars.

Canonical storage is streaming and partitioned to avoid loading the complete
dataset into memory. Accepted partitions and all derived datasets are immutable,
content-addressed, and published atomically. Corrections create lineage; they do
not overwrite evidence.

## Model and inference boundary

Training is pooled across instruments and horizons unless evidence supports a
more complex bounded alternative. Baselines precede learned models. Every model
binds its ordered feature schema, dataset manifest, normalization, code revision,
dependency lock, training configuration, deterministic seeds, calibration, OOD
profile, metrics, artifact digest, signature, and expiry.

Forecasting occurs offline or in the existing asynchronous, off-hot-path model
service. Each ticker/horizon result is either a valid common-contract forecast
or an explicit abstention. Missing history, stale inputs, invalid data,
unsupported instruments, incompatible artifacts, deadline misses, NaN/Inf,
overflow, and failed provenance validation cause abstention or rejection.

The existing deterministic reference adapters validate infrastructure only.
They are not trained POC models and must not be reported as TimesFM or evidence
of predictive value. POC artifacts may advance only through
`OFFLINE_VALIDATED` or `REPLAY_VALIDATED` during this sequence; promotion to
`SHADOW`, `CANARY`, `LIMITED_RISK`, or `PRODUCTION` is outside scope.

## Non-trading invariant

No code delivered by this POC may import, invoke, address, configure, or gain
credentials for risk, OMS, smart routing, exchange gateways, or live activation.
Forecast services report `live_trading_capable=false`. Kubernetes artifacts are
for non-colocated research and model services only. No model sends an order,
and no forecast is an operator recommendation to trade.

## Determinism and evaluation

The POC dataset and training seed is `20260831` unless a phase records a distinct
seed for a justified operation. Source objects, universe, calendar, partitions,
datasets, features, labels, models, forecasts, evaluations, configuration, code,
and environment are content-identified. Repeated deterministic builds,
inference, and evaluation over identical inputs must produce identical canonical
hashes.

Evaluation is chronological and walk-forward. It reports per-ticker,
per-horizon, pooled, sector, and regime evidence with minimum sample thresholds,
calibration, quantile coverage, abstention, OOD, and baseline comparisons.
Insufficient combinations report `INSUFFICIENT_EVALUATION_DATA`. No report may
claim profitability or economic value from raw accuracy, synthetic data, or
minute-bar execution approximations.

## Completion criteria

The bounded POC is complete only when every source symbol remains visible in
coverage evidence; every ticker/horizon yields a validated forecast or explicit
abstention; source authorization and storage admission evidence verify; no
record is fabricated; point-in-time leakage tests pass; dataset, inference, and
evaluation hashes reproduce; signed provenance verifies; storage remains within
the envelope; secrets and untrusted text remain contained; and tests prove there
is no trading-component connection.

The dependency-aware implementation sequence is defined in the
[forecasting POC roadmap](forecasting-poc-roadmap.md), the system flow is shown
in the [forecasting POC data-flow diagram](forecasting-poc-data-flow.mmd), and
phase evidence is governed by the
[forecasting POC quality gates](../testing/forecasting-poc-quality-gates.md).
