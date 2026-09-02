# Model registry disable and rollback runbook

## Purpose and authority

Use this runbook when a model artifact, signature, feature contract,
calibration/OOD profile, deployment, or observed behavior is unsafe or
uncertain. Registry actions do not replace the independent risk, market-state,
gateway, strategy, account, or firm kill switches. If trading safety may be
affected, activate the applicable kill switch first and keep it active until all
systems are independently revalidated.

Only authenticated operators acting under the organization's incident and
change policy may use real registry keys. Never paste a private key into a
command line, ticket, chat, log, repository, or runbook. The current CLI accepts
an approved protected key-file path; production HSM/KMS integration remains a
prerequisite.

## Immediate disable

1. Record incident ID, model ID/version, affected environments, detection time,
   operator identity, and observed reason. Preserve logs and journal evidence.
2. Independently inhibit model consumption or trading as required. Do not wait
   for registry diagnosis to activate a safety control.
3. Inspect the signed lineage with a trusted public key. Treat any hash,
   signature, chain, schema, or pointer failure as an integrity incident.
4. Issue `disable` with a bounded, specific reason. Disable is valid even when
   artifact bytes fail their hash because revocation must not depend on the
   suspect object.
5. Verify the returned state is `DISABLED`, approval is `REVOKED`, and no active
   environments remain. Re-run `inspect-lineage` and independently verify every
   consumer has rejected or evicted the version. Keep kill switches active if
   any consumer state is unknown.

## Rollback

Rollback is appropriate only when a prior version is already signed, approved,
deployed, artifact-valid, and exactly feature-compatible with the runtime.

1. Complete the immediate safety/inhibit steps above.
2. Identify the current active pointer and the intended prior semantic version.
   Review its manifest, approval actor/reference, artifact hash, feature schema,
   calibration/OOD evidence, and prior deployment evidence.
3. Obtain a reviewed rollback authorization reference under the applicable
   change policy.
4. Execute `rollback` for the named environment. The operation appends a signed
   `ROLLED_BACK` event, revokes all outgoing-version pointers, and atomically
   restores the selected environment pointer to the prior version.
5. Inspect both lineages and the environment pointer. Verify the outgoing
   version is inactive and approval-revoked. Verify the restored version and
   exact artifact/schema hashes before any downstream activation.
6. Run replay and shadow checks. Restoring a pointer does not authorize live
   trading, bypass risk, recover a stale forecast, or re-enable a gateway.

## Integrity or partial-write failure

- Never edit, truncate, resign, or silently repair an original artifact,
  manifest, event log, or pointer.
- Preserve the registry root read-only and hash a forensic copy.
- If the lifecycle event is durable but its pointer update is missing, keep all
  consumers disabled. Rebuild a pointer only through a separately reviewed
  recovery tool/procedure that verifies the complete event chain and records a
  new signed recovery event; that tool is not implemented in this phase.
- If an event tail is malformed, do not append after it. Escalate as registry
  corruption and recover into a new root from verified immutable objects and
  audited events.
- Loss of signing authority, trust-store disagreement, split brain, storage
  uncertainty, or unknown consumer state means not ready and fail closed.

## Recovery exit criteria

Recovery requires all of the following:

- incident and operator actions are durably recorded;
- immutable hashes, signatures, event chains, feature compatibility, and
  deployment pointers verify;
- replay and shadow evidence for the selected version passes;
- every consumer reports the intended version/generation and has no stale
  forecast from the disabled version;
- independent data, clock, risk, market-state, configuration, fencing, journal,
  and gateway controls are healthy; and
- an authorized operator explicitly completes the normal staged promotion.

There is no direct rollback-to-live path and no automatic production recovery.
