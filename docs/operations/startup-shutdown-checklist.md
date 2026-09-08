# Startup and shutdown checklist

This checklist is for `SIMULATION` and `PAPER` only. Preserve the completed copy
under the session evidence identifier. Any unchecked or unknown mandatory item
keeps new order admission inhibited.

## Startup

- [ ] Record session ID, change/incident ID, operators, UTC and monotonic start,
  source revision, artifact hashes, deployment profile hash, configuration hash,
  host-facts hash, and deterministic seed where applicable.
- [ ] Confirm `live_trading_capable=false`, `AEGIS_ENABLE_LIVE_TRADING=OFF`,
  `AEGIS_LIVE_TRANSMISSION=DISABLED`, and profile mode `SIMULATION` or `PAPER`.
- [ ] Confirm no production endpoint, credential, route, native adapter, or live
  activation record is installed or reachable.
- [ ] Create and verify the startup inhibit before starting any edge service.
- [ ] Validate CPU/NUMA/NIC/huge-page/memlock/file-descriptor assignments and
  journal mount/free-space/fsync health against the selected profile.
- [ ] Start and validate PTP/clock guard. Require expected source identity,
  hardware timestamps, fresh synchronization, and `HEALTHY` clock state.
- [ ] Start the journal, run its recovery scanner, verify segment/index chains,
  writer epoch, capacity, retention hold, and asynchronous queue health.
- [ ] Start edge core with a new process epoch. Verify exactly one owner and no
  split-brain or stale shared-memory state.
- [ ] Start observability. Verify build/version, health, readiness, configuration
  hash, bounded metrics, structured logs, alert routes, and runbook links.
- [ ] Start feed handlers in recovery. Verify channel/session identities, A/B
  agreement, sequence continuity, freshness, raw journaling, and a validated
  snapshot before data becomes healthy.
- [ ] Rebuild books and features; require valid uncrossed books, warm/fresh
  snapshots, deterministic parity, and explicit session status.
- [ ] Load only signed approved model artifacts. Expired, late, incompatible,
  uncalibrated, or excessive-OOD outputs remain ineligible.
- [ ] Reconstruct OMS, fills, positions, P&L, reservations, and risk from verified
  evidence. Reconcile all unknown or unmatched state before risk becomes ready.
- [ ] Install the immutable risk snapshot and confirm halt, clock, feed/book,
  configuration, authorization, fencing, journal, and all kill states.
- [ ] Start the synthetic/PAPER gateway last. Verify local-only session, mode
  label, epoch/token, rate limits, audit chain, and a no-order heartbeat.
- [ ] Run a bounded PAPER smoke decision and verify intent → risk → OMS → router →
  paper gateway → fill → position/P&L → audit correlation.
- [ ] Clear only the commissioning inhibit through the authorized non-live
  workflow. This does not clear incident kills or authorize live operation.

Startup order is clock → journal → edge core → observability → paper gateway,
with feed/book/model/risk readiness validated before order admission. Process
liveness alone is never readiness.

## Shutdown

- [ ] Record operator, reason, session ID, UTC and monotonic time, configuration
  hash, and current order/position/journal sequences.
- [ ] Inhibit new intents and gateway submission first; verify the inhibit at risk
  and final gateway boundaries.
- [ ] Quiesce strategies and forecast consumers. Do not wait indefinitely for
  models, intelligence, exporters, or control-plane RPCs.
- [ ] Resolve or explicitly mark outstanding PAPER commands and responses. Never
  infer cancel or fill from silence.
- [ ] Reconcile OMS, gateway, fills, positions, P&L, and risk snapshots; record all
  remaining unknowns as a failed shutdown requiring recovery.
- [ ] Stop the paper gateway, then observability, then edge core.
- [ ] Drain only bounded mandatory journal queues, seal/rotate the segment, fsync
  according to policy, and verify its checksum/sequence/hash chain.
- [ ] Stop the journal, then the clock guard. Retain the inhibit across restart.
- [ ] Capture final health/readiness states, queue/drop counters, journal hash,
  position/P&L hash, resource observations, and audit-extraction result.
- [ ] Confirm restart will allocate new process/session authority and cannot
  restore any prior authorization implicitly.

A shutdown deadline expiry, journal failure, or reconciliation mismatch is an
incident. Preserve evidence and follow the relevant recovery runbook.
