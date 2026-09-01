# Incremental feature engine

The C++ feature engine consumes validated depth snapshots and trade events. It
is a single-owner, preallocated hot-path component. Construction allocates its
fixed storage image; accepted updates, snapshot construction, publication, gap
handling, and replay reset do not allocate, perform I/O, read a clock, or call an
external service. Callers supply all event and publication times.

The numeric and parity policy is established by
[ADR-0009](../adr/0009-fixed-point-incremental-features.md). The persistent
metadata boundary remains the canonical `FeatureSnapshotMetadata` described in
[Event Contracts](event-contracts.md).

## Input contract

A book input binds a canonical global event ID and ordinal, session, instrument,
configured venue, exchange event time, process-monotonic time, source operation,
affected price/quantity, validated depth image, and data quality. A trade input
binds the same provenance plus aggressor side, price ticks, and quantity units.

Events must arrive in strictly increasing global-ordinal order and
nondecreasing process-monotonic order. Exchange times may differ across venues;
the snapshot uses the greatest accepted exchange time. Unknown venues, invalid
IDs, malformed or crossed depth, zero/nonpositive prices or quantities,
`STALE`/`INVALID` input quality, and checked-arithmetic failure invalidate the
engine. An invalid engine accepts no new event until explicit recovery or a
session reset.

## Feature definitions

All divisions truncate toward zero. `ppm` means parts per million. Rates use the
observed span from the oldest retained activity event through the latest event,
with a one-nanosecond minimum denominator. Multi-level weights descend from the
configured depth count to one independently on each venue.

| Feature | Integer unit and definition |
| --- | --- |
| Midpoint | Half-ticks: best bid plus best ask |
| Spread | Ticks: best ask minus best bid |
| Relative spread | `2 * spread / midpoint`, ppm |
| Microprice | Ticks × 1,000,000, weighted by opposite-side top quantity |
| Top imbalance | `(bid quantity - ask quantity) / total`, ppm |
| Multi-level weighted imbalance | Weighted bid-minus-ask quantity over weighted total, ppm |
| Order-flow imbalance | Cont-style signed top-price/top-quantity contribution, quantity units |
| Signed trade imbalance | Buy-aggressor minus sell-aggressor volume over total, ppm |
| Add/cancel/execute rates | Event counts per observed second, millihertz |
| Queue depletion | Cancelled plus executed quantity per observed second |
| Rolling return | Latest minus oldest retained midpoint over oldest, ppm |
| Realized volatility | Integer square root of mean squared retained midpoint returns, ppm |
| Volume | Retained executed quantity units |
| VWAP | Ticks × 1,000,000 using a stable price anchor |
| Trade/quote intensity | Retained event counts per observed second, millihertz |
| Cross-venue divergence | Maximum minus minimum venue midpoint, half-ticks |
| Venue leadership | Deterministic best bid/ask venue numbers and dominant bid-leader share, ppm |
| Replenishment | Explicit source flag or same-side/same-price add after a bounded depletion, millihertz |
| Data quality | Canonical quality code and data age in nanoseconds |
| Time of day | Clamped session progress, ppm |

At equal best prices, quantities from all matching venues are summed. Leader
ties choose the lowest configured venue number. A consolidated locked or
crossed view is invalid because no licensed transition rule authorizes it.

## Warm-up, missing data, and freshness

Warm-up thresholds for book and trade event counts are configuration fields.
Every configured venue must publish valid two-sided depth. Trade-dependent
features remain `WARMING` until their threshold and become `MISSING` when the
threshold permits readiness but the retained window has no trades. Return and
volatility features require two midpoint observations. Cross-venue divergence
requires two contributing venues.

Book values carry the last book-event monotonic time; trade values carry the
last trade-event time. Each configured venue is checked separately against the
stale threshold. Window capacity loss is visible as `DEGRADED`, never hidden.
Gap, recovery, and session reset clear all rolling state and warm-up counts.

## Ownership and publication

One thread owns an engine for its lifetime. Wrong-thread calls fail closed.
`FeatureSnapshotPublisher` invokes a caller-supplied nonblocking function; a
false result is reported as explicit backpressure. The publisher performs no
retry, logging, disk access, or synchronous telemetry.

`FeatureParityTestHarness` stores at most 256 canonical inputs in a fixed array,
runs the same implementation online and in replay, and compares content-derived
snapshot IDs and hashes. The checked-in randomized seed is `20260829`.

## Capacity and current limitations

The compile-time limits are eight venues, sixteen weighted levels per venue,
2,048 rolling samples, and 256 parity inputs. Event-rate capacity must be sized
from venue burst measurements. Replenishment inference is deliberately a
conservative indicator, not a claim that hidden liquidity has been proven.
Auction-specific, corporate-action-adjusted, and licensed venue semantics remain
outside this generic implementation.
