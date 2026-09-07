# Partial colocation outage runbook

## Trigger

Use this runbook when only part of a colocated edge fails: market-data host,
shared-memory region, gateway link, journal device, risk process, cross-connect,
power domain, or standby replication path. Partial reachability is ownership
ambiguity until proven otherwise.

## Immediate controls

1. Fence new order emission for the affected session and record the HA
   `partial_colocation_outage` reason.
2. Engage the narrowest safe kill scope consistent with uncertain exposure.
3. Preserve the current witness lease, fencing token, gateway session, journals,
   shared-memory headers, risk snapshot, and peer observations.
4. Do not route normal trading to a remote region. It may coordinate
   independently authorized risk reduction, but must report
   `RISK_REDUCTION_ONLY` and its actual non-colocated latency.

## Triage matrix

| Lost capability | Required posture |
| --- | --- |
| Market data/book validity | Halt new and price-sensitive actions; recover sequence/snapshot first |
| Gateway connectivity | Reconcile session/open orders and drop copy before any new command |
| Risk state | Block new orders; reconstruct positions/fills under a new risk epoch |
| Journal device/path | `UNSAFE`; stop emission and preserve the original media |
| Shared memory | Fence stale epoch; create and validate a new segment, never reinterpret in place |
| Standby/replication only | Current leader may be `ACTIVE_DEGRADED` while its lease and all local safety dependencies remain valid; restore before bounded queue exhaustion |
| Witness/quorum | No new grant or renewal; current lease expiry fences the edge |

## Recovery

Restore power/network/storage without clearing evidence. Prove one physical
gateway owner, issue a higher token if ownership was ever ambiguous, verify and
catch up the recovery hash chain, and execute the complete
[edge failover procedure](edge-failover-runbook.md). Validate clock, market data,
book, configuration, risk, OMS, gateway, journal, and kill-switch state again.

## Exit criteria

All required colocated dependencies are healthy and time-current, ownership is
unambiguous, the standby is caught up or explicitly alarmed as degraded,
reconciliation is complete, and incident evidence and operator actions are in
the immutable audit. A remote service must not be labeled as preserving
colocation latency.
