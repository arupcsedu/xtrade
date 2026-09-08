# Disaster-recovery runbook

## Safety objective

Recover trustworthy evidence and non-live service capability without duplicate
orders or invented state. A remote region may preserve control and analysis but
must not claim colocated latency or order authority. Site-specific RTO/RPO values,
backup destinations, encryption keys, contacts, and recovery priorities remain
approval-controlled external records.

## Declare and contain

1. Appoint the incident commander and evidence custodian; record detection UTC,
   scope, last known process monotonic state, sites, identities, and open orders.
2. Engage firm kill and isolate all gateway routes/sessions. Ambiguous physical
   ownership means no emitter is authorized.
3. Stop retention and automated cleanup. Preserve hosts, disks, journals, shared
   memory metadata, control/model audits, logs, manifests, SBOM/provenance, and
   backup catalog read-only where possible.
4. Revoke compromised identities and signing trust. Do not reuse a process,
   writer, authority, leader, or exchange-session epoch.

## Select a recovery point

1. Inventory immutable backups and replicas by source, creation/receipt time,
   encryption/signature, schema, configuration, model, segment, sequence, and
   content hash.
2. Verify the selected chain from its trust root. Missing, corrupt, reordered, or
   unverifiable intervals are declared data loss and keep trading inhibited.
3. Use copy-only journal repair for a verified prefix. Never modify or replace the
   corrupted original.
4. Record explicit recovery-point and recovery-time observations; do not claim an
   RPO/RTO objective that has not been approved and measured at the target site.

## Restore in isolation

1. Provision an isolated `SIMULATION` environment from exact signed artifacts.
   Confirm no venue route or credential is present.
2. Restore identity/configuration/model metadata, then verified journals and
   analytical derivatives. Derivatives never override the journal.
3. Reconstruct books, features, model outputs where recorded, decisions, risk,
   OMS, fills, positions, and P&L. Compare deterministic replay hashes.
4. Reconcile external open-order, execution, drop-copy, cash, and position state
   only through authorized adapters and custodians. Unknown remains unsafe.
5. Allocate new epochs and prove exactly one fenced leader. Exercise clock, feed,
   halt, gateway, journal, risk, model, and split-brain fault recovery.
6. Run security validation, dependency/SBOM verification, restore tests,
   full-system PAPER acceptance, soak sampling, and the operator kill drill.

## Return to service

Return only to PAPER after every authoritative owner signs its exit criteria,
audit custody is complete, no state remains unknown, and the incident commander
approves the new non-live session. Keep the firm kill and startup inhibit until
that point. Disaster recovery cannot satisfy or bypass the
[production activation checklist](production-activation-checklist.md).
