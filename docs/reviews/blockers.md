# Aegis-MX Production Blockers

| Field | Value |
| --- | --- |
| Decision | **STOP / NO-GO** |
| Fresh review | 2026-09-07 |
| Source audit | [Production-readiness audit](production-readiness-audit.md) |
| Root-cause record | [Blocker remediation record](blocker-remediation-2026-09-07.md) |

A blocker closes only with evidence bound to an immutable source revision and the
real environment whose behavior it claims. A synthetic test cannot certify a
licensed protocol, physical fence, or production cluster.

## Open BLOCKERs

| ID | What was remediated | Remaining blocking condition | Minimum closure evidence |
| --- | --- | --- | --- |
| PRD-B002 | Five installed non-live edge processes, strict signed-config verifier, and executable inventory checks | No complete target-site composition, reviewed site overlay, or colocated-host qualification | Signed package; real NIC/PTP/NUMA/NVMe profile; cold-start, watchdog, disk-failure, shutdown, inhibit, and rollback evidence |
| PRD-B003 | Licensed boundary remains explicitly fail-closed; no protocol fabricated | No authorized real feed/reference/order-entry/drop-copy/broker integration | Specification provenance, legal approval, entitlements, conformance matrix, provider certification, capacity/recovery and secret custody |
| PRD-B004 | Ed25519 verifier-only grant fixes caller-asserted authority | No independent witness/consensus, key custody, physical session fence, or multi-host proof | Monotonic witness, physical stale-owner rejection, partition/failover tests across fault domains, certified session takeover |
| PRD-B005 | Canonical mandatory decision evidence is durably accepted before PAPER send and verified after reopen | Not every source/model/decision/risk/order/fill/position record uses the authoritative journal; no fresh-process whole-system reconstruction and external reconciliation | Power-loss tests and exact replay hashes from verified segments plus authoritative drop-copy/open-order reconciliation |
| PRD-B006 | Manifests remain safely non-live and validators disclose ten placeholders | No signed service images, long-running authenticated service contracts, distributor, IAM/CA/KMS/CSI/admission/storage, cluster, or restore proof | Nonzero signed release lock and deployed authorization, outage, backup/restore, upgrade and rollback evidence |

## Closed findings

| ID | Status | Closure evidence |
| --- | --- | --- |
| PRD-B001 | **Closed** | Intended implementation is committed; the audit is committed separately as evidence-only documentation; final worktree is clean |
| PRD-C001 | **Closed** | Exact-frame, expiring authenticated capability; unsafe-state issuance rejection; move-only verifier-created authority consumed at the adapter boundary; tamper/replay/reuse tests |
| PRD-C002 | **Closed** | Canonical full explanation accepted by mandatory bounded journal before `send_order`; saturation and persistent-reopen tests |
| PRD-C003 | **Closed** | Ed25519 signed witness envelope and verifier-only grant; forgery, wrong-root, tamper, lease, replay and rollback tests |

No CRITICAL finding remains open. Closing these code defects does not grant live
authority and does not reduce any remaining blocker.

## Required order of closure

1. Complete the authoritative journal and fresh-process recovery composition.
2. Qualify the exact edge artifact on reviewed target hardware.
3. Deploy authenticated regional/control dependencies with signed artifacts.
4. Complete licensed intake, implementation, certification, and physical HA
   fencing.
5. Close the HIGH release gates, then repeat the hostile audit. Human approval is
   still required and must never enable live trading automatically.
