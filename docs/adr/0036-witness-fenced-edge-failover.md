# ADR 0036: Witness-fenced edge failover with mandatory reconciliation

- Status: Accepted
- Date: 2026-09-04
- Owners: edge-core, OMS, risk, gateways, operations

## Context

Process epochs fence stale writers on one shared-memory host, and the OMS and
gateway already bind commands to an exchange-session epoch and fencing token.
Those controls do not decide which host owns a session during a process crash or
network partition. Automatic promotion based only on peer-heartbeat loss can
create two order emitters. Replication lag can also cause a replacement to
repeat an already acknowledged command.

## Decision

Aegis-MX separates leadership authority from peer health. An independent
authenticated quorum/witness grants a bounded monotonic lease naming one node,
process epoch, exchange-session epoch, and fencing token. The local HA
coordinator validates but does not mint that authority. A higher token is
accepted only after the witness asserts that the previous owner is fenced and
the recovery replica is caught up by exact sequence and hash.

Promotion always enters `LEADER_RECONCILING`. OMS state, gateway open orders and
executions, positions, risk-service epoch, and journal integrity must agree with
the replicated recovery cursor before emission becomes possible. The final
local emission check writes a hash-chained recovery record to a bounded SPSC
handoff before the gateway call. Command IDs are retained in a fixed ledger;
same-ID/same-hash retries are suppressed and same-ID/different-hash conflicts
latch `UNSAFE`.

Peer loss alone degrades redundancy but does not revoke a still-valid witness
lease. Peer claims of active leadership, expired leases, stale shared memory,
partial site ownership, and missed local heartbeats fence immediately. Journal
or recovery corruption is terminal until restart. Remote-region instances are
restricted to risk-reduction coordination and cannot emit orders.

Leadership and recovery views are integer-only, fixed-layout, preallocated, and
driven by caller-supplied monotonic time. Operator state is published through a
reader-pinned immutable snapshot. State transitions form a bounded hash chain
for asynchronous draining to the append-only journal.

## Consequences

- Two healthy peers cannot safely self-elect from heartbeat observations.
- A witness outage eventually fences the current leader when its lease expires.
- Failover availability depends on replica currency and authoritative venue
  reconciliation; ambiguity intentionally extends downtime.
- A missing standby does not immediately stop a safe leader, but bounded
  recovery backpressure eventually blocks further emissions.
- The witness, physical gateway-session fencing, cross-host recovery transport,
  and licensed venue recovery semantics remain external integration boundaries.
- HA coordination adds no network, disk, allocation, Python, or logging call to
  the final emission check.

## Rejected alternatives

- Heartbeat-only election was rejected because a partition cannot distinguish a
  crashed peer from an isolated live emitter.
- Shared-memory process epoch as a cross-host lease was rejected because its
  ownership and atomic ABI are host-local.
- Resending all live orders after replay was rejected because an acknowledged
  command may already be working at the venue.
- Promoting a lagging replica was rejected because missing order evidence makes
  duplicate prevention unprovable.
- Treating a remote region as an equivalent colocated leader was rejected
  because it cannot preserve local market-data or gateway latency and could act
  on stale state.

## Review triggers

Review this decision before changing witness quorum or lease semantics, allowing
more than one standby consumer, overwriting recovery history, enabling remote
order emission, permitting promotion with lag, changing gateway session
ownership, or adding any live-capable adapter.
