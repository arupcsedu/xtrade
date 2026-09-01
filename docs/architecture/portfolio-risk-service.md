# Position, P&L, and portfolio-risk service

The portfolio-risk service is the authoritative colocated reconstruction of
orders, logical fills, positions, cost basis, P&L, and portfolio exposure. It
does not create an order, authorize trading, connect to a broker, or transmit to
an exchange. Simulation remains the only implemented execution environment.

## Boundary and timing

`PortfolioRiskService` is a bounded single-writer component in `cpp/risk`.
Normal order, fill, and mark events are real-time edge work. Drop-copy
reconciliation, corrections, busts, corporate actions, session rollover, and
stress evaluation are near-real-time. Replay and historical stress analysis are
offline uses of the same deterministic contracts.

The service exposes build information, health, readiness, configuration hash,
fixed counters, immutable snapshots, and graceful terminal shutdown. Its
hash-chained journal records are the structured audit log; synchronous text
logging is not performed in the event path.

## Event and state contracts

| Input | Identity and behavior |
| --- | --- |
| Order | Idempotent receipt ID plus stable order ID and deterministic lifecycle transition |
| Fill | Receipt ID plus logical fill ID, source, order/account/strategy/venue/instrument, side, integer ticks/quantity, and fee nanos |
| Correction | New receipt ID, target logical fill ID, and complete replacement economics |
| Bust | New receipt ID and target logical fill ID; a second bust is a logical duplicate |
| Mark | Instrument, positive integer ticks, and process-monotonic receipt time |
| Session rollover | New session ID and explicit flat-required or carry-position policy |
| Corporate action | Fully resolved integral numerator/denominator; provider interpretation remains outside this component |

Orders and fills use fixed open-addressed tables. Position rows are keyed by
account, strategy, and instrument. Event receipts and logical fills use separate
idempotency domains. A primary fill without an order is rejected. An independent
drop-copy fill may reconstruct a missing position, but the snapshot is
`RECONCILING` until the order identity arrives.

## Accounting

Each position maintains signed quantity, signed open cost in currency nanos,
and realized P&L. For an opening or same-direction fill, quantity and signed
cost are added. For a closing fill, the deterministic proportional open cost is
removed and the price difference becomes realized P&L. Fees are subtracted from
realized P&L. A reversal closes the existing side and opens the residual side at
the fill price. Checked arithmetic rejects overflow.

Average price is an audit view derived as:

```text
abs(open_cost_currency_nanos)
-------------------------------------------
abs(position_quantity) * tick_value_nanos
```

The open-cost numerator remains authoritative, so integer display rounding does
not alter later P&L. Unrealized P&L is `signed_mark_notional - signed_open_cost`.
Total P&L, peaks, and drawdown derive from the same snapshot.

## Exposure snapshot

The immutable snapshot contains position rows and aggregate instrument,
strategy, account, and sector views. It maintains gross, net, beta,
liquidity-adjusted, and event exposure in integer currency nanos, pending buy
and sell quantities, realized/unrealized/total P&L, peak, drawdown, active
orders/fills, unmatched source counts, orphan count, journal provenance, health,
readiness, and a stable hash.

A snapshot is ready only when all configured marks are present, no severe
invariant is active, no orphan fill exists, and required drop-copy confirmations
are complete. Journal exhaustion, conflicting fill economics, accounting
mismatch, or arithmetic overflow is `UNSAFE` and fail closed.

Pre-trade risk calls `install_portfolio_snapshot` locally. Installation verifies
schema, hash, session/configuration/account scope, freshness inputs, symbol and
strategy coverage, and monotonic sequence. The pre-trade evaluation itself
reads only its local atomics and never blocks on the portfolio service.

## Stress model

The infrastructure slice publishes deterministic, fixed-point P&L impacts for:

- market gap from net exposure;
- volatility spike from gross exposure;
- configured sector shock;
- beta-scaled correlated selloff;
- liquidity-collapse loss from exposure above ordinary gross exposure;
- configured options-gamma shock for instruments with nonzero positions.

These are risk plumbing scenarios, not economic forecasts. Shock sizes are
configuration inputs and have no claim of predictive or regulatory adequacy.

## Recovery and ownership

One thread owns mutations. Concurrent readers use immutable pinned snapshots.
Each journal record includes the input event, deterministic result, resulting
snapshot identity, previous record hash, and record hash. Recovery creates a
fresh service with the same configuration, replays records in sequence, and
requires identical statuses, invariant reasons, snapshot sequences, hashes, and
journal chain.

See [ADR 0023](../adr/0023-event-sourced-bounded-portfolio-risk.md), the
[reconciliation runbook](../operations/portfolio-reconciliation-runbook.md),
and the [testing record](../testing/portfolio-risk-testing.md).

