# ADR-0008: Bounded single-writer order books

- Status: Accepted
- Date: 2026-08-29
- Decision owners: Order Book and Edge Core
- Governing contract: [Engineering Contract](../architecture/engineering-contract.md)

## Context

The validated feed path needs a deterministic state machine that supports both
native order-by-order data and canonical aggregate updates. The book must expose
queue order, depth, top-of-book, status, recovery state, and stable snapshots
without allocation or synchronization in its update path.

Canonical v1 `BookUpdate` deliberately has no venue order ID. SMX/1 retains a
license-clean native order ID, but its aggregate snapshot cannot recreate order
priority. Treating an aggregate recovery image as an order-by-order image would
fabricate queue state.

Channel sequences can interleave instruments. A per-instrument book can retain
the last sequence it consumed, but feed-level continuity remains the feed
handler's responsibility.

## Decision

Each `OrderBook` is bound to its construction thread and has one writer. Readers
on another thread consume copies published by a future bounded handoff; they do
not access mutable book storage. Wrong-thread calls return an explicit error and
do not mutate validity.

The engine preallocates two bounded storage images during construction:

- a fixed order pool and open-addressed order-ID index;
- sorted, contiguous bid and ask level arrays;
- intrusive FIFO order queues at each price; and
- an active image plus a staging image for validated snapshot replacement.

No update, depth query, invariant check, clear, reset, or consolidated query
allocates. Initialization-time construction and book registration may allocate
their fixed storage images.

Books start `RECOVERING` by default. Only explicit recovery completion or a
validated snapshot produces `VALID`. Any malformed, stale, inconsistent,
capacity-exhausted, aggregate-mismatched, or impermissibly crossed update makes
the book `INVALID`. An invalid book accepts no ordinary update; a session reset,
corporate-action reset, or validated snapshot is required.

Order replacement always loses queue priority, including quantity reductions.
This is a deterministic internal rule, not a claim about any licensed venue.
Trades update tape state and never implicitly mutate depth. Halts and closed
status reject ordinary book and trade mutations while permitting status,
auction, and explicit clear operations.

Per-book sequencing requires a strictly newer sequence under modular ordering.
Contiguous checking is optional for dedicated per-instrument streams. Shared
channel gaps are detected before dispatch by the feed handler. Snapshots bind
identity, session, mode, sequence, version, orders, levels, status, auction,
tape state, and priority metadata with a deterministic hash. Order-by-order
snapshot loading requires every live order and verifies derived aggregates
against supplied levels before swapping the staging image into service.

`BookUniverse` enforces duplicate live order IDs across all instruments in the
same venue/session and builds consolidated depth from valid constituents only.
It fails closed if a constituent is invalid or the consolidated view crosses.

Corporate actions never perform implicit tick or quantity rescaling. They clear
state, bind a new reference-data version and session, and return the book to
`RECOVERING`; adjusted reference data and a new snapshot must follow.

## Consequences

Capacity, level insertion, clear, and invariant work are bounded but not always
constant-time. Sorted arrays favor cache locality and predictable shallow-book
performance; inserting a new level shifts a bounded suffix. Open-addressed
lookup avoids tree allocation, while tombstones can increase probe length over
a long session and must be covered by capacity benchmarks.

The mutable engine is deliberately not a multi-reader object. Immutable
cross-thread publication and replay integration remain separate downstream
work. Aggregate SMX/1 recovery is valid for price-level books only; an
order-by-order recovery source must provide an authorized order-bearing image.

## Alternatives considered

`std::map` and `std::unordered_map` were rejected for the hot path because node
allocation, allocator behavior, pointer chasing, and rehashing are not bounded
enough. A lock-protected shared book was rejected because ownership transfer and
reader contention would enter the update path. Reconstructing synthetic queue
priority from aggregate snapshots was rejected as unsafe fabrication.

## Review triggers

Revisit this ADR before adding multiple writers, lock-free readers, venue-specific
priority semantics, crossed transition rules, dynamic capacity growth, aggregate-
to-order recovery, or corporate-action arithmetic.
