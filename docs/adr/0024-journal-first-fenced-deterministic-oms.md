# ADR 0024: Journal-first fenced deterministic OMS

- Status: Accepted
- Date: 2026-08-30
- Owners: OMS, risk, replay, gateways

## Context

The OMS converts an exactly approved intent into a normalized gateway command
and must remain reproducible across duplicate, reordered, and racing venue
events. A restart cannot infer whether a command reached a venue. Venue and
broker protocols, recovery queries, and drop-copy rules are licensed details
that are not present in this repository. The hot path also prohibits network
calls, disk writes, dynamic allocation, and unbounded waits.

## Decision

The repository owns a single-writer, fixed-capacity `DeterministicOms`. It uses
an explicit transition matrix, integer price ticks and quantities, fixed hash
tables for order, receipt, and execution identity, and immutable versioned
snapshots. Each accepted new, cancel, or replace intent must carry the exact,
unexpired deterministic risk decision for the same immutable intent.

Every input reserves a journal position before state evaluation. The result and
any `GatewayCommand` are committed into the bounded hash chain before the
command is returned to its caller. The OMS does not transmit commands. The
reference binary journal persistence API is off the hot path and is fenced to
the same build ABI, configuration hash, record hashes, and chain hash.

Receipt identity, intent identity, and logical execution identity are separate.
Repeated receipts are idempotent; a receipt reused with different content is an
unsafe invariant violation. The first matching primary or drop-copy fill
changes quantity, while a matching second source only reconciles it. Conflicting
execution economics fail closed.

Commands and venue events carry an exchange-session epoch and monotonically
increasing active-leader fencing token. An old or foreign authority cannot emit
a command. Authority rotation moves every live order to `UNKNOWN_RECOVERY`.
Restart replay verifies every historical result, emits no historical command,
and moves every reconstructed live order to `UNKNOWN_RECOVERY`; readiness
returns only after explicit normalized recovery observations.

Canonical schema v1.8 additively extends `OrderEvent` with OMS input/source/
outcome, prior and current state, version, normalized external identity,
deterministic client order ID, authority, exact risk evidence, and journal and
snapshot evidence.

## Consequences

- Steady-state evaluation is bounded and allocation-free after construction.
- Journal, order, execution, and idempotency capacity exhaustion is explicit
  and inhibits operation rather than evicting safety evidence.
- A caller may enqueue a returned command only after the committed result; it
  must independently enforce the future synthetic gateway's final safety gate.
- Recovery is deliberately operational and near-real-time, not an innermost
  hot-path action.
- The binary journal is a same-build reference format, not a cross-version
  archival contract. Canonical `AuditEnvelope` records remain the long-term
  interoperability boundary.
- Real venue client-ID limits, replace semantics, recovery queries, drop-copy
  keys, session rules, and retransmission behavior remain licensed integration
  boundaries.

## Rejected alternatives

- Re-emitting unacknowledged commands after replay risks duplicate live orders.
- Recording after transmission creates an unauditable crash window.
- Treating primary and drop-copy fills as separate executions transiently
  doubles positions and risk.
- A distributed database or RPC lookup during transition evaluation violates
  deterministic bounded-runtime requirements.
- Floating-point execution values make replay dependent on evaluation order and
  platform behavior.

