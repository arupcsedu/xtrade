# Bounded POC data repository recovery

Recovery is evidence-preserving and manual. The repository never modifies a
corrupted original and never deletes files to restore quota headroom.

## Immediate response

1. Stop the future writer or ingestion process. Do not start another writer.
2. Capture the exact CLI output, policy marker, allocation evidence, hostname,
   process identities, and UTC time without copying credential material.
3. Run `aegis-data usage` and `aegis-data verify` only if the filesystem is
   stable. Both commands may intentionally fail closed.
4. Preserve corrupt and partial objects in place or make an operator-approved
   forensic copy outside the bounded root. Do not overwrite originals.
5. Escalate ambiguous quota, ownership, or filesystem state before recovery.

## Failure-specific procedure

### Interrupted publication

A file named `tmp/<manifest-id>.manifest.partial` indicates that the manifest
publication did not complete cleanly. Determine whether a matching final
manifest exists. Verify the partial bytes and referenced object independently.
Retain both names as evidence. A retry is intentionally blocked until an
operator documents and moves the partial to an approved evidence location; do
not remove it automatically.

### Corrupt manifest or hash mismatch

Do not edit the manifest or source object. Record the reported path and error
code. Quarantine a copy through a separately approved, admitted operation, then
reconstruct from the authorized source. Publish the result as a new immutable
object and use `CORRECTS` or `REPLACES` lineage. Verification must pass before a
dependent partition may be used.

### Missing lineage or source reference

Recover the exact referenced immutable manifest from the evidence store or
authorized source. Do not rewrite the child reference. If the parent cannot be
recovered, mark the descendant unusable and exclude it through a new dataset
manifest; do not conceal the missing edge.

### Unknown or exhausted quota

Stop writes. Filesystem-wide free capacity does not clear the condition. Obtain
fresh authoritative limit and usage evidence. If the limit is exhausted, use
`cleanup-plan`, review retention and licensing obligations, and execute any
approved cleanup outside this tool. Re-run usage, estimation, and verification.

### Stale lock or writer crash

The operating system releases the advisory lock when a process exits. Confirm
the writer is actually gone and no admission is active. Never delete or replace
`.admission.lock`; initialization validates its identity and link count. Inspect
for partial publication evidence before admitting another writer.

### Policy marker mismatch

Do not rewrite `.aegis-data-root.json`. Confirm the intended binary and policy.
A policy migration requires an ADR, an inventory and integrity snapshot, a
bounded migration plan, and an immutable audit record. Until then, use is
blocked. The only approved v2-to-v3 exception is the guarded `migrate-policy`
operation in ADR 0062; manual marker replacement remains prohibited.

### Symlink, hard link, special file, or cross-device entry

Treat the root as potentially tampered. Stop all writers, preserve metadata,
identify the creator, and review every path component. Restore data only into a
new approved root using verified regular-file copies and newly published
manifests. Do not continue accounting in the ambiguous tree.

## Recovery closure

Recovery is complete only when:

- the exact root and policy marker validate;
- authoritative quota and physical reserve are known;
- no unexplained partial, symlink, hard link, special, or cross-device entry
  remains;
- `aegis-data verify` passes;
- source, correction, and replacement lineage is complete;
- the operator records cause, affected objects, preservation location, actions,
  command outputs, and hashes; and
- no network download or trading capability was enabled as a workaround.
