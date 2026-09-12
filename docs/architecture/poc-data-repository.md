# Bounded forecasting POC data repository

| Field | Value |
| --- | --- |
| Status | Implemented for offline POC use |
| Schema version | `1.0.0` |
| Default root | `/scratch/djy8hg/aegis_mx_poc_data` |
| Network behavior | Repository methods perform none; provider-neutral ingestion is a separate caller |
| Deletion behavior | Operator plan only; never automatic |
| Decision | [ADR 0047](../adr/0047-bounded-data-repository-admission.md) |

## Boundary and invariants

The data repository is an offline storage-control boundary for ingestion,
canonicalization, dataset construction, model artifacts, and reports. It does
not fetch data, train a model, publish a forecast, or call any trading component.
The implementation is in
`python/research/aegis_mx_research/data_repository.py`; the read-only
administrative CLI is `aegis-data`.

All policy quantities are exact integer bytes. POC-root limits use decimal GB;
the scratch soft quota preserves the authoritative binary value. Reports show
binary GiB separately and never substitute rounded display values in admission
calculations.

| Control | Decimal-byte value | Admission rule |
| --- | ---: | --- |
| Administrative scratch allocation | 10,995,116,277,760 | Fixed to the verified 10 TiB `hdquota -s` soft limit; effective quota is the smaller of policy and current evidence |
| Target root | 80,000,000,000 | Crossing requires review and is denied |
| Hard root | 100,000,000,000 | Crossing is denied |
| Minimum filesystem reserve | 50,000,000,000 | Projected free space below this value is denied |
| Temporary workspace | 20,000,000,000 | Existing plus projected temporary and retry bytes may not cross it |

The administrative scratch allocation is fixed. A reviewed runtime policy may lower
the target, hard, or temporary limits or increase the reserve, but validation
rejects any change that weakens this envelope or makes its limits inconsistent.

Filesystem capacity and user entitlement are deliberately separate. A
successful `statvfs` probe establishes only the physical reserve. Admission
also requires complete, explicitly authoritative quota evidence containing a
limit, current use, source, and UTC observation time. Unknown projections,
quota, or filesystem capacity fail closed.

Storage policy v2 corrects the original 250 GB assumption, which described the
home allocation rather than personal scratch. Existing v1 roots remain bound
to their immutable v1 marker and require the copy migration in
[ADR 0050](../adr/0050-authoritative-scratch-quota-correction.md); policy
markers are never edited in place.

## Storage layout

The root is configurable and must be absolute and outside the Git worktree.
Initialization creates or validates these fixed areas:

| Area | Purpose |
| --- | --- |
| `quarantine/` | Rejected or unresolved source objects pending review |
| `raw/` | Immutable authorized source objects |
| `canonical/` | Validated canonical partitions |
| `derived/` | Reproducible features and session summaries |
| `datasets/` | Point-in-time training/evaluation datasets |
| `manifests/` | Content-addressed source and partition manifests |
| `models/` | Signed or candidate offline model artifacts |
| `reports/` | Evaluation and operational evidence |
| `tmp/` | Bounded partial publication and operation workspace |

The `.aegis-data-root.json` marker binds the root to the exact policy hash. A
single `.admission.lock` provides a nonblocking process fence. A policy change
does not silently reinterpret an existing root: the marker mismatch is an
explicit error requiring a documented migration.

## Admission sequence

1. Acquire the nonblocking repository writer fence.
2. Inspect every entry without following symbolic links.
3. Count logical bytes, including sparse extents, partial downloads, and all
   temporary objects. Allocated bytes are reported but not used to evade limits.
4. Reject symlinks, hard-linked files, special files, cross-device entries, and
   files that change while inspected.
5. Calculate projected root, temporary, quota, and filesystem-reserve peaks
   with checked signed 64-bit arithmetic.
6. Return all deterministic denial reason codes, or retain the lease for the
   admitted write epoch.

The caller must supply output, temporary, and retry-overhead bounds before any
mutation. Admission is not a reservation service across unrelated processes;
the retained lease is the required fence for atomic manifest publication.

## Immutable manifests

`SourceManifest` records source identity and version, storage path, object
SHA-256, decimal size, record count, schema identity, the complete aggregate
point-in-time range, universe snapshot SHA-256, and correction/replacement
lineage. `PartitionManifest` replaces source-object identity with a dataset and
partition key and binds a canonical sorted set of source-manifest identities.

The canonical JSON body is serialized with sorted keys, ASCII encoding, and no
insignificant whitespace. Its SHA-256 determines both the `manifest_id` and the
redundant `manifest_sha256`. Dataset IDs are similarly derived from a sorted,
unique partition set plus universe, calendar, and schema versions. The
[manifest schema](../../schemas/data-manifest-v1.schema.json) documents the
wire representation; the implementation performs stricter semantic and hash
validation without relying on a network schema resolver.

Publication verifies the referenced object first, writes and synchronizes an
exclusive `.partial` file, creates the final name with an atomic hard link,
synchronizes its directory, then removes the temporary name. Existing identical
bytes make retry idempotent. Existing different bytes, prior partials, or a
concurrent winner fail explicitly. An original object or manifest is never
overwritten.

## Verification and audit

`verify` authenticates manifest encodings and filenames, object sizes and
SHA-256 values, partition source references, and correction/replacement lineage.
Every issue contains a stable code, relative path, and bounded detail. It does
not repair or mutate the repository.

CLI output couples every report to a structured storage audit record containing
the operation, outcome, UTC wall time, root, policy hash, evidence hash, reason
codes, and an invariant `network_access_performed=false`. See the
[audit schema](../../schemas/data-storage-audit-v1.schema.json).

## Concurrency and failure containment

Only admission and manifest publication are serialized. Read-only use and
verification do not claim a consistent snapshot while another process mutates
unmanaged files; callers must use the admission lease for all future repository
writes. No method deletes data, invokes a shell, opens a socket, or reads a
credential. Recovery after a visible partial publication is an operator action
described in the [recovery runbook](../operations/poc-data-recovery.md).

The bounded-storage implementation is not a remote-source authorization. The
separate [provider-neutral ingestion boundary](provider-neutral-ingestion.md)
currently exposes only local providers and a socket-free test double. Real
download work remains blocked by the
[licensed integration boundaries](licensed-integration-boundaries.md) and the
Prompt 48 approval contract.
