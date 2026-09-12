# Point-in-time instrument reference data

## Boundary

`aegis_mx_research.reference_data` is an offline, bounded reference-data layer
for the forecasting POC. It consumes the authoritative `ticker.txt` snapshot
and already retained source evidence. It performs no network access and has no
risk, OMS, router, gateway, credential, or live-activation dependency.

The source disposition is deliberately asymmetric:

| Evidence | Permitted use | Completeness |
| --- | --- | --- |
| Retained Alpaca asset observations | Current symbol, provider asset identity, broad asset class, listing venue | Current-only; no listing date, symbol history, subtype, or action history |
| Retained Alpaca calendar response | Exact sessions and early closes in the backfill range | Retrospective; weekday closure names and halt history unresolved |
| Nasdaq Trader directories | None in this implementation | Persistent automated use is not authorized |
| SEC current ticker mappings | None in this implementation | Not authoritative historical symbology |
| Synthetic fixtures | Contract and conflict testing | Never represented as observed market truth |

## Identity and temporal semantics

An instrument identity is stable across symbol changes and ticker reuse only
when an authorized source supplies a stable provider instrument key. The
Alpaca-derived identity is compatible with the existing minute canonicalizer.
The provider key itself is not written to the reference artifact.

Every revision has two independent dimensions:

- `BusinessInterval` or an action effective timestamp states when the fact is
  economically effective.
- `ReferenceProvenance.available_at_ns` is the maximum of local observation,
  processing, and revision times. Source publication time is optional and
  remains absent when the source did not provide it.

A query filters by both `effective_at_ns` and `known_at_ns`. An overlapping
mapping to different instruments, conflicting calendar state, broken revision
sequence, missing calendar date, unknown schema, bad digest, inexact default
adjustment, or integer overflow fails closed. Symbol lookup never follows a new
symbol automatically.

## Corporate actions

`ExactRatio` stores reduced positive integer terms. A split stores new shares
per old share. Expressing a pre-split price on a post-split basis applies the
reciprocal. The default rejects an inexact integer result; callers must name
`FLOOR` or `HALF_EVEN` to permit rounding. Cash dividends are integer currency
nanos plus an explicit currency and are returned separately for total-return
label construction.

No corporate-action records were available from the approved Prompt 50/51
source. An empty action vector therefore means `coverage unavailable`, not
`no actions occurred`; the resolution report is not safe for historical
training.

## Calendars and halts

The snapshot includes every date between the first and last retained session.
Recorded sessions have exact UTC boundaries. Structural weekends are explicit;
unclassified weekday closures are not relabeled as holidays without an
authorized source. Early-close status follows the recorded close before 16:00
America/New_York. `halt_coverage_complete=false` prevents consumers from
interpreting an empty halt vector as complete evidence.

The authoritative contracts are the
[point-in-time data contract](point-in-time-data-contract.md),
[forecast universe contract](forecast-universe.md), and
[horizon contract](forecast-horizons.md). The decision is recorded in
[ADR 0054](../adr/0054-bitemporal-reference-data-with-current-only-source-evidence.md).
