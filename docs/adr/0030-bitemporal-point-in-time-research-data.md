# ADR 0030: Bitemporal point-in-time research data

- Status: Accepted
- Date: 2026-08-31

## Context

Research records have at least two independent temporal meanings: when a fact
was effective in the world and when Aegis-MX could have known it. Overwriting a
macro release with a later revision, applying a split before its effective
date, using current index constituents historically, or carrying a post-event
estimate into a feature window creates look-ahead or survivorship bias while
leaving superficially plausible results.

The existing earnings, macro, news, filing, options, and market-specialist
contracts already retain local provenance. Research and backtesting also need a
provider-neutral common store and dataset gate that applies the same temporal
rules across record families.

## Decision

`python/research/aegis_mx_research` is an offline-only, append-only reference
implementation. Every immutable record contains event, publication, receive,
processing, and revision wall-clock UTC nanoseconds; a half-open business
validity interval; exact source/content identity; schema version; and contiguous
record version.

Knowledge availability is the later of processing and revision time.
`as_known_at(t)` includes revisions available at `t` and
`latest_available_before(t)` is strict. These knowledge queries do not imply
business effectiveness. `membership_at` and `symbol_mapping_at` additionally
require the requested timestamp to fall inside the record's validity interval
and accept an explicit knowledge cutoff.

Corrections never overwrite earlier records. News corrections and filing
amendments must link to the immediately prior accepted revision. Other vintage
families require contiguous versions and strictly increasing revision time.
Unknown schema versions, malformed timestamps, broken lineages, duplicate IDs,
or capacity exhaustion reject the append.

Dataset manifests bind feature and label windows, partitions, exact source
record IDs, historical universes, split method, embargo, and any random seed.
The leakage validator rejects future macro revisions, future constituents,
post-event estimates, prematurely applied corporate actions, survivorship bias,
feature/label or cross-split label overlap, nonchronological partitions, missing
provenance, and randomized time-series splitting. One violation rejects the
whole manifest with stable reason codes.

## Consequences

- Point-in-time and effective-time questions remain distinct and reproducible.
- Corrections, amendments, delistings, and old symbols remain queryable after a
  newer version exists.
- Random sampling may still be used inside a training algorithm when justified
  and seeded, but it cannot assign time-series observations to train/test
  partitions.
- The reference store allocates and scans in Python. It is suitable for
  research, validation, and replay orchestration, never the execution hot path.
- Provider adapters, production timestamp semantics, entitlement, retention,
  and correction identifiers remain licensed integration boundaries.
