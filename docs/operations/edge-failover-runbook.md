# Edge leader failover and recovery runbook

## Immediate posture

On leader loss, competing leadership, heartbeat delay, lease expiry, gateway
disconnect, risk restart, or replication alarm, block new order emission first.
Do not promote from peer-heartbeat loss alone. Do not resend a historical
command, clear `UNKNOWN_RECOVERY`, edit a journal, or reuse a fencing token.

Protective risk reduction may continue only through a separately authorized,
explicitly modeled path. This implementation does not grant a remote region
normal order authority.

## Capture evidence

Record the build/version, configuration hash, node and process IDs, process
epoch, exchange-session epoch, fencing token and lease expiry, HA state/reason,
gateway session state, OMS health and unknown-order count, risk-service epoch,
journal sequence/hash, recovery publisher/applied sequence/hash, peer heartbeat,
and operator identity. Preserve journals and shared-memory metadata read-only.

## Promote the hot standby

1. Confirm the leader process and its gateway network/session path are fenced.
   A missing heartbeat is insufficient.
2. Obtain an authenticated quorum grant with a token strictly greater than the
   prior token and an explicit prior-owner-fenced assertion.
3. Require the standby recovery applied sequence/hash to equal the published
   sequence/hash. If not, remain `STANDBY_SYNCHRONIZING`.
4. Rotate the gateway/session and OMS authority to the new token. Keep gateway
   command consumption disabled.
5. Verify every journal segment and reconstruct OMS, fills, positions, P&L, and
   risk state. Treat every formerly live OMS order as `UNKNOWN_RECOVERY`.
6. Reconnect the synthetic/paper or licensed gateway under the new token and
   obtain a complete open-order, execution, and drop-copy view.
7. Reconcile every live order and execution. An ambiguous absence stays unknown;
   it is not permission to send again.
8. Confirm the risk snapshot was rebuilt under its current nonzero service epoch
   and agrees with reconciled positions.
9. Submit reconciliation evidence matching the recovery stream's latest source
   journal sequence/hash. Require `ACTIVE_LEADER` or the explicitly alarmed
   `ACTIVE_DEGRADED` state and `can_emit_orders=true`.
10. Record the activation and watch duplicate suppression, gateway reject,
    replication lag, journal health, and lease renewal metrics.

## Scenario actions

| Scenario | Action |
| --- | --- |
| Leader crash | Fence process and gateway, advance token, follow full promotion procedure |
| Standby crash | Keep valid leader, page on degraded redundancy, restore/catch up standby before lease loss |
| Network partition | Do not self-elect; let unrenewed lease expire and fence isolated side |
| Split brain indication | Fence both emitters, isolate gateway paths, preserve evidence, establish a new token only after ownership is unambiguous |
| Delayed heartbeat | Do not revive process in place; restart with a higher process epoch and reconcile |
| Stale shared memory | Quarantine segment, create a new region epoch, validate ABI/hash, then reconcile |
| Gateway disconnect | Enter `LEADER_RECONCILING`; query complete session state after reconnect |
| Risk-service restart | Reject new orders until positions and limits produce a reconciled snapshot under the new risk epoch |
| Journal/recovery failure | Enter `UNSAFE`, preserve original, use copy-only repair/recovery, restart |

## Roll back a failed promotion

Fence the candidate and its gateway session. Do not return the old leader to
service under its prior token. Recover the most authoritative verified journal
and venue view, issue a still-higher token to one fenced/reconciled candidate,
and repeat the procedure. Escalate if authoritative order or fill state remains
ambiguous.

## Exit criteria

Exactly one authenticated lease and physical gateway-session owner exist; the
recovery stream is healthy and caught up; journal hashes verify; OMS, gateway,
positions, fills, and risk are reconciled; no unknown order remains; safety
services are healthy; operator state is current; and the activation record is
auditable. This runbook does not authorize live trading.
