# ADR 0062: Expand the bounded forecasting POC data root to 800 GB

- Status: Accepted
- Date: 2026-09-13
- Owners: Research data platform and safety engineering
- Supersedes in part: Storage-envelope values in ADRs 0045, 0047, 0049, and
  0050
- Related: [Forecasting POC contract](../architecture/forecasting-poc-contract.md),
  [data repository architecture](../architecture/poc-data-repository.md), and
  [storage operations](../operations/poc-data-storage.md)

## Context

The verified scratch allocation is 10 TiB, while storage policy v2 stops new
writes at an 80 GB review threshold and has a 100 GB hard ceiling. The operator
has expanded the project-specific storage allowance to 800 decimal GB. Merely
raising the hard ceiling would be ineffective because the target threshold is
also fail-closed.

The existing populated data root is cryptographically bound to policy v2.
Silently accepting its marker under new semantics or editing it without
preserving evidence would break auditability and rollback.

## Decision

Storage policy v3 sets both the effective target and hard data-root ceiling to
`800,000,000,000` decimal bytes. The 10 TiB administrative allocation, 50 GB
minimum filesystem reserve, 20 GB temporary-workspace ceiling, complete quota
evidence, serialized admission, and no-automatic-deletion rules remain
unchanged. The increase grants storage capacity only; it does not authorize a
provider, dataset, date range, download, redistribution, training purpose, or
live trading.

Existing v1 and v2 schemas remain available for immutable evidence. A v2 root
is migrated only by the dry-run-by-default `aegis-data migrate-policy` command.
Execution requires the exact reviewed v2 policy SHA-256 and `--execute`. The
command holds the admission fence, archives the exact v2 marker read-only,
publishes a content-addressed migration record, rechecks the source marker, and
atomically installs the v3 marker. An unknown marker, conflicting evidence,
stale staged marker, unsafe path, or concurrent writer fails closed.

Feature-dataset manifest v1 accepts either historical 100 GB evidence or new
800 GB evidence so already published, self-hashed manifests remain valid. New
builders emit 800 GB. No historical manifest is rewritten.

## Consequences

- New admitted operations may grow the configured root to 800 GB, subject to
  current quota, reserve, temporary-space, source-specific, and request bounds.
- The previous 80 GB planning envelope remains useful historical sizing
  evidence but is no longer the admission threshold.
- Existing source-specific caps, including SEC, GDELT, and ALFRED limits, do
  not increase automatically.
- Rollback evidence is the archived v2 marker and migration record. Reverting
  the operational marker in place is prohibited; a reviewed successor
  migration or verified-copy rollback is required.
- Storage policy changes remain unrelated to trading authorization.
