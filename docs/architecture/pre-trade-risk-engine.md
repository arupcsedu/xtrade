# Deterministic pre-trade risk engine

## Boundary and safety model

The risk engine is the sole local boundary between a versioned `OrderIntent`
and a possible OMS authorization. Models and the ensemble cannot call a gateway.
The kernel has no RPC, disk, Python, heap allocation, wall-clock read, or
floating-point dependency. Callers supply immutable market, clock, feed, book,
authorization, configuration, and policy-hook evidence.

Structural validity, expiry, and non-regressing process-monotonic evaluation
time are preconditions to the numbered checks. A violated precondition is
journaled as an explicit fail-closed rejection.

Only `EvaluationStatus::journaled` plus a structurally valid
`DecisionCode::approved` is an authorization. `engine_busy`,
`journal_unavailable`, and `not_initialized` contain no usable decision. The
approved record is bound to its intent hash and SHA-256, risk snapshot and
configuration, context hash, position/kill generations, authority epoch,
expiry, and local journal sequence.

The engine can atomically install one healthy account-scoped colocated
portfolio snapshot. Installation rejects schema/configuration/session/account
scope mismatch, missing symbol or strategy state, hash conflict, and sequence
rollback. Intent evaluation still reads only local atomic state and performs no
portfolio RPC. See the
[portfolio-risk architecture](portfolio-risk-service.md).

## Deterministic check order

| # | Check | Fail-closed input |
| ---: | --- | --- |
| 1 | Trading mode | Unknown or `LIVE`; only simulation/paper are admitted |
| 2 | Operator/session/account | Missing authorization or identity mismatch |
| 3 | Strategy authorization | Unknown or disabled strategy |
| 4 | Symbol authorization | Unknown or disabled instrument |
| 5 | Restricted list | Restricted instrument |
| 6 | Market state | Unsafe or stale authoritative snapshot |
| 7 | Halt | Official halt/unknown status |
| 8 | Clock health | Non-healthy, stale, or mismatched clock evidence |
| 9 | Feed/book health | Non-healthy, invalid, stale, or mismatched data |
| 10 | Maximum order quantity | Zero or above symbol limit |
| 11 | Maximum order notional | Checked tick × tick-value × quantity overflow/limit |
| 12 | Price collar | Nonpositive or outside checked reference collar |
| 13 | Tick size | Price not divisible by configured integer increment |
| 14 | Duplicate intent | Seen identity or fixed table exhaustion |
| 15 | Order rate | Fixed-window order capacity exceeded |
| 16 | Cancel rate | Fixed-window cancel capacity exceeded |
| 17 | Symbol position | Projected position plus pending reservation exceeds limit |
| 18 | Gross exposure | Checked projected marked gross exceeds limit |
| 19 | Net exposure | Checked absolute marked net exceeds limit |
| 20 | Sector concentration | Projected fixed-sector exposure exceeds limit |
| 21 | Factor/beta exposure | Projected PPM factor exposure exceeds limit |
| 22 | Daily loss | Firm P&L is stale or below loss limit |
| 23 | Strategy loss | Strategy P&L is stale or below loss limit |
| 24 | Drawdown | Checked firm peak-to-current loss exceeds limit |
| 25 | Credit/capital | Projected gross exceeds capital limit |
| 26 | Short sale/locate | Required local hook is not explicitly allowed |
| 27 | Self-trade prevention | Local hook is not explicitly allowed |
| 28 | Venue authorization | Unknown, disabled, or mismatched venue |
| 29 | Kill switch | Applicable symbol/strategy/venue/account/firm kill |
| 30 | Configuration freshness | Identity, revision, hash, age, or fencing mismatch |

All checks are represented in `completed_check_mask`. A rejection identifies the
first failed check; an approval proves all 30 bits completed. Cancel intents use
the same safety checks but carry zero approved price and quantity.

## State and concurrency

Limits and working state are preallocated for bounded symbol, strategy, venue,
sector, factor, duplicate-intent, and rate-window capacities. The atomic gate
provides one coherent view for evaluations, fill/P&L updates, kill commands, and
limit installation. Position, kill, and evaluation generations make observed
state auditable. A completed kill command is therefore ordered after any prior
approval and before every later one; a final atomic reread is defense in depth.

Opposing pending orders are projected as independent fill intervals and cannot
net away position, gross, net, sector, or factor risk. An approved new order
increments pending buy or sell quantity before publication.
Fills update position/P&L and release an explicit reserved amount. Until the OMS
owns per-order reservations, operators must treat unmatched reservations as a
safe overestimate and replace/restart as requiring full reconciliation.

See [ADR 0022](../adr/0022-journal-first-deterministic-pre-trade-risk.md), the
[kill-switch runbook](../operations/risk-kill-switch-runbook.md), and the
[testing record](../testing/pre-trade-risk-testing.md).
