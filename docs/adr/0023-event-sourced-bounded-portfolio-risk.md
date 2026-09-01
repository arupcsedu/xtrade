# ADR 0023: Event-sourced bounded portfolio risk

- Status: Accepted
- Date: 2026-08-30
- Owners: Risk, OMS, edge-core, replay

## Context

Pre-trade risk needs current positions, pending quantities, P&L, and exposure
without a network request. Fills can arrive through the primary order path and
an independent drop-copy path; either path can duplicate, correct, or bust a
logical execution. Average-cost accounting is path dependent, so reversing a
late correction against only aggregate totals is not deterministic. The edge
also needs bounded runtime and memory, immutable snapshots, journal recovery,
and an explicit unsafe state when sources disagree.

Provider execution identifiers and corporate-action semantics are licensed or
broker-specific and are not present in the repository.

## Decision

The risk boundary owns one single-writer, fixed-capacity
`PortfolioRiskService`. It consumes a synthetic normalized `PortfolioEvent`
contract and appends every result to a bounded hash-chained local journal.
Logical fills are keyed by `GlobalEventId`; receipt events have independent
idempotency keys. The first matching source changes accounting, a matching
second source only completes reconciliation, and conflicting economics move
the service to `UNSAFE` without double application.

Normal fills update one position incrementally using signed integer quantity,
signed integer open cost in currency nanos, and checked integer arithmetic.
Corrections and busts are exceptional near-real-time operations: the affected
position is rebuilt in original application order from a fixed fill ledger.
This preserves average cost and realized P&L deterministically without dynamic
allocation. A fully resolved split ratio may establish a new accounting
baseline. A later correction of a pre-action fill is not guessed; it requires
external reconciliation.

Every state-changing event produces a versioned fixed-layout
`PortfolioSnapshot` and publishes it through the existing immutable snapshot
store. Readers pin a checksum-validated version. Pre-trade risk imports one
complete healthy snapshot under its local writer gate; it never calls the
portfolio service or a remote system while evaluating an intent. Snapshot
sequence rollback or same-sequence hash conflict is rejected.

Canonical FlatBuffers schema v1.7 additively extends `PositionSnapshot` with
account identity, journal/portfolio sequences, open cost, pending quantity,
portfolio exposure totals, drawdown, readiness, health, invariant reason, and
the snapshot hash. One canonical record represents one
account/strategy/instrument row; portfolio totals repeat identically for every
row in the same snapshot sequence.

## Consequences

- Ordinary event processing, snapshot publication, and pre-trade reads allocate
  no memory after construction and have finite capacity.
- Journal exhaustion, arithmetic overflow, source conflict, missing marks, and
  unresolved orphan fills inhibit readiness.
- Correction cost is bounded by fill-ledger capacity and is intentionally not
  part of the innermost pre-trade path.
- A single pre-trade engine currently accepts a one-account portfolio snapshot.
  Multi-account portfolio aggregation is maintained, but account-specific
  pre-trade engines must receive account-scoped views rather than mixed totals.
- Real broker drop-copy keys, bust/correction lifecycle rules, fractional-share
  handling, tax lots, FX conversion, derivatives valuation, and corporate-action
  rounding remain licensed integration boundaries.

## Rejected alternatives

- Applying both primary and drop-copy fills and reconciling totals later permits
  transient duplicate risk and is unsafe.
- Reversing a correction from aggregate average cost loses path information.
- Reading PostgreSQL, Kafka, or a portfolio RPC during pre-trade evaluation
  violates the hot-path contract and makes failure behavior unbounded.
- Floating-point money or average prices make replay dependent on evaluation
  order and platform details.

