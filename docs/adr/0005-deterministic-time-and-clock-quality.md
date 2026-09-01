# ADR 0005: Deterministic time and clock-quality evaluation

- Status: accepted
- Date: 2026-08-29
- Decision owners: edge-core, risk, market-data, OMS, and gateways

## Context

Aegis-MX needs nanosecond timestamps for four incompatible domains: local wall
time, process monotonic time, exchange event time, and hardware/NIC receive
time. Mixing those domains, silently overflowing a duration, reading a system
clock inside a decision loop, or immediately trusting a recovered PTP source
can invalidate event ordering and safety deadlines. Clock health is also a
mandatory order-transmission precondition.

The v1 wire schema already contains `ClockQualityState` as a source-observation
record whose enum describes synchronization conditions. Repurposing those enum
numbers for a new operational state machine would violate the schema evolution
policy.

## Decision

Create a separate C++ target, `aegis::time`, owned by `edge-core`.

- Represent each timestamp domain with a distinct fixed-width type and expose
  no cross-domain arithmetic operator.
- Represent durations as signed integer nanoseconds. All addition, subtraction,
  elapsed-time, and magnitude operations return explicit status and never
  saturate, wrap, throw, or invoke undefined behavior.
- Permit system clock access only through explicitly named injected interfaces.
  Production implementations call `steady_clock` or `system_clock` only when
  their explicitly named read method is invoked. Tests and replay use manual or
  simulated clocks.
- Calculate all local pipeline latency from monotonic stage captures. Hardware
  receive time remains available for validated event ordering, but is paired
  with a local monotonic ingress capture rather than subtracted from a
  monotonic decode timestamp.
- Implement the operational state machine as
  `UNKNOWN -> SYNCING -> HEALTHY`, with immediate transitions to `DEGRADED` or
  `UNSAFE` when thresholds demand it. Recovery from either state requires a
  continuous configured stabilization interval from one source identity.
- Make thresholds mandatory and validated. The library provides no operational
  PTP offset, drift, staleness, or stabilization defaults because those values
  require platform, venue, and risk approval.
- Interpret offset in nanoseconds and drift in signed parts per billion. Use
  two threshold bands: values within the healthy band may stabilize to
  `HEALTHY`; values between healthy and unsafe bands are `DEGRADED`; values
  at or beyond the unsafe boundary are `UNSAFE`.
- Map `HEALTHY` to normal operation, `DEGRADED` to the configured `REDUCE_ONLY`
  or `BLOCKED` mode, and `UNKNOWN`, `SYNCING`, and `UNSAFE` to `BLOCKED`.
- Expose a bounded, allocation-free metrics snapshot containing state, reason,
  source identity, offset, drift, synchronization age, hardware timestamp
  availability, and transition/failure counters. Export adapters remain outside
  the hot path.

The runtime operational state is deliberately distinct from the existing v1
wire `ClockQualityCode`. A future additive schema revision may persist the
detailed state after reader-first rollout; this ADR does not rename or
reinterpret existing wire values.

## Consequences

Positive consequences:

- incompatible timestamps cannot be compared or subtracted accidentally;
- replay and tests advance time without sleeps or ambient clock state;
- overflow, timestamp regression, stale synchronization, and clock jumps are
  machine-readable failures;
- state evaluation performs bounded work without allocation, I/O, locking, or
  implicit system calls;
- source changes and recovery cannot immediately restore normal operation.

Costs and constraints:

- callers must capture local monotonic timestamps at each latency boundary;
- configuration owners must supply reviewed thresholds before constructing a
  state machine;
- virtual injected clock reads are explicit boundary calls and are not intended
  to be repeated unnecessarily inside a critical loop;
- detailed operational state is not yet added to the v1 serialized schema.

## Alternatives considered

Using `std::chrono::time_point` aliases directly was rejected because aliases
can still share a clock/domain and do not encode exchange versus NIC provenance.

Using wall time for deadlines or latency was rejected because wall clocks can
step backward or forward.

Subtracting hardware epoch time from process monotonic time was rejected because
the result is meaningless without a versioned correlation model and uncertainty
bound.

Immediately returning from `UNSAFE` to `HEALTHY` was rejected because a single
good sample is not evidence of stable recovery.

Providing example thresholds as production defaults was rejected because the
repository has no approved platform or venue clock specification.

## Evidence and review triggers

This decision is gated by deterministic boundary/overflow tests, injected-clock
tests, backward wall-jump tests, drift and stale-sync transitions, source-change
stabilization tests, sanitizer builds, and timestamp/arithmetic benchmarks.

Revisit this ADR before adding a timestamp domain, correlating distinct clock
domains, persisting the operational state, changing recovery semantics, or
introducing platform-specific hardware-clock access.
