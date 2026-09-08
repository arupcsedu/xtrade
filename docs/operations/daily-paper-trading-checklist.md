# Daily PAPER-trading checklist

Complete this checklist for each session. `PAPER` is the repository-owned local
simulator, not a broker connection and not real-money trading.

## Before the session

- [ ] Create the daily evidence ID and record operators, build, configuration,
  model, schema, calendar, risk, and deployment hashes.
- [ ] Review open incidents, unresolved alerts, change windows, restricted-list
  state, corporate actions, scheduled earnings/macro events, auctions, and halts.
- [ ] Complete the [startup checklist](startup-shutdown-checklist.md).
- [ ] Verify all dashboards and alerts, including feed age/gaps, book validity,
  clock offset/state, journal lag, queues, model freshness/OOD, risk rejects,
  orders/fills, positions/P&L, leader state, and gateway mode.
- [ ] Verify all kills and inhibits match the approved session plan. Never clear
  an unexplained kill to make a test proceed.
- [ ] Record the test scenario set, seed, acceleration, paper fill/latency model,
  expected event count, acceptance thresholds, and stop conditions.

## During the session

- [ ] Continuously confirm mode `PAPER`, live capability false, one leader, fresh
  clock/data/book/risk/configuration, and a writable mandatory audit path.
- [ ] Investigate any gap, crossed book, stale snapshot, late model, OOD increase,
  disagreement, risk reject burst, unknown order, position mismatch, queue drop,
  or journal alert without weakening a threshold.
- [ ] Confirm halts and kills block fresh orders and recovery observes configured
  dwell/stabilization periods.
- [ ] Confirm every gateway command has a fresh exact risk approval and mandatory
  decision explanation; sample correlations throughout the day.
- [ ] Record operator actions and corrections. Do not delete or silently replace
  earlier alerts, forecasts, decisions, or evidence.

## End of session

- [ ] Stop through the [shutdown checklist](startup-shutdown-checklist.md).
- [ ] Verify final books, orders, fills, positions, realized/unrealized P&L,
  exposure, and fees against the PAPER simulator.
- [ ] Verify journal segments and indexes, then replay sampled windows and compare
  deterministic hashes.
- [ ] Export the redacted operator/control audit and preserve configuration,
  build/model manifests, raw test reports, alerts, and known limitations.
- [ ] Record resource growth, descriptor counts, queue accumulation/drops, latency
  drift, CPU imbalance, and any acceptance-threshold breach.
- [ ] Open incidents for every unexplained discrepancy. A daily sign-off reports
  PAPER test status only and conveys no production or live authorization.
