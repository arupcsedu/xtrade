# ADR 0063: Tri-state point-in-time feature events

| Field | Value |
| --- | --- |
| Status | Accepted |
| Date | 2026-09-14 |
| Scope | Offline forecasting POC feature datasets |

## Context

The Prompt 57 feature schema reserved `news_event_flag` and
`macro_event_flag`, but the accepted real dataset was built with an empty event
snapshot. Both columns consequently contained only zero. Zero was ambiguous:
it could mean that the source was observed and no event was active, or that no
source was loaded at all. Training admission excluded both columns, correctly,
because they had no variance and no authenticated source coverage.

The accepted SEC corpus contains official filing acceptance timestamps and can
provide point-in-time issuer-disclosure events. The ALFRED adapter can provide
date-precision release records, but only after its independent authorization,
credential, and complete-series gates pass. GDELT metadata does not assert a
historical publication timestamp and must not be treated as if it did.

## Decision

Event flags are tri-state integer features:

- `1`: an authenticated event is active at the feature cutoff;
- `0`: source coverage includes the cutoff and no event is active; and
- `null`: no authenticated source coverage includes the cutoff.

Every event snapshot binds the resolved universe, source-request universe,
source reports, source manifests, coverage intervals, event records, source
hashes, availability times, and validity windows in a self-hashed immutable
document. Feature-dataset identity includes both the snapshot hash and explicit
per-family coverage.

For the POC, an SEC `8-K`, `10-Q`, `10-K`, `6-K`, or `20-F`, including an
amendment, activates the legacy `news_event_flag` for 24 elapsed hours beginning
at the official SEC acceptance timestamp. Here, “news” means an authenticated
issuer disclosure; it does not claim publisher-news coverage or sentiment.

An ALFRED release activates `macro_event_flag` for 24 elapsed hours beginning at
its conservative `known_at_utc_ns`, which is the next UTC day for date-only
availability. This is safe for offline daily-context infrastructure validation,
but it is not an authoritative same-day intraday macro-release timestamp.

Training readiness evaluates NEWS and MACRO coverage independently. A covered,
nonconstant flag may be selected. An uncovered or constant flag remains
excluded. Existing dataset manifests remain readable: legacy empty snapshots
remain unavailable, while a legacy nonempty combined snapshot retains its prior
interpretation.

Calendar-horizon targets are independent of instrument identity, so the
single-owner offline builder caches their resolved timestamp or reason code by
feature cutoff. The cache is explicitly bounded by the aggregate-point limit.
Prior-session summaries are indexed once per instrument/session rather than
reconstructed for every minute. These optimizations preserve output hashes and
do not add concurrency or nondeterministic eviction.

## Consequences

- Missing event data can no longer masquerade as an observed no-event state.
- The SEC-backed issuer-event feature can be used without GDELT credentials or
  publisher full text.
- The macro feature remains unavailable until the complete approved ALFRED
  source is actually ingested.
- The binary features carry event presence only. Event type, materiality,
  direction, novelty, surprise, and decay require separately versioned features.
- Repeated full-dataset builds avoid redundant calendar resolution while
  retaining bounded memory and identical output identity.
- The 24-hour validity window is a POC hypothesis that must be evaluated against
  zero-event and shorter/longer-window baselines; it is not an economic claim.

## References

- [Point-in-time feature events](../architecture/point-in-time-feature-events.md)
- [Leakage-safe feature datasets](../architecture/leakage-safe-feature-datasets.md)
- [Training admission](0061-training-admission-with-degraded-source-evidence.md)
- [SEC ingestion decision](0055-bounded-sec-edgar-ingestion.md)
- [ALFRED ingestion decision](0057-bounded-alfred-vintage-and-date-only-availability.md)
