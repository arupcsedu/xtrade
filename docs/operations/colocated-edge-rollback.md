# Colocated edge rollback

Rollback is an operator-controlled, fail-closed release change. A rollback package is evidence and configuration material; extracting it never installs files, switches a symlink, starts systemd, or authorizes trading.

## Build and verify

```bash
edge-deployment package-rollback --profile-path approved-paper-profile.json \
  --facts reviewed-host-facts.json \
  --source-date-epoch 1600000000 \
  --output dist/edge/aegis-edge-rollback-paper.tar.gz
edge-deployment verify-rollback \
  --package dist/edge/aegis-edge-rollback-paper.tar.gz
```

Store the printed SHA-256 with the release approval record. Build twice with the same source epoch to confirm byte identity.
When `--facts` is supplied, packaging first validates CPU, NUMA, huge-page,
NIC, timestamp, and queue compatibility and embeds the exact reviewed facts.
Base CI bundles omit site facts because checked-in placeholders are not host
authorization.

## Procedure

1. Activate the firm/account/strategy/venue/symbol kill policy as appropriate and write `/run/aegis-mx/trading.inhibit`.
2. Stop `aegis-edge.target`. Confirm no gateway process owns the exchange session and no order-emitting leader is active.
3. Preserve the journal, health records, logs, active configuration, current release target, and failure evidence. Never repair or overwrite the original journal.
4. Verify the chosen rollback archive and the earlier release/artifact signatures. Extract it into a new administrative staging directory with restrictive permissions.
5. Reconcile orders, fills, positions, fencing token, gateway session, and journal recovery. Ambiguous state blocks restart.
6. Render the rollback profile into an empty directory, validate host facts, and compare it to the approved earlier deployment manifest.
7. Atomically switch `/opt/aegis-mx/current` to the already installed and verified earlier release. Do not install over either release directory.
8. Run unit-file verification, configuration signature/expiry checks, clock readiness, journal recovery, and health probes.
9. Start simulation or paper services only after operator approval. Retain the inhibit record until reconciliation and readiness are complete.
10. Record both operators, reason, old/new artifact hashes, configuration hashes, timestamps, reconciliation outcome, and validation evidence in the immutable administrative audit.

If validation fails, stop. Leave the edge inhibited and escalate through the incident-response and failover runbooks. A remote region may reduce risk but is not equivalent to local latency.
