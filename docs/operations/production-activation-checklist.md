# Production activation review checklist

## Current status: PROHIBITED

This document is a review gate, not an executable activation procedure.
No production activation was performed. The current
[production blockers](../reviews/blockers.md) make the terminal decision
**STOP / NO-GO**. Checked-in profiles and tooling support only `SIMULATION` and
`PAPER`.

## Prerequisite evidence

- [ ] Fresh hostile production-readiness audit has no BLOCKER or CRITICAL finding.
- [ ] Exact live-capable source, build manifest, compiler/toolchain, SBOM,
  provenance, signatures, and reproducibility evidence are approved.
- [ ] Live capability was built only through the separately authorized compile-
  time option; non-live artifacts remain unable to load a production adapter.
- [ ] Exact immutable runtime configuration is signed, fresh, scoped, staged,
  independently approved, and bound to the artifact/site/integrations.
- [ ] Explicit operator authorization is authenticated, scoped, expiring, bound
  to the current authority epoch, and distinct from required approvals.
- [ ] Risk services and all 30 pre-trade checks are healthy with current limits,
  positions, P&L, credit/capital, restricted state, and independent kill paths.
- [ ] Clock synchronization, source identity, hardware timestamping, offset,
  drift, freshness, and stabilization are valid on target hardware.
- [ ] Licensed market/reference data, books, sessions, halts, auctions, sequence
  recovery, and data quality are current and certified.
- [ ] Exactly one physically fenced gateway/session owner has a current leader
  token; split-brain and stale-owner rejection are proven across fault domains.
- [ ] OMS, open orders, executions, drop copy, positions, cash/P&L, and risk
  reservations reconcile with no unknown state.
- [ ] The authoritative journal is writable, durable, complete, checksummed,
  replayable after power loss, replicated according to policy, and under legal
  retention/incident controls.
- [ ] Every real adapter/provider passed licensed conformance, capacity,
  certification, recovery, security, operational, and legal review.
- [ ] Target-site systemd composition, NIC/PTP/NUMA/NVMe mapping, cold start,
  watchdog, disk failure, shutdown, failover, rollback, and soak evidence pass.
- [ ] Regional/control services use signed immutable images, authenticated
  identities, secrets/KMS/CA, backups, restore proof, and fail-closed distribution.
- [ ] Regulatory, surveillance, reporting, security, business continuity, and
  incident approvals are complete for the exact scope.
- [ ] An auditable activation record cryptographically binds every item above and
  is durably acknowledged before any state can be armed.

## Independent final gate

Even after every prerequisite is externally satisfied, the final gateway must
re-evaluate build capability, signed configuration, operator authority, risk,
clock, data/book/status, fencing, kill/inhibit state, and activation-record
identity for the exact command. Unknown or changed evidence rejects. Restart,
recovery, failover, rollback, or timeout clears authority and starts non-live.

There is no break-glass path that creates live authority, no automatic
activation, and no action to run from this repository while status is
`PROHIBITED`.
