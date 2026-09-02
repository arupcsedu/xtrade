# Shadow/canary model deployment and rollback runbook

## Authority and invariants

This runbook governs model lifecycle only. A registry `PRODUCTION` version or
canary admission does not authorize live trading. If model behavior might affect
trading safety, activate the applicable independent symbol/strategy/account/
firm kill switch first. Do not wait for statistical confirmation.

Operators must use approved external identities, protected registry/deployment
signing keys, and organization change records. Never paste a key into a command,
ticket, chat, repository, dashboard, or log. Keep the lifecycle signer in the
deployment coordinator boundary; inference and strategy processes must not have
it.

## Before shadow

1. Verify candidate and comparison artifacts, manifests, signatures, exact
   feature schema, lineage, calibration/OOD profiles, point-in-time dataset, and
   replay evidence.
2. Review the signed deployment configuration. Confirm all ten metrics, minimum
   samples, confidence value, regimes, instrument/strategy allowlists, capital,
   order rate, risk age, model versions, environments, approver, and rollback
   target/identity. Independently recompute its SHA-256 and verify its signature.
3. Confirm the production comparison version is currently approved and in model
   lifecycle `PRODUCTION`. Confirm the candidate is approved and in `SHADOW`.
4. Verify the append-only deployment audit is writable, fsync-capable, private,
   monitored, and on backed-up control storage. A missing/corrupt audit is not a
   reason to start fresh.
5. Confirm dashboards and alerts cover coordinator state, sample sufficiency,
   confidence bound versus threshold, triggers, rejection reasons, deadline
   misses, and rollback count.

## Shadow review and canary approval

1. Confirm every accepted pair has an identical feature-snapshot ID/hash and
   exchange as-of time. Investigate any blocked or rejected observation.
2. Review each metric in each required regime. Do not substitute an aggregate,
   normal-regime-only result, raw accuracy, or aggregate P&L.
3. Confirm minimum samples are met and no upper confidence bound breaches its
   signed threshold. Review implementation shortfall, P&L attribution anomaly,
   and risk pressure separately.
4. A second authorized operator, different from the configuration approver,
   records reason, external approval reference, and exact configuration hash.
   There is no unattended canary promotion.
5. Verify the registry candidate lifecycle is `CANARY`, the activation audit is
   durable, and scope limits displayed by the controller match the signed file.

## Automatic rollback response

When state enters `ROLLBACK_PENDING`, `ROLLED_BACK`, `DISABLED_FAIL_CLOSED`, or
`DISABLE_FAILED_FAIL_CLOSED`:

1. Treat the candidate as unsafe. Activate independent kill switches if any
   consumer or trading state is uncertain.
2. Record incident ID, UTC detection time, deployment/configuration hashes,
   model versions, regime, metric, sample count, mean, confidence interval,
   threshold, affected scope, and operator identity.
3. Verify candidate forecasts are evicted/rejected and no new candidate scope
   admission succeeds. Risk and OMS must continue operating without the model.
4. For `ROLLED_BACK`, verify the signed registry event and canary pointer select
   the prior exact compatible approved version. Inspect both lineages and retain
   the candidate disabled from consumption.
5. For `DISABLED_FAIL_CLOSED`, rollback itself failed. Verify the candidate has
   a signed global `DISABLED` event and no active environment. Preserve the
   suspect registry root and deployment audit read-only; follow the
   [registry rollback runbook](model-registry-rollback-runbook.md).
6. For `DISABLE_FAILED_FAIL_CLOSED`, neither registry mutation succeeded. The
   coordinator is locally inhibited but other consumers may still see a canary
   pointer. Immediately activate independent kill/model-disable controls,
   isolate the control plane, and treat the registry as an integrity incident.
7. Preserve canonical forecast, feature, market-state, risk, order/fill, and
   deployment journal evidence. Never edit or repair an original corrupted log.

## Audit or restart failure

- An audit append failure during canary triggers rollback. Do not redirect to an
  unverified empty file or suppress fsync errors.
- An active canary that cannot read, verify, or match its prior audit must remain
  unavailable. Do not reset sample counts, sequences, or rate limits.
- A configuration hash, lifecycle, model version, feature schema, or recovered
  run-state mismatch is an integrity incident.
- Preserve the original files, create a forensic copy, and recover only into a
  new path under a reviewed procedure. No repair-in-place tool exists.

## Recovery exit criteria

Recovery requires all of the following:

- incident evidence and operator actions are durable;
- registry artifacts, signatures, chains, pointers, deployment configuration,
  and deployment audit verify;
- the prior version passes deterministic replay and fresh shadow comparison;
- all required regime/metric samples and reviewed confidence policy are met;
- risk, market state, books, clocks, journals, positions, fencing, and kill
  controls are independently healthy; and
- a second authorized operator starts a new signed staged deployment.

Never resume a rolled-back canary in place. There is no automatic promotion or
rollback-to-live path.
