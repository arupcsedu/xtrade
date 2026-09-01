# Event-level exchange simulator and backtester

The Aegis-MX backtester is an offline event evaluator. It consumes deterministic
synthetic events, verified synthetic captures, or already-canonicalized
historical event streams. It exposes no bar-fill API and has no path to an
exchange gateway.

## Event ordering

For each event time, due acknowledgements and expirations are applied first.
The market event then updates external queue state and the visible book, trades
or auction matches consume price-time queues, forecast/fill horizons are marked,
and strategies finally observe the resulting state. A zero-latency decision may
cross the current quote, but it cannot fill against the trade that caused the
decision.

Order-level synthetic identities establish cancellation and execution priority.
Price-level events are accepted only as an explicitly lower-fidelity aggregated
queue model. Deterministically inferred hidden quantity is placed ahead of the
hypothetical order and retained as an assumption in the order record.

Stale or degraded events cannot cause fills or new decisions. Invalid hashes,
malformed shapes, sequence rollback, crossed/invalid books, or resource-limit
breaches terminate the run with a stable status. Trading halts prevent matching;
auction imbalance events match only while the instrument is in auction state.

## Evaluation

Forecast samples are resolved at their declared horizon and report log loss,
Brier score, directional precision, calibration error, and p10-p90 coverage.
Execution reports fill ratio, time to fill, effective/realized spread, slippage,
adverse selection, and implementation shortfall. Portfolio reports after-cost
P&L, event-sample Sharpe and Sortino, drawdown, empirical 95% CVaR, turnover, and
event-period attribution.

Every strategy report contains zero, half, realistic, and double transaction-
cost scenarios. JSON reports set `raw_accuracy_only_claims_permitted` and
`live_trading_capable` to false and carry explicit limitations. Backtest output
is infrastructure/evaluation evidence, not proof of profitability.

The rationale is recorded in
[ADR 0029](../adr/0029-event-level-backtesting-with-explicit-model-risk.md), and
focused commands and deterministic fixtures are listed in
[Event Backtester Testing](../testing/event-backtester-testing.md).
