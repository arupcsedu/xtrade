# Aegis-MX PAPER Soak Stability Report

Status: **PASS**

- Mode: PAPER; live-capable builds were rejected.
- Workers: 2 across 2 host(s).
- Accelerated primary events: 2,000,000,000.
- Exact replay events: 2,000,000,000.
- Real-time simulated events: 120,000.
- Total generated events: 4,000,120,000.
- Full-system acceptance cycles: 20.
- Certification probe executions: 100.
- Mean worker accelerated throughput: 3,578,930 events/s.
- Concurrent multi-node accelerated throughput: 6,933,342 events/s.

## Stability observations

- Post-warm-up RSS growth: maximum 536,576 bytes; limit 67,108,864 bytes.
- Late-session RSS range/limit: 0/1,048,576 bytes.
- File-descriptor growth/range: maximum 0/0.
- Sampled generator latency p50/p95/p99/p99.9/max: 171/210/250/510/4230 ns.
- Worker throughput imbalance ratio: 1.066865; limit 2.0.
- PAPER telemetry drops: 0.
- Duplicate order emissions: 0 observed by the PAPER acceptance and fencing probes.
- Acknowledged-state loss: 0 observed by failover, OMS recovery, and restart checks.

## Fault and event coverage

| Coverage | Count |
|---|---:|
| Auction Periods | 20 |
| Configuration Updates | 20 |
| Earnings Events | 20 |
| Feed Recoveries | 20 |
| Gateway Restarts | 20 |
| Halts | 20 |
| Leader Failovers | 20 |
| Macro Releases | 20 |
| Model Restarts | 20 |
| Session Transitions | 520 |


## Acceptance checks

- [x] all workers passed
- [x] worker evidence hashes valid
- [x] paper mode only
- [x] requested event coverage met
- [x] deterministic sampled replay
- [x] no unexplained memory growth
- [x] no file descriptor leak
- [x] no invalid state transitions
- [x] no duplicate order emission
- [x] zero acknowledged state loss
- [x] stable tail latency
- [x] recovery from injected failures
- [x] journal integrity
- [x] clock state recovered safely
- [x] model freshness enforced
- [x] stale snapshots not consumed
- [x] position and pnl consistent
- [x] no queue accumulation or drops
- [x] cpu throughput balanced
- [x] minimum acceptance cycles met


## Scope and limitations

This is synthetic/reference PAPER infrastructure validation; it does not qualify a licensed feed, broker protocol, real NIC, or production hardware and makes no profitability claim.

Generator-call latency includes measurement overhead and is not an end-to-end colocated latency qualification. Configuration changes are deterministic test-version changes, not production control-plane actions.

Raw worker summaries, per-session NDJSON, cycle reports, and probe logs are
retained beside this report. Their SHA-256 digests are recorded in
`paper-soak-report.json`.
