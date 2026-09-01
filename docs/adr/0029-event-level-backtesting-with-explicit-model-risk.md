# ADR 0029: Event-level backtesting with explicit model risk

- Status: Accepted
- Date: 2026-08-31

## Context

Sub-minute execution cannot be evaluated faithfully with bar-close fills. A
backtester must preserve event ordering, acknowledgement latency, queue priority,
partial fills, market state, and costs while keeping hypothetical orders wholly
separate from exchange transmission. Accuracy-only reports can also create
misleading economic claims when execution and transaction costs are omitted.

## Decision

`cpp/backtesting` is an offline-only event consumer. It accepts the repository's
license-clean synthetic order/price-level event contract, verified synthetic
captures, or caller-supplied canonicalized historical events. There is no bar
input and no exchange-gateway dependency.

The reference matching model uses price-time priority. Order-level events retain
synthetic order identity so cancellations and executions ahead of a strategy
order affect its queue position. Price-level inputs are supported with an
explicit `aggregated_price_level` quality label and cannot be represented as
order-level truth. Hidden liquidity is an inferred, seeded scenario with its
quantity recorded on each affected order; it is never described as observed.

Acknowledgement latency and venue rejects are deterministic functions of the
recorded seed and submission ordinal. Orders become eligible only after their
acknowledgement time. Events with stale or degraded quality suppress new
strategy actions and fills; invalid data, malformed hashes, or sequence rollback
fail the run closed. Halts prevent fills, and reopening/auction processing uses
explicit status and imbalance events.

Reports always include forecast, execution, portfolio, event-period, and
transaction-cost sensitivity sections. They compare zero-cost and realistic
cost results and state that raw forecast accuracy alone cannot support an
economic claim. Shadow orders share the execution model but never affect cash,
positions, or P&L.

## Consequences

- Results are event-level and reproducible from source events, configuration,
  strategy actions, and seed.
- Simulation assumptions remain visible in each report and do not imply live
  fill likelihood or profitability.
- Caller-supplied historical adapters must provide canonical events; licensed
  provider decoders remain outside this component.
- The offline implementation may allocate during setup and reporting. It is not
  eligible for placement in the live execution hot path.
