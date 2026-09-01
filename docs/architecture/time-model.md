# Time and temporal-integrity model

## Purpose

The `aegis::time` library provides the temporal primitives used by colocated
market-data, book, feature, model, risk, OMS, and gateway boundaries. It
implements time representation and clock safety only; it does not create order
authority, connect to a venue, or select operational thresholds.

The normative design decision is [ADR-0005](../adr/0005-deterministic-time-and-clock-quality.md).

## Timestamp domains

| C++ type | Meaning | Unit | Valid use |
| --- | --- | --- | --- |
| `WallClockTimeNs` | Local UTC/system clock epoch time | Signed nanoseconds since Unix epoch | Audit and presentation; jump detection |
| `MonotonicTimeNs` | Process/session-local steady clock | Unsigned nanoseconds from an opaque origin | Deadlines, stabilization, and local latency |
| `ExchangeTimeNs` | Venue-originated event time | Signed nanoseconds since Unix epoch | Event provenance and validated ordering |
| `HardwareReceiveTimeNs` | NIC/PTP receive time | Signed nanoseconds since Unix epoch | Ingress ordering and provenance |

The types are not implicitly convertible. Arithmetic is available only for two
timestamps of the same domain and returns `TimeResult`. Overflow, underflow, or
regression is an explicit `TimeError`; no operation wraps or saturates.

Exchange and hardware receive timestamps may be ordered only when the current
clock-quality and uncertainty rules permit it. They must never be directly
subtracted from process monotonic time. An ingress record therefore pairs its
hardware receive timestamp with a monotonic capture taken at the same software
boundary.

## Clock access

`SystemMonotonicClock` and `SystemWallClock` contain the only system-clock calls.
Nothing reads a clock during arithmetic, state evaluation, monotonicity checks,
or latency decomposition. Callers explicitly invoke `monotonic_now()` or
`wall_now()` and pass the result through the hot path.

`TestMonotonicClock`, `TestWallClock`, and `SimulatedClock` are deterministic.
The simulator advances wall and monotonic time together for normal progress and
can jump wall time independently to reproduce clock steps. Tests never sleep.

The library does not fabricate a hardware clock reader. Hardware receive
timestamps must come from a licensed/provider adapter or an explicitly
synthetic test source.

## Operational clock state

The state machine consumes one `ClockQualityObservation` at a caller-supplied
monotonic time:

- signed PTP offset in nanoseconds;
- signed frequency drift in parts per billion;
- opaque 128-bit source identity;
- last successful synchronization time in the same monotonic domain; and
- hardware timestamp availability.

Threshold configuration contains healthy and unsafe absolute offset bands,
healthy and unsafe absolute drift bands, healthy and unsafe synchronization-age
bands, a positive stabilization period, the hardware-timestamp requirement, and
the degraded operation mode. Every healthy bound must be strictly lower than
its unsafe bound. Invalid configuration cannot construct a state machine.
Synchronization-age and stabilization bounds must fit the signed duration
representation, offset/drift magnitude bounds must fit their source types, and
unknown degraded-mode enum values are rejected.

| Evaluated condition | State | Operation mode |
| --- | --- | --- |
| No observation | `UNKNOWN` | `BLOCKED` |
| Healthy sample, stabilization incomplete | `SYNCING` | `BLOCKED` |
| Healthy sample, one source stable for the full interval | `HEALTHY` | `NORMAL` |
| Between healthy and unsafe thresholds, or optional hardware time absent | `DEGRADED` | Configured `REDUCE_ONLY` or `BLOCKED` |
| Unsafe threshold reached, required hardware time absent, stale sync, invalid source/time, or monotonic regression | `UNSAFE` | `BLOCKED` |

`DEGRADED` and `UNSAFE` clear accumulated stabilization. Recovery begins with a
new healthy observation. Changing source identity also restarts stabilization.
State evaluation never relies on wall time.

This operational state is not a renaming of the existing schema
`ClockQualityCode`; the latter remains a version-1 source-observation contract.

## Wall-clock jump and monotonicity checks

`WallClockJumpDetector` compares wall progress with monotonic progress. The
residual is:

```text
(current wall - previous wall) - (current monotonic - previous monotonic)
```

A residual outside the configured symmetric tolerance is a forward or backward
jump. A non-increasing monotonic sample or arithmetic overflow is a separate
failure. Valid observations establish a new baseline after classification so a
single wall step does not create repeated alarms forever.

`MonotonicityValidator<Timestamp>` reports first, advanced, equal, or regressed
samples for one timestamp domain and can explicitly permit equal values. It
never imposes ordering across domains.

## Latency decomposition

`LatencyTrace` uses monotonic captures for every local stage:

1. wire ingress capture to decode completion;
2. decode completion to book application;
3. book application to feature readiness;
4. inference start to inference completion;
5. decision completion to send handoff; and
6. send handoff to acknowledgement receipt.

Each named utility and the aggregate decomposition fail on regression or
overflow. Unmeasured scheduling gaps remain visible between markers; the six
requested values are not forced to sum to a fabricated end-to-end number.

## Metrics boundary

`ClockQualityMetrics` is a pull snapshot with fixed fields and no dynamic labels:
state, reason, operation mode, source identifier, offset nanoseconds, drift ppb,
sync age nanoseconds, hardware timestamp availability, observation count,
transition count, source-change count, and monotonicity-failure count.

Prometheus/OpenTelemetry adapters copy this state through a bounded off-path
handoff. They must not stringify source identifiers, allocate labels, or call
exporters in the decision loop.

## Ownership and next integration

`edge-core` owns the types and evaluator. Platform operations own time-source
configuration. Risk and the final gateway gate consume immutable snapshots and
must treat every state except an explicitly permitted mode as non-authoritative.
The clock-failure response is documented in the
[runbook](../operations/clock-failure-runbook.md).
