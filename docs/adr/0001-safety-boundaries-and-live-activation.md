# ADR-0001: Safety Boundaries and Live Activation

| Field | Value |
| --- | --- |
| Status | Accepted |
| Date | 2026-08-28 |
| Deciders | Aegis-MX principal engineering baseline |

## Context

Aegis-MX will combine event ingestion, multiple model types, deterministic
decision logic, pre-trade risk, and exchange connectivity. A failure or ambiguous
state in any of these areas can create financial, regulatory, or operational
harm. The platform needs an initial boundary model before interface schemas or
services are implemented.

The [engineering contract](../architecture/engineering-contract.md) mandates
fail-closed behavior, model isolation, deterministic risk, bounded hot-path work,
and multiple independent conditions for live transmission. The architectural
baseline must make those requirements enforceable at a small number of explicit
boundaries.

## Decision

1. Partition the system into a colocated deterministic execution path and an
   off-hot-path control, model, storage, analytics, and observability plane.
   External RPC, disk I/O, Python, LLM inference, distributed databases, garbage
   collection, and dynamic allocation are excluded from the hot path.
2. Models communicate only through a common versioned forecast contract. Models
   cannot address or invoke a venue gateway.
3. Place deterministic pre-trade risk between all order intents and the gateway.
   Risk rejection has no strategy, model, or operator bypass.
4. Make the mode-gated gateway the sole real-venue transmission boundary. Venue
   adapters start in `SIMULATION` or `PAPER`; only licensed specifications may
   support a real venue. Synthetic/reference adapters cannot reach real venue
   endpoints.
5. Use the explicit mode states `SIMULATION`, `PAPER`, `LIVE_ARMED`,
   `LIVE_ACTIVE`, and `HALTED`. Live states are absent from builds without the
   default-off compile-time live option. No restart or recovery restores a live
   state automatically.
6. Require two recorded runtime transitions for live use: validation into
   `LIVE_ARMED`, followed by final activation into `LIVE_ACTIVE`. Each transition
   re-evaluates all live predicates and requires a durable, integrity-protected
   activation record. Partial or ambiguous activation remains non-live.
7. Evaluate live authorization as the conjunction of signed configuration,
   explicit scoped operator authorization, healthy/current risk, valid clock
   synchronization, valid market data and book state, unique fenced gateway
   authority, clear halt/kill state, build capability, and durable activation
   evidence. Unknown is false. The gateway re-evaluates the predicates at the
   final transmission boundary.
8. Keep halt and kill-switch enforcement independent from models and strategies.
   Resetting an inhibit does not reactivate trading.
9. Use integer ticks and integer quantities on the execution path, monotonic
   clocks for local deadlines, and validated PTP/hardware timestamps plus a
   deterministic tie-break rule for event ordering.
10. Journal decision and safety evidence through a bounded, nonblocking handoff
    to a local append-only binary journal. The precise response to unavailable
    mandatory journaling remains a prerequisite ADR before any transmission
    functionality is implemented.

The detailed target context is in
[`../architecture/system-context.md`](../architecture/system-context.md). The
operational rules are in
[`../operations/live-trading-safety.md`](../operations/live-trading-safety.md).

## Consequences

### Positive

- Unsafe or ambiguous state converges on rejection or halted transmission.
- The gateway provides a final, independently testable enforcement point.
- Models can evolve without acquiring order authority or hot-path dependencies.
- The activation record and versioned inputs support deterministic replay and
  audit.
- Simulation and paper implementations can exercise the same boundaries before
  live capability exists.

### Costs and constraints

- Live activation requires coordination across build, identity, configuration,
  risk, time, market data, leadership, gateway, and journal components.
- The two-step activation sequence adds operational latency; activation is not a
  hot-path operation, so safety and auditability take precedence.
- Venue-specific reconciliation and protective actions cannot be generalized
  safely and require later licensed specifications, ADRs, and tests.
- The local journal handoff needs explicit capacity and failure policy before an
  execution slice can be complete.

## Alternatives considered

### Runtime live flag only

Rejected. A single mutable flag cannot prove build provenance, configuration
integrity, current authority, risk readiness, clock/data validity, unique
leadership, or audit durability.

### Direct model-to-gateway submission

Rejected. It bypasses common forecast validation, ensemble policy,
deterministic risk, and a reproducible decision record.

### Automatic restoration of live mode after restart

Rejected. Cached authority can be stale, revoked, replayed, or inconsistent with
venue and account state. Restart requires reconciliation and fresh activation.

### Synchronous external persistence on the hot path

Rejected. It introduces unbounded latency and couples transmission to network or
disk behavior. A bounded local handoff preserves the path boundary; its
fail-closed capacity and durability semantics still require a dedicated ADR.

## Validation implications

Tests must observe the final gateway boundary and independently negate every
live predicate. They must cover partial activation, restart, stale authorization,
invalid signatures, risk/data/clock failure, split brain, kill switches, journal
failure, queue saturation, and malformed or late forecasts. Replay must include
rejections and mode transitions. No such implementation or test harness exists
in this documentation-only phase.
