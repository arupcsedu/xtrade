# OMS recovery runbook

## Trigger and immediate posture

Use this runbook after process restart, active-leader token change, unknown
external order, execution conflict, receipt conflict, snapshot publication
failure, journal exhaustion, or an impossible venue response. Stop downstream
command consumption first. Do not retransmit historical commands and do not
infer absence from silence.

## Evidence collection

Record the build version, configuration hash, exchange-session epoch, active
fencing token, OMS health/invariant, snapshot and journal sequence/hash, process
epoch, last normalized venue sequence, and operator identity. Preserve the
binary journal and canonical audit records read-only. A hash-chain or ABI/
configuration failure is not recoverable by skipping records.

## Deterministic restart

1. Start a fresh OMS with the exact configuration and a new empty target
   journal. Keep command consumption disabled.
2. Load or inspect the source journal and require header, ABI, configuration,
   record-hash, chain-hash, and trailing-data checks to pass.
3. Run replay recovery. Any result or snapshot divergence is `UNSAFE`; preserve
   evidence and escalate.
4. Confirm every formerly live order is `UNKNOWN_RECOVERY`, OMS readiness is
   false, and replay returned no gateway command.
5. Obtain a complete, authority-fenced normalized open-order/execution view from
   an authorized synthetic or licensed adapter. Partial or conflicting results
   are insufficient.
6. Apply explicit recovery observations. An ambiguous absence must remain
   unknown. Reconcile every required primary/drop-copy execution identity.
7. Require zero unknown orders, zero unreconciled required fills, a valid
   snapshot/hash chain, current authority, and `HEALTHY` before enabling the
   downstream synthetic command consumer.

## Split brain and authority rotation

Never reuse or reduce a fencing token within an exchange-session epoch. A lower
token or different epoch is a split-brain signal. Isolate both leaders, disable
consumption, establish one operator-authorized epoch/token, and perform full
recovery. Token rotation intentionally unknowns all live orders.

## Capacity and corruption

Journal, order, execution, or receipt capacity cannot be cleared in place.
Disable consumption, preserve the source, start a correctly capacity-qualified
process, replay, and reconcile. Corrupt or configuration-mismatched state is not
manually edited. Restore from an independently verified earlier segment and
reconcile the complete external state, or remain inhibited.

## Exit criteria

Exit only with an auditable activation record, verified journal and snapshot
hashes, no unknown or unreconciled state, current risk/configuration/clock/data
health, one valid leader, and successful simulation reconciliation. This
runbook does not authorize paper or live trading.

