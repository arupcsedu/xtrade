# ADR 0047: Serialize bounded data-root admission and publish immutable manifests atomically

- Status: Accepted
- Date: 2026-09-09
- Owners: Research data platform and safety engineering
- Supersedes: None
- Related: [Forecasting POC contract](../architecture/forecasting-poc-contract.md),
  [point-in-time data contract](../architecture/point-in-time-data-contract.md),
  and [data repository architecture](../architecture/poc-data-repository.md)

## Context

The forecasting POC must operate within a stated 250 GB administrative
allocation while targeting 80 GB, enforcing a 100 GB root ceiling, retaining a
50 GB filesystem reserve, and limiting temporary space to 20 GB. Shared
filesystem free space does not prove a user's entitlement. Concurrent writers,
sparse partial files, conversion peaks, interrupted publication, path
redirection, and automatic cleanup can invalidate an otherwise plausible
capacity calculation or destroy audit evidence.

Source and partition metadata must be immutable, deterministic, and verifiable
before any remote ingestion is implemented. A database or network coordinator
would add an availability dependency to an offline POC and would not make local
filesystem mutations atomic.

## Decision

Use a configurable absolute external root with a fixed versioned layout and a
policy-hash marker. Never allow the root inside the Git worktree. Account using
logical file size so sparse or partially downloaded objects cannot hide their
eventual footprint; report allocated bytes separately.

Serialize admission and the associated publication epoch with a nonblocking
POSIX advisory file lock. Calculate all peaks with checked signed 64-bit decimal
bytes. Require both physical filesystem evidence and complete authoritative
user-quota evidence. Unknown evidence, projection overflow, policy threshold,
or ambiguous filesystem content rejects the operation.

Represent source and partition manifests as strictly decoded canonical JSON.
Compute a SHA-256 over the versioned type and complete payload, use that digest
in the identity, and store it redundantly in the envelope. Verify the object
before publication. Publish with an exclusive synchronized temporary file and
an atomic hard link to the final immutable name. Never overwrite an existing
different manifest. Corrections and replacements are new manifests with
explicit lineage.

Return cleanup candidates only as an operator-approved plan. The repository
contains no deletion operation and performs no network access.

## Consequences

- Admission remains deterministic, local, auditable, and fail-closed during a
  control-plane or network outage.
- A retained lease lets a future writer bind its actual mutation to the same
  serialized accounting epoch.
- Logical-size accounting is intentionally conservative for sparse files.
- Unmanaged mutation by a process that ignores the lock remains detectable but
  cannot be prevented by a userspace library; all future repository writers
  must use the lease.
- An interrupted `.partial` file is visible and blocks retry until an operator
  preserves and resolves it.
- Immutable content may consume more space during corrections; cleanup and
  retention remain explicit operational decisions.
- POSIX lock and hard-link semantics are required. A future object-store backend
  needs a separate conditional-create and consistency ADR.

## Rejected alternatives

- **Use filesystem free space as quota:** it exposes other users' capacity and
  can admit writes beyond this user's allocation.
- **Count allocated blocks only:** sparse and partial objects can evade the
  projected logical footprint.
- **Check then write without a retained fence:** concurrent admissions can both
  pass against the same starting state.
- **Rename with replacement semantics:** it can silently overwrite immutable
  evidence after a race or retry.
- **Delete least-recently-used data automatically:** it can destroy licensed,
  provenance, or incident evidence and silently change datasets.
- **Coordinate through PostgreSQL or an RPC:** it adds an avoidable remote
  dependency and still does not own local filesystem atomicity.
