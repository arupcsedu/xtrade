# Edge high availability, fencing, and recovery

| Field | Value |
| --- | --- |
| Status | Implemented reference safety slice; external witness integration required |
| Decision | [ADR-0036](../adr/0036-witness-fenced-edge-failover.md) |
| Operations | [Edge failover](../operations/edge-failover-runbook.md), [partial colocation outage](../operations/partial-colocation-outage-runbook.md) |
| Tests | [HA testing](../testing/high-availability-testing.md) |

## Safety outcome

`cpp/high_availability` owns the process-local leadership decision and bounded
recovery handoff for critical colocated edge services. It does not elect a
leader by itself and does not contain a network consensus implementation. It
accepts only an Ed25519-verified, quorum-confirmed fencing capability from an
independent witness boundary. Verification binds signer key identity, trust-root
identity, lease, session, node, process epoch, exchange epoch and monotonic token;
the coordinator cannot consume the caller-controlled wire structure directly.
Production composition must make the grant's
monotonically increasing token the gateway-session fencing token as well as the
OMS authority token.

An order emission is possible only while all of these facts are simultaneously
true:

- the local process owns the expected process epoch and has not missed its
  monotonic heartbeat deadline;
- a current witness lease names the exact node, process epoch, exchange-session
  epoch, and fencing token;
- the node is colocated, not a remote-region recovery worker;
- OMS, gateway, positions, risk, journal, and the recovery stream have completed
  explicit reconciliation for the same authority;
- gateway, risk, journal, and shared-memory dependencies remain healthy;
- no live peer claims leadership; and
- the command identity was durably journaled by its owner and accepted into the
  bounded recovery stream before the gateway call.

The coordinator returns an `EmissionDecision`; it never sends an order. The
existing OMS, risk engine, and gateway final safety gate remain mandatory.

## Components and ownership

| Component | Responsibility | Explicit non-responsibility |
| --- | --- | --- |
| `EdgeHaCoordinator` | Validate process epoch, witness lease, service health, reconciliation evidence, and final emission authority | Leader election, network RPC, exchange protocol, order construction |
| `FencingGrantVerifier` | Verify witness signature, configured signer/trust root, exact scope and lease; create the only grant type accepted by the coordinator | Consensus, key custody, physical session revocation |
| `BoundedRecoveryStream` | Preallocated SPSC hash chain, producer/replica cursors, command identity ledger, exact authority rotation, overload failure | Durable disk journal, multi-region consensus, retransmission transport |
| `LeadershipSnapshotStore` | Atomic immutable operator/readiness snapshot | Durable transition storage |
| HA transition journal | Fixed-capacity hash-chained state evidence for asynchronous journal draining | In-place overwrite or silent wraparound |

The recovery stream is a transport-neutral reference implementation. A
production transport may replicate the same fixed records, but it must preserve
sequence, previous hash, record hash, authority, bounded backpressure, and
acknowledged replica cursor semantics. It may not acknowledge a record before
the standby has applied it.

The SPSC payload edge is release/acquire: the producer finishes a record before
publishing the ring tail, and the consumer finishes copying before releasing the
slot. Producer and replica sequence cursors are separate atomics. Each owner
writes its hash before release-publishing its sequence; monitoring acquires the
sequence before sampling the hash. Producer and replica command ledgers are
single-owner fixed arrays. Authority rotation is a quiescent composition-root
operation permitted only when both cursors and hashes match; it is not safe to
race rotation with append or apply.

## Deterministic state machine

| Current state | Input | Next state | Can emit? |
| --- | --- | --- | --- |
| `STARTING` | valid process-epoch claim | `STANDBY_SYNCHRONIZING` | No |
| `STARTING` | conflict/corruption | `FENCED` | No |
| `STANDBY_SYNCHRONIZING` | recovery hash/sequence caught up | `HOT_STANDBY` | No |
| `HOT_STANDBY` or `FENCED` | higher current grant, prior owner fenced, stream caught up | `LEADER_RECONCILING` | No |
| `LEADER_RECONCILING` | complete matching evidence for all services | `ACTIVE_LEADER` or `ACTIVE_DEGRADED` | Yes |
| `ACTIVE_LEADER` | standby failure only; witness lease and local safety healthy | `ACTIVE_DEGRADED` | Yes, until recovery queue capacity is exhausted |
| active state | gateway disconnect or risk-service epoch change | `LEADER_RECONCILING` | No |
| active state | peer claims active, lease expires, shared memory is stale, or site ownership is ambiguous | `FENCED` | No |
| any running state | journal/recovery corruption or command identity conflict | `UNSAFE` | No |
| any running state | orderly shutdown | `STOPPED` | No |

`FENCED` and `UNSAFE` are latched against ordinary health updates. Leaving
`FENCED` requires a higher witness token and full reconciliation. `UNSAFE`
requires process restart and evidence preservation. An equal-token lease renewal
can extend a valid lease but cannot clear either state.

## Recovery and duplicate prevention

Every emission record binds the canonical command ID and command hash to source
journal sequence/hash, session, exchange-session epoch, fencing token, and
process-monotonic time. Reusing the command ID with the same hash returns
`duplicate_suppressed`; reusing it with a different hash corrupts identity and
latches the recovery path `UNSAFE`.

A standby cannot accept a higher token while its applied sequence/hash differs
from the publisher sequence/hash. After token rotation it must restore the OMS,
gateway open-order view, positions/fills, risk epoch, and verified journal. The
existing OMS changes live orders to `UNKNOWN_RECOVERY`; an observed working or
acknowledged order is reconciled without producing another new-order command.
Silence or ambiguous absence is never evidence that resending is safe.

## Failure behavior

| Failure | Required behavior |
| --- | --- |
| Leader crash | Witness fences the old gateway/session, advances token, caught-up standby reconciles before promotion |
| Standby crash | Leader reports `ACTIVE_DEGRADED`; it cannot fail over and eventually fails closed if the bounded recovery queue fills |
| Network partition | No lease renewal; lease expiry fences. A live peer claiming active fences immediately |
| Delayed heartbeat | A process cannot revive its leadership after exceeding its local heartbeat timeout |
| Stale shared memory | Fence and allocate/validate a new region epoch before recovery |
| Gateway disconnect | Enter reconciliation; reconnect and query authoritative open orders before activation |
| Risk restart | A changed risk-service epoch forces new risk/position reconciliation |
| Journal failure | Enter terminal `UNSAFE`; preserve files and restart through journal recovery |
| Partial colocation outage | Fence new orders; follow the partial-outage runbook |
| Remote-region survival | `RISK_REDUCTION_ONLY`; no order emission and no colocation-latency claim |

## Operator-visible contract

The atomic `LeadershipSnapshot` exposes state/reason, node/process/session
ownership, fencing token and lease expiry, local heartbeat, risk epoch,
replication cursors, transition sequence, configuration hash, readiness to emit,
standby availability, reconciliation requirement, and an operator-attention
flag. Exporters run outside the strict hot path. Alerts must link to the two HA
runbooks above.

## External production boundary

The repository does not fabricate a distributed consensus service, witness
protocol, broker/exchange session takeover semantics, or cross-host transport.
Before production use, an organization must integrate an authenticated
quorum/witness, ensure the old network session is physically fenced, certify the
venue-specific recovery query, qualify replication capacity on target hardware,
and prove that the gateway rejects stale tokens. Without those external facts,
the coordinator remains simulation/paper infrastructure and cannot authorize
live trading.
