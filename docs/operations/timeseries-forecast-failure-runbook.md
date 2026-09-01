# Time-series forecast service failure runbook

## Safety effect

The service is advisory and outside the execution hot path. Loss, lateness,
overload, or invalid output removes forecast availability. It does not authorize
trading, relax risk, extend forecast age, or permit a synchronous edge RPC.

## GPU failure

1. Confirm `model_health=degraded` and the bounded
   `forecast_gpu_failures_total` metric.
2. Confirm CPU retry uses the same model version, checkpoint digest, context
   digest, and horizons.
3. If CPU capacity misses deadlines, disable the adapter generation or reduce
   admitted load. Do not increase forecast age to conceal misses.
4. Restore GPU readiness only after a deterministic self-check and fresh health
   generation. Previously stale forecasts remain stale.

## Deadline or overload increase

Inspect bounded queue depth, rejected submissions, batch size, adapter latency,
CPU fallback count, and cache-hit rate. Scale this non-colocated service or shed
work at admission. Never make risk, OMS, or the gateway wait for recovery.

## Invalid provenance or malformed output

Keep readiness false, retain hashes and machine-readable rejection reasons, and
quarantine the checkpoint/configuration. Do not log raw licensed series. Resume
only with an approved immutable artifact and fresh point-in-time context.

## Rollback and shutdown

Stop admission, drain only until the configured monotonic shutdown deadline,
discard unfinished work, and terminate. Roll back v1.3 writers before v1.3
readers. Colocated cache consumers reject expired entries throughout the
operation.

## Operational endpoints

Use `/healthz` for process/model health and `/readyz` for admission readiness.
`/version` must continue reporting `live_trading_capable=false`, and
`/configuration` must expose the reviewed SHA-256. `/metrics` contains no
instrument or raw-series labels. A healthy process with repeated deadline,
adapter, or cache-staleness increments still requires investigation; never
extend forecast age merely to restore a green dashboard.
