# ADR 0049: Separate no-I/O fetch planning from explicitly authorized bounded execution

- Status: Accepted
- Date: 2026-09-09
- Owners: Research data platform and security engineering
- Supersedes: None
- Storage values superseded by: [ADR 0062](0062-expand-bounded-poc-data-root-to-800-gb.md)
- Related: [Provider-neutral ingestion](../architecture/provider-neutral-ingestion.md),
  [bounded data repository](../architecture/poc-data-repository.md), and
  [source approval](../compliance/forecasting-poc-source-approval.md)

## Context

The forecasting POC needs a common ingestion boundary before any licensed
provider can be considered. A downloader that discovers work while fetching,
retries indefinitely, trusts a filesystem-wide free-space value, or persists
signed URLs can exceed the 100 GB root, leak credentials, or make a replay
depend on timing. A generic transport must not imply rights to access, retain,
or train on a specific dataset.

Interrupted objects also need a clear ownership model. Blind range resume can
join bytes from different object versions, while blind restart can erase useful
recovery evidence. Concurrent callbacks must not publish outside the same
storage-admission epoch.

## Decision

Define a provider-neutral `DataProvider` protocol whose `plan` method is
contractually local-only and whose `fetch_range` method is called only by an
explicit execution operation. `FetchRequest.execute` defaults to false. Bind a
plan to the exact request, sorted source objects, storage estimate, entitlement,
rate policy, and retry policy with a deterministic SHA-256 identity.

Require finite object, total-byte, date, ticker, concurrency, chunk, attempt,
per-call timeout, delay, and metadata bounds. Use process-local fixed-interval
rate limiting and derive retry jitter from a recorded seed, object identity, and
attempt rather than ambient randomness. Execute at most one bounded batch
concurrently and sort terminal results by object identity.

Persist a canonical SHA-256-authenticated checkpoint after every accepted range
and retry transition. Resume only when the staged prefix and all planned object
identity fields verify. Let providers declare either verified-range resume or
complete-object restart. Publish raw or quarantine bytes only through the
retained repository admission lease and never overwrite existing different
content.

Treat entitlements as a separate injected contract. A remote-class provider
requires exact `AUTHORIZED` state, a policy hash, approval-record hash,
network-access authorization, and a nonexpired UTC time. Credentials are
resolved only at execution through an external interface and are never part of
plan identity or persistent metadata.

Implement only filesystem replay, deterministic synthetic minute bars, and an
in-process socket-free HTTP/S3 test double in this phase. Do not implement a
general HTTP client or any provider-specific adapter.

## Consequences

- Operators can review complete work and peak storage before any fetch call.
- Unit and integration tests reproduce retry delays, object ordering, content
  hashes, checkpoints, and manifests without external network access.
- A crash leaves a visible synchronized staged prefix and authenticated
  checkpoint; resume is explicit rather than automatic.
- Object and checkpoint synchronization adds latency, which is acceptable for
  the offline path and measured separately.
- In-process rate limits do not coordinate multiple hosts. A concrete provider
  phase must add an account-wide policy without weakening these local bounds.
- Approval records remain an external administrative dependency. The generic
  interface cannot convert the currently blocked source policy into authority.

## Rejected alternatives

- **Fetch while enumerating:** size and scope are unknown when mutation starts.
- **Enable network providers in the CLI:** no checked-in source has the required
  approval, and a generic endpoint creates an SSRF surface.
- **Use nondeterministic retry jitter:** replay cannot reproduce the attempt
  schedule or compare fault results.
- **Resume from byte count alone:** a changed prefix or source version can
  silently splice objects.
- **Overwrite malformed or corrected inputs:** it destroys provenance and
  prevents independent audit.
- **Put credentials in request URLs or configuration:** identities, audit, and
  exceptions can persist them.
- **Use an unbounded asynchronous task pool:** overload can exceed memory,
  provider rate, and temporary-storage assumptions.
