# ADR 0022: Journal-first deterministic pre-trade risk

- Status: Accepted
- Date: 2026-08-30
- Owners: risk, OMS, edge-core, compliance

## Context

Every order intent must receive the same decision during online processing and
replay. The decision path cannot depend on a service, disk, dynamic allocation,
floating-point arithmetic, or wall-clock calls. A positive decision must not be
observable unless its audit record has bounded local capacity. Concurrent fills,
configuration changes, kill commands, and intent evaluations must not create a
mixed-state approval.

## Decision

1. `DeterministicPreTradeRiskEngine` executes the 30 checks in the numbered order
   documented in the architecture. The first failure wins and has a stable check
   and reason code. Unknown, malformed, stale, unavailable, and overflow states
   reject.
2. Limits are an immutable, content-hashed, revisioned `RiskLimitSnapshot` with
   fixed maximum cardinalities. A bounded atomic gate serializes mutations and
   evaluations. Contention has a bounded retry count and grants no authorization.
3. Prices are `int64` ticks, quantities are integer instrument units, money is
   integer currency nanos, and factor values use integer PPM. All products, sums,
   absolute values, and sign conversions are checked before use.
4. The engine reserves a slot in a fixed-capacity local journal before evaluating.
   If reservation fails, it produces no decision. It commits the completed
   decision before returning it to the caller. Disk durability remains an
   off-hot-path journal responsibility.
5. Approved new orders atomically reserve pending symbol quantity. Fill updates
   release a caller-specified portion of that reservation. Exact order-level
   reservation reconciliation belongs to the OMS integration phase.
6. Symbol, strategy, venue, account, and firm kill state is held in atomics.
   Engagement and reset share the bounded evaluation gate, active authority
   epoch, and increasing command sequence; reset additionally requires explicit
   authorization. This makes a completed kill command ordered against approvals.
   The engine also rereads kill state and authority epoch immediately before
   approval.
7. Limit replacement rejects rollback and same-revision hash conflict. A newer
   snapshot fences the engine, engages the firm kill, invalidates local state,
   and requires positions/P&L to be reseeded before an authorized reset.
8. The repository permits `SIMULATION` and `PAPER` evaluation only. `LIVE` always
   rejects. Locate and self-trade prevention are fail-closed local policy hooks;
   no venue rule is invented.
9. Schema v1.6 additively binds `OrderIntent`, `RiskDecision`, and
   `KillSwitchEvent` to account, venue, action, mode, evidence hashes, generations,
   authority epoch, check masks, and journal sequence.

## Consequences

- The single bounded evaluation lane gives deterministic snapshots and simple
  replay semantics; a sharded owner-per-account design may be evaluated later.
- Capacity exhaustion and contention are explicit non-authorizations, not
  risk rejections that a gateway could accidentally treat as approval.
- The local journal proves publication ordering, not durable storage. The OMS
  and append-only journal phases must preserve the decision and reconcile exact
  reservations before any gateway can exist.

## Rollback

Stop intent production, engage the firm kill, retain v1.6 journal records, and
restore a previously approved binary and configuration only through a new,
higher revision. Never decrement a revision or automatically clear a kill.
