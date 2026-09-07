# ADR 0041: Safety-ordered full-system paper validation

- Status: Accepted
- Date: 2026-09-06

## Context

Prompt 39 requires one repeatable validation path spanning synthetic market data,
forecast specialists, the decision stack, execution, positions, audit, and
observability. The requested logical flow lists risk before OMS and routing.
However, the router contract established by ADR 0026 deliberately accepts a
venue-neutral execution objective and emits a venue-specific child proposal.
The risk contract approves an exact venue, price, quantity, action, account,
strategy, configuration, and validity interval. Performing final routing after
risk would therefore invalidate the approval or allow routing terms to escape
the approved scope.

The intelligence specialists are near-real-time services and Python is forbidden
from the colocated hot path. Their integration boundary is the common,
versioned `ModelForecast` contract and bounded forecast cache, not an in-process
Python call.

## Decision

The full-system PAPER acceptance harness uses this safety order:

```text
synthetic protocol -> feed handler -> book -> incremental features
-> versioned specialist forecast contracts -> market-state controller
-> constrained ensemble/abstention -> venue-neutral execution objective
-> deterministic router proposal -> fresh exact pre-trade risk
-> OMS -> fenced PAPER gateway -> fills -> positions/P&L
-> decision explanation, component journals, and bounded telemetry
```

The harness applies the following constraints:

- it constructs only `GatewayMode::paper` and `TradingMode::paper` components;
- it contains no endpoint, credential, live adapter, or activation path;
- every gateway command must bind to an approved, unexpired risk decision whose
  hash matches the OMS command;
- an ensemble abstention never creates an objective or order;
- market halt, invalid feed/book state, unsafe clock state, gateway loss, and
  ambiguous process ownership prevent new gateway emission;
- specialist implementations remain out of process; deterministic contract
  fixtures exercise their common publication boundary without putting Python or
  RPC on the hot path;
- each scenario is run from a fresh state twice with the same explicit seed, and
  deterministic component and outcome hashes must match;
- generated JSON and Markdown reports are off-path artifacts and are never
  consulted by execution.

## Consequences

The validation order differs textually from the high-level Prompt 39 arrow list,
but it enforces the stronger existing risk invariant: routing cannot mutate an
approved order. Reports present both the logical stage names and the enforced
execution order.

The harness proves integration against repository-owned synthetic data and
forecast contracts. It does not certify licensed market-data feeds, news feeds,
broker sessions, exchange protocols, network latency, economic value, or live
trading. Those remain separately governed integration boundaries.
