# Configuration control and emergency-inhibit runbook

## Preconditions

- Confirm the service health reports `live_trading_capable: false` for this
  build and the intended environment is simulation or paper.
- Obtain short-lived authenticated identities through the approved production
  identity boundary. Keep signing keys in approved HSM/KMS custody; repository
  tests use deterministic synthetic keys only.
- Record the change ticket, exact current hash, target file hash, activation
  time, and author/approver identities. Never paste keys into commands, logs,
  configuration JSON, or tickets.

## Normal change

1. Run `config-service dry-run --config candidate.json` and review every field,
   warning, full SHA-256, and 128-bit version.
2. The author proposes the exact signed candidate with an activation time
   inside its validity interval.
3. A different approver verifies risk limits, venue mode, roles, lineage,
   calendars, sessions, model versions, and the proposed hash before approval.
4. At or after staged time, the activator activates. If the active parent
   changed, stop and rebase a new revision; never override lineage.
5. Distribute asynchronously to edge local storage. Confirm signature,
   revision, hash, readiness, mode, and denial/allow reason in paper shadow
   checks before relying on it.

The complete CLI flag reference is emitted by each command with `-h`. CLI key
files are a local development adapter; production must place authentication and
signing behind approved protected boundaries.

## Control-plane outage or stale cache

The edge continues using its last verified local snapshot until either signed
UTC validity, the snapshot offline age, or the account risk-configuration age
expires. It must then report `CONFIGURATION_CACHE_STALE`,
`RISK_CONFIGURATION_STALE`, or `CONFIGURATION_EXPIRED_OR_CLOCK_UNSAFE` and
block new orders. Do not extend this window by copying or reinstalling the same
snapshot; identical bytes cannot refresh age.

Existing order cancellation and kill processing remain governed by the risk,
OMS, and gateway recovery runbooks. Never weaken freshness to restore service.

## Emergency inhibit

1. Verify the intended scope and target. When uncertain, engage the broader
   safe scope.
2. An authorized kill operator issues a new command ID and strictly increasing
   sequence with a machine-readable reason code.
3. Confirm the signed command is fsynced in the authority audit and each edge
   kill journal before declaring delivery complete.
4. Verify matching new-order checks report `KILL_SWITCH_ENGAGED`.

There is no emergency reset command. Recovery requires diagnosis, reconciliation,
a reviewed configuration revision, distinct approval, staged activation, and
normal edge restart/distribution controls.

## Rollback

Rollback uses the expected-active hash as a compare-and-swap guard and requires
a rollback operator plus a distinct approver. The target must be older, same
identity/environment, and still within signed validity. If no such snapshot
exists, engage a kill and prepare a new forward revision.

The current v1 running-edge distributor rejects revision downgrade. After the
authority rollback, stop the paper edge, stage the authority-selected target in
protected local storage, and reopen it; verify hash and persistent kill state
before readiness. A signed rollback-selection envelope is required before
automated online rollback distribution can be enabled.

## Integrity failure

If audit chain, signature, content hash, epoch, request sequence, or local cache
verification fails:

1. treat the service/edge as unready and block new orders;
2. preserve original files read-only for investigation;
3. engage the independent kill path if its integrity remains established;
4. compare with immutable backups and audit evidence; and
5. restore by copy into a new directory. Never edit the corrupted original.

Related procedures: [live-trading safety](live-trading-safety.md),
[clock failure](clock-failure-runbook.md), and
[model rollback](model-registry-rollback-runbook.md).
