# ADR 0020: Authoritative edge market-state controller

- Status: Accepted
- Date: 2026-08-30
- Owners: edge-core, risk, market-data, operations

## Context

Market-data, clock, book, intelligence, model, and operator inputs can indicate
different regimes simultaneously. If ensemble, risk, OMS, or gateways derive
their own precedence, an official halt or unsafe input can be masked by an
inferred event state. Recovery is also unsafe if an open status immediately
turns a halted or invalid book into `NORMAL`.

The state is read on the colocated decision path. Publication cannot allocate,
block, call a network service, or write synchronously to disk. Nevertheless,
every authoritative state or primary-reason change must enter the audit journal
before readers can observe it.

## Decision

1. `edge-core` owns one fixed-layout C++20 `MarketStateController`. Ensemble,
   risk, OMS, and gateways may consume its immutable snapshot; none may override
   it or implement a competing precedence order.
2. Inputs normalize official status, feed health, book validity, clock quality,
   news, earnings and macro calendars, realized volatility, spread, aggregate
   depth, model OOD/disagreement, and operator controls. Provider- and venue-
   specific mapping stays in the producer boundary.
3. Ranked arbitration is: official halt/close; unsafe clock; invalid/stale data;
   kill switch; recovery/reopening; event state; predictive stress or normal.
   `SHUTDOWN` is a terminal lifecycle state. A recognized official halt/close
   takes precedence over a simultaneous new shutdown request.
   A recognized halt/close also remains authoritative in a malformed or
   out-of-order observation; its exact source sequence is recorded without
   moving the controller's accepted-input watermark backward.
4. Safety escalation is immediate. `HALTED`, `DATA_DEGRADED`, and `STARTUP`
   cannot transition directly to `NORMAL`. They enter `REOPENING` or `RECOVERY`.
   `REOPENING` and `RECOVERY` require separate configured continuous healthy
   stabilization intervals. A renewed recovery input resets the interval.
5. Normal, resolved-news, and operator-requested exits from event and volatility
   states have a configured minimum dwell before entering `RECOVERY`.
   Higher-priority event changes and feed/book/clock safety recovery predicates
   remain immediate.
6. Scheduled earnings or macro state activates `SCHEDULED_EVENT` at the
   configured lead interval before release. An overdue scheduled record without
   release progression is invalid data and enters `DATA_DEGRADED`.
7. Every state change and every change to the primary reason/reason mask is a
   versioned transition. The controller submits the fixed-layout, hashed record
   to a mandatory bounded nonblocking journal sink before publishing. Rejection
   publishes nothing, invalidates the readable snapshot, returns
   `JOURNAL_REJECTED`, and never permits new orders. The next accepted fresh
   evaluation journals and republishes before clearing the invalidation.
8. One writer publishes through an odd/even sequence over lock-free atomic
   64-bit words. A release store publishes the completed image; readers use
   acquire loads, bounded retry, and a deterministic stable-hash check. There is
   no non-atomic concurrent payload access.
9. Only `NORMAL` reports `new_orders_permitted=true`, and that flag is advisory
   to downstream deterministic risk. It is never sufficient authorization to
   transmit an order.

## Consequences

- Identical ordered inputs and configuration produce identical transitions,
  reason masks, hashes, and journal records.
- Journal backpressure sacrifices availability and makes the controlling call
  fail closed rather than creating unaudited state.
- The bounded journal sink is the edge-core handoff. Durable append and recovery
  remain the independent journal-writer responsibility and cannot block this
  controller.
- Threshold values in repository tests are synthetic. Production limits require
  independent risk, operations, and venue review.

## Rollback

Consumers may stop reading the new snapshot only if they remain fail closed.
They must not reinstate local precedence logic. Existing transition records
remain versioned and replayable. A schema-major rollback requires a new process
epoch and an explicit compatibility plan.
