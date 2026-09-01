# ADR-0009: Fixed-point incremental online features

- Status: Accepted
- Date: 2026-08-29
- Decision owners: Feature Engine, Model Contracts, and Edge Core
- Governing contract: [Engineering Contract](../architecture/engineering-contract.md)

## Context

Online and replay feature calculations must agree exactly, must not allocate or
rebuild full history in the event path, and must never pass a NaN or infinity to
a model or decision component. Floating-point aggregates make exact parity
dependent on instruction selection, reassociation, and platform math behavior.
An absent value represented as zero would also be unsafe because zero is valid
for many features.

Cross-venue state is bounded by configured venues and depth. Rolling event
history is bounded both by time and capacity. At sufficiently high rates the
capacity can be exhausted before the time horizon expires, so silently claiming
a complete window would be misleading.

## Decision

The online payload is a fixed-layout C++20 array of 26 named feature slots. Each
slot contains a signed 64-bit integer, a process-monotonic as-of timestamp, and
an explicit `MISSING`, `WARMING`, `VALID`, `DEGRADED`, `STALE`, or `INVALID`
state. Prices remain in integer ticks; ratios and returns use parts per million;
rates use millihertz or integer units per second. Midpoint and cross-venue
divergence use half-ticks so odd tick sums remain exact. VWAP and microprice use
ticks multiplied by one million.

All multiplication, addition, subtraction, and conversion boundaries are
checked. Numeric failure invalidates the engine. Ratio helpers reduce operands
by their greatest common divisors before multiplication to avoid avoidable
overflow. No floating-point value is present in the snapshot contract.

History uses runtime-sized rings backed by compile-time fixed arrays. Updates
evict expired contributions and update aggregate sums in bounded work. When the
ring overwrites an event that is still inside the requested time horizon, the
affected state is `DEGRADED` until that lost interval expires. Current
cross-venue calculations may scan the configured venue and depth bounds; they
never scan event history.

Every configured venue must have a valid two-sided depth before the engine is
`READY`. Book and trade features retain separate freshness timestamps. A newer
trade cannot refresh an unchanged book feature. A stale configured venue makes
the snapshot stale. Gap notification invalidates and clears rolling state;
explicit recovery or session reset is required before accepting a new sequence.

The feature snapshot ID is content-derived from its identities, exact source
event range, feature-definition version, values, validity, timestamps, and
engine state. The existing canonical `FeatureSnapshotMetadata` record carries
that ID and a SHA-256 digest when the payload crosses a persistence or network
boundary. The fixed-layout hot payload is not serialized in the event loop.

Online and replay use the same `FeatureEngine`; the bounded parity harness
records canonical inputs and proves identical IDs and stable hashes on replay.

## Consequences

Fixed-point outputs are deterministic and cannot contain non-finite values, but
they are quantized toward zero. Feature-definition versions must change when a
unit, scale, formula, warm-up policy, or validity rule changes. Consumers must
check validity and definition version before using a value.

Memory per engine is fixed at compile time even when a smaller runtime window is
configured. Capacity sizing therefore trades memory for the probability of an
explicit degraded interval. Current depth work is bounded rather than strictly
constant-time.

## Alternatives considered

IEEE-754 feature payloads with NaN sentinels were rejected because sentinel
values can propagate and exact replay parity is platform-sensitive. Decimal
libraries and arbitrary-precision integers were rejected from the hot path due
to dynamic allocation and unbounded work. Recomputing each rolling statistic
from full history was rejected because cost grows with window occupancy.

## Review triggers

Revisit this ADR before adding a feature whose safe range cannot fit the stated
units, changing a formula or rounding rule, adding dynamic venue/window growth,
serializing the payload in the event loop, allowing partially stale venues, or
introducing a separate offline implementation.
