# Training-readiness runbook

## Preconditions

1. Use `/scratch/djy8hg/env/aegis_mx_contracts`.
2. Require an accepted Prompt 57 manifest with leakage `PASS`.
3. Require a clean Git worktree containing the exact builder implementation.
4. Require the private source approval at
   `/scratch/djy8hg/aegis_mx_poc_data/manifests/approvals/alpaca-iex-academic-approval-v1.json`.
   Confirm it is unexpired and still matches the academic, internal-only,
   one-minute IEX scope, data root, universe hash, and backfill approval hash.
   A malformed identifier or expiry blocks admission but remains represented in
   the machine-readable report as null evidence; it is never treated as valid.
   `/scratch` and its cluster filesystem alias may differ textually; the gate
   accepts them only when both absolute paths resolve to the same target.
5. Confirm current authoritative quota evidence and the 800 GB root, 20 GB
   temporary, and 50 GB reserve gates.
6. Do not add provider credentials or the private approval to the repository.

## Admission

Run from `/scratch/djy8hg/xtrade`:

```bash
export PYTHONPATH=python/research:python/contracts:python/intelligence:python/training:python/model_serving:.
PY=/scratch/djy8hg/env/aegis_mx_contracts/bin/python

$PY tools/check_training_readiness.py
$PY tools/check_training_readiness.py --execute
```

The first command verifies without publishing. The second publishes an
immutable report under
`/scratch/djy8hg/aegis_mx_poc_data/reports/training-readiness/` only if status is
`READY_FOR_INFRASTRUCTURE_VALIDATION`.

## Failure response

- `DIRTY_WORKTREE`: review, test, and commit the exact implementation. Never
  override the result.
- `SOURCE_AUTHORIZATION_MISMATCH`: stop training. Restore or renew the exact
  private approval, rebind it through a new authenticated backfill when its
  identity changes, and rerun admission. Do not edit an accepted backfill in
  place.
- object/hash/count failure: quarantine the affected object and rebuild from
  its immutable source lineage.
- partition-lineage mismatch: reject the dataset; do not combine manifests.
- leakage failure: preserve evidence and correct the builder with a regression
  test.
- insufficient coverage: retain explicit abstention or reduce model scope in a
  new reviewed configuration; never synthesize history.
- unavailable optional feature: exclude it through the readiness feature mask
  or rebuild from an authorized point-in-time source.

## Cleanup

After admission, use `aegis-data cleanup-plan` to review obsolete spools and
quarantine evidence. The application never deletes them automatically.
