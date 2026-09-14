# Canonical minute normalization recovery runbook

## Scope

Use this runbook for offline normalization only. It authorizes no download,
forecast, paper order, or live order.

## Preconditions

- Confirm the data root is the configured external POC root, never the Git
  worktree.
- Obtain current authoritative quota evidence and confirm repository admission
  succeeds.
- Verify all requested source manifests and objects with `aegis-data verify`.
- Verify the universe digest, historical reference snapshot, tick snapshot, and
  exact schema versions.
- Stop if mappings or tick sizes are not both known and effective for the
  requested historical interval.

## Interrupted source ingestion

1. Keep the original source object and manifest immutable.
2. Reopen the SQLite spool. Opening runs its integrity check.
3. If the source appears in `processed_sources` with the same object SHA-256,
   no action is required.
4. If it is absent, rerun the same source. The source transaction either
   committed completely or rolled back completely.
5. If its identity names a different hash, quarantine the new object and stop.

## Interrupted partition publication

The final partition manifest is the acceptance marker.

- Quality report only: unaccepted orphan; retain for diagnosis or list in a
  cleanup plan.
- Quality report plus Parquet, no manifest: unaccepted orphan; verify both and
  rerun publication. Identical objects are idempotent.
- Manifest exists: run repository verification and Parquet row-count/codec
  inspection. Never overwrite it.
- Existing path has different bytes: stop with immutable conflict. Create a
  documented correction/replacement lineage only after root-cause review.

## Corruption

1. Stop normalization against the affected spool or partition.
2. Preserve the corrupted original for audit; never repair it in place.
3. Record file size, SHA-256 when readable, source manifests, and error code.
4. Create a new spool and replay verified immutable sources.
5. Compare logical record-sequence, quality-report, and Parquet hashes.
6. Accept output only after a new immutable manifest and full repository verify.

## Conflicting duplicate

Do not choose a value automatically. Record both source manifest IDs and the
economic key, quarantine the affected source object, and investigate source
correction/adjustment semantics. Resume only with explicit correction lineage.

## Current POC limitation

Do not attempt the real two-year conversion with the current Prompt 52
reference snapshot. Historical symbol effectiveness and tick revisions are
missing, so fail-closed rejection is the expected behavior.
