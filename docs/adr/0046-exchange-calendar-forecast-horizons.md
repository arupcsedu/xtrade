# ADR 0046: Exchange-calendar forecast horizon semantics

- Status: Accepted
- Date: 2026-09-09
- Owners: model-contracts, research, model-serving, data governance

## Context

`ModelForecast.horizon_ns` expresses an elapsed duration. It cannot identify a
future trading-minute or session endpoint across overnight closures, weekends,
holidays, early closes, or halts. Treating `1d` or `1mo` as a fixed duration
would make labels, forecasts, replay, and evaluation disagree.

## Decision

1. Schema v1.9 additively appends `HorizonSpec` and an explicit target
   `ExchangeEventTimeNs` to `ModelForecast`.
2. POC horizons count eligible regular-session minutes or deterministic future
   regular-session closes from an immutable versioned exchange calendar.
3. Every trading horizon binds a nonzero calendar `ConfigurationVersion` and a
   `REJECT`, `PAUSE`, or `COUNT_SCHEDULED` halt policy.
4. The stored `horizon_ns` is retained unchanged as actual elapsed nanoseconds
   between as-of and target. It is a compatibility value, not the semantic
   definition of a calendar horizon.
5. Legacy builder inputs are encoded explicitly as `ELAPSED_NANOSECONDS` with
   no calendar semantics and a checked derived target.
6. Unknown enums, absent provenance, calendar mismatch, inadequate coverage,
   invalid time grids, inconsistent target/elapsed values, and overflow fail
   closed.

## Consequences

- Online, replay, training-label, and evaluation code can share one target
  resolver and produce identical timestamps.
- Calendar artifacts become required point-in-time provenance and must be
  versioned alongside a dataset or forecast.
- Early closes require no hardcoded duration; their explicit close is selected.
- Halts remain policy decisions rather than silently counted or ignored.
- Older readers may ignore appended fields, so reader-first rollout is required
  before calendar-aware writers publish.
- Existing v1.8 bytes remain readable, and no stored record is modified.

## Rejected alternatives

- Fixed wall-clock durations were rejected because they cross market closures
  and mislabel sessions.
- Encoding trading semantics only in a string label was rejected because labels
  are not sufficient for deterministic validation.
- Replacing `horizon_ns` was rejected as a breaking schema and API change.
- Inferring calendar revisions or halt behavior was rejected because missing
  temporal state must fail closed.

## Rollout and rollback

Deploy v1.9 bindings and semantic readers, validate v1.8 stored fixtures, then
enable v1.9 writers. During rollback, stop calendar-aware writers first. Retain
both v1.8 and v1.9 readers for the regulated replay window. Details are in the
[forecast horizon contract](../architecture/forecast-horizons.md) and
[schema evolution policy](../../schemas/schema-evolution-policy.md).
