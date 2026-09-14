# ADR 0045: Bounded minute-data forecasting proof of concept

- Status: Accepted
- Date: 2026-09-09
- Owners: research, intelligence, model-contracts, model-serving, data governance
- Storage values superseded by: [ADR 0062](0062-expand-bounded-poc-data-root-to-800-gb.md)

## Context

Aegis-MX has canonical model forecasts, an off-hot-path time-series service,
deterministic reference baselines, an in-memory point-in-time research store,
leakage checks, signed model-registry primitives, and synthetic event-level
training and backtesting. It does not have an authorized historical minute-data
repository, persistent analytical point-in-time storage, provider-specific
market/news/macro ingestion, exchange-calendar-aware forecast horizons, a
twelve-horizon equity dataset, or trained price-return forecast artifacts.

The user has a 250 GB administrative allocation and wants to validate the
research workflow before buying or reserving substantially more storage. Broad
tick, quote, options, daily-bar, international, and news-archive acquisition
would expand cost, licensing risk, and temporal complexity before the central
data-to-forecast claims have been tested. The existing `horizon_ns` contract
also treats a horizon as elapsed time; it cannot accurately express trading
minutes and session endpoints across closes, weekends, holidays, and early
closes.

## Decision

1. The POC universe is loaded only from `/scratch/djy8hg/xtrade/ticker.txt`.
   Symbol count is observed and reported but never hardcoded. Source order is
   retained, canonical sorted content is hashed, and unresolved instruments
   remain explicit.
2. Historical price input is limited to authorized one-minute OHLCV for regular
   sessions and an approved two-year window. Longer-period summaries are derived
   from these minutes. Separate daily bars, quotes, ticks, depth, options,
   extended hours, and international expansion are excluded.
3. The primary target is return in integer PPM. Implied future integer prices
   are derived outputs with checked arithmetic and complete provenance; raw
   price prediction is not the claimed modeling objective.
4. Forecast horizons are 5, 10, 15, 30, 60, 120, and 300 eligible trading
   minutes plus 1, 5, 10, 21, and 42 trading-session endpoints. A versioned
   `HorizonSpec` and explicit target timestamp become authoritative. A scalar
   elapsed-nanosecond field alone is insufficient for session semantics.
5. Reference data, calendars, corporate actions, SEC material, GDELT metadata,
   and selected FRED/ALFRED vintages are bounded point-in-time inputs. Missing
   optional sources reduce coverage; no missing market or event record is
   fabricated.
6. All provider access defaults disabled and requires a reviewed source policy
   proving storage, research, training, derived-data, retention, attribution,
   timestamp, and user-classification permissions for the exact dataset.
7. Storage uses decimal-byte accounting with an 80 GB target, 100 GB hard
   data-root limit, 20 GB temporary-workspace limit, and 50 GB required reserve.
   The 250 GB user allocation is an external administrative ceiling. Unknown
   quota or projected peak usage blocks remote mutation, and no automatic
   deletion is allowed.
8. Canonical data and derived datasets are streamed, immutable,
   content-addressed, point-in-time, checksummed, and atomically manifested.
   Training and evaluation use chronological partitions and a 42-session
   purge/embargo with seed `20260831`.
9. Learned models are pooled across instruments and horizons and are compared
   with deterministic baselines. Artifacts bind dataset, features, code,
   dependencies, normalization, calibration, OOD, metrics, hashes, signatures,
   and expiry. The bounded sequence permits registry progression only through
   offline or replay validation.
10. The entire POC is offline or asynchronous advisory computation. It has no
    dependency on risk, OMS, routing, gateways, exchange credentials, or live
    activation. It cannot send or authorize an order and cannot establish a
    profitability or production-readiness claim.

The normative details are in the
[forecasting POC contract](../architecture/forecasting-poc-contract.md).

## Consequences

- The first implementation dependency is an immutable universe and
  exchange-calendar horizon contract, not a downloader or model.
- Provider choice and full backfill remain blocked until rights and timestamp
  semantics are approved for the exact source and user classification.
- The POC can fit inside a bounded allocation only if every operation performs
  projected-peak admission and streaming partitioned processing.
- Recently listed, unsupported, delisted, OTC, foreign, or otherwise ambiguous
  symbols may abstain. Their visibility is a correctness requirement.
- Minute bars support a useful forecasting-infrastructure evaluation, but they
  cannot validate tick-level fill, queue-position, NBBO, or passive-execution
  claims.
- Existing `ModelForecast` readers require a reader-first additive migration
  before session-aware horizon writers rely on new fields. Existing return
  fields and `horizon_ns` meaning cannot be changed in place.
- Existing time-series reference adapters, synthetic microstructure models, and
  event backtester remain valid in their documented scopes; none is evidence
  that this POC has trained or economically useful forecasts.
- Scaling beyond this POC requires measured storage/performance evidence and a
  fresh rights, cost, coverage, and architecture decision.

## Rejected alternatives

- Downloading separate minute and daily datasets was rejected because it spends
  storage, creates adjustment/parity ambiguity, and is unnecessary when session
  summaries can be derived deterministically.
- Starting with full tick, quote, depth, options, extended-hours, or global-news
  data was rejected because it exceeds the proof-of-concept question and adds
  licensed semantics that the repository does not possess.
- Treating `1d`, `1w`, or `1mo` as fixed wall-clock nanoseconds was rejected
  because exchange closures and early closes make the result temporally wrong.
- Hardcoding the observed 79-symbol count was rejected because the file is the
  authority and may change.
- Treating shared-filesystem free capacity as the user quota was rejected
  because it can authorize writes beyond an administrative allocation.
- Selecting a free provider by convenience was rejected because access does not
  establish persistent-storage, training, derived-data, or retention rights.
- Training one complex model per ticker was rejected for the initial POC because
  sparse histories, recent listings, reproducibility, and resource cost favor a
  pooled baseline-first design.
- Connecting forecast output to paper or live order entry was rejected because
  this POC evaluates research infrastructure only and must remain incapable of
  transmitting orders.

## Compatibility and migration

The horizon contract must be added under the existing
[schema evolution policy](../../schemas/schema-evolution-policy.md). Existing
fields, enum values, and wire meanings remain unchanged. New readers deploy and
prove compatibility before new writers publish a session-aware horizon. During
rollback, those writers stop before readers that do not understand the new
semantics. Stored source objects and accepted datasets are immutable; migrations
produce new content-addressed artifacts linked to their predecessors.
