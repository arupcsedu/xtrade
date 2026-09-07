# Full-system PAPER trading validation

## Purpose

The Phase X acceptance runner is a deterministic system test, not a trading
strategy and not a live activation tool. It drives the real C++ feed handler,
order book, feature engine, market-state controller, ensemble, router, risk
engine, OMS, PAPER gateway, portfolio service, high-availability fencing
primitive, decision explanation, and telemetry interfaces. Near-real-time model
services enter through validated `ModelForecast` contracts so Python, RPC, and
provider I/O remain outside the edge hot path.

The mandatory execution ordering is documented in
[ADR 0041](../adr/0041-safety-ordered-full-system-paper-validation.md).

## Deterministic scenario catalog

The runner executes these scenarios in stable ordinal order with seed
`20260906` unless explicitly overridden:

1. ordinary midday trading;
2. market open;
3. market close;
4. earnings release;
5. CPI release;
6. breaking negative news;
7. false rumor followed by correction (the contradictory fast alert is retained,
   then a neutral correction forecast is separately evaluated and journaled);
8. index rebalance;
9. hidden-liquidity replenishment;
10. trading halt and reopening;
11. feed gap;
12. clock degradation;
13. model timeout;
14. risk-service restart;
15. gateway disconnect;
16. split-brain attempt.

Each scenario starts from isolated state. The exact replay pass reconstructs the
same inputs from the seed and compares source, feature, forecast, ensemble, risk,
OMS, gateway/fill, portfolio, explanation, and final outcome hashes.

## Acceptance invariants

The generated report fails unless all of these hold:

- every emitted gateway command has an exact approved risk binding;
- official halt blocks new orders until reopening and recovery stabilize;
- a feed gap produces a non-healthy data state and blocks new orders;
- expired model outputs receive zero weight and all-invalid inputs abstain;
- the high-disagreement scenario produces less exposure than the ordinary case;
- OMS and portfolio journals reconstruct orders, fills, positions, and P&L after
  the restart scenario;
- every OMS-created order has a valid `DecisionExplanationRecord`;
- all deterministic replay hashes match;
- every gateway and risk object remains in PAPER mode and live transmission is
  not compiled into the acceptance target.

## Commands

Use the required repository Python environment:

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts \
  make paper-integration
```

The command writes:

- `build/reports/paper-trading/acceptance-report.json` — machine-readable result
  conforming to
  [`paper-trading-acceptance-report-v1.schema.json`](../../schemas/paper-trading-acceptance-report-v1.schema.json);
- `build/reports/paper-trading/system-report.md` — human-readable scenario and
  acceptance summary.

For a batch node on installations that provide the user-authorized `parallel`
partition:

```bash
sbatch --wait tools/slurm/paper-integration.sbatch
```

The Slurm script requests no credentials and runs only the PAPER acceptance
target. This site's `parallel` partition requires two nodes, so the launcher
reserves one task on each node while the batch process performs the single
deterministic validation run. Local execution remains the reference because
scheduler availability and queue delay are site-specific.

## Interpretation

A passing report demonstrates deterministic safety integration against the
repository-owned synthetic protocol. It does not demonstrate predictive value,
profitability, real-NIC performance, exchange certification, or authorization
for live trading.
