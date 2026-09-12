# Provider-neutral bounded ingestion

| Field | Value |
| --- | --- |
| Status | Implemented for local fixtures and socket-free mocks |
| Contract version | `1.0.0` |
| Deterministic seed | `20260831` |
| Runtime class | Offline/asynchronous; never a trading hot path |
| Real provider adapters | Not implemented |
| Remote source authority | None in checked-in policy |
| Decision | [ADR 0049](../adr/0049-provider-neutral-bounded-ingestion.md) |

## Boundary

The ingestion package plans and lands immutable source objects in the bounded
POC data repository. It cannot normalize market data, build training datasets,
run a model, publish a forecast, or call risk, OMS, routing, or gateway code.
The installed CLI exposes only the synthetic minute-bar provider and a
filesystem replay provider. The HTTP/S3-like provider is an in-process,
socket-free test double and is not exposed by the CLI.

No provider-specific market-data, SEC, GDELT, ALFRED, or symbol-directory
adapter exists. The checked-in source policy keeps every remote source
disabled. Adding any such adapter requires the exact dataset approval described
by the [source authorization runbook](../operations/data-source-authorization-runbook.md).

## Contracts and ownership

| Contract | Purpose and invariant |
| --- | --- |
| `DataProvider` | Metadata planning is local-only; bounded range fetch and payload validation are explicit |
| `FetchRequest` | Exact provider, dataset, date, ticker, universe hash, seed, call timeout, and resource bounds; `execute=false` by default |
| `SourceObject` | Credential-free locator, immutable source version, expected size/hash, record count, timestamp aggregate, and lineage |
| `FetchPlan` | Sorted de-duplicated objects, total size, storage peak, provider health, entitlement, rate and retry policies |
| `FetchResult` | Terminal per-object outcomes, attempts, recorded retry delays, hashes, paths, and whether a remote-class boundary was exercised |
| `DownloadCheckpoint` | Canonical authenticated progress record bound to plan, request, object, version, byte prefix, attempt, and sequence |
| `RateLimitPolicy` | Fixed-interval request starts with a finite maximum wait |
| `RetryPolicy` | Finite exponential delays with SHA-256-derived jitter from the recorded seed and object ID |
| `ProviderHealthState` | `UNKNOWN`, `HEALTHY`, `DEGRADED`, or `UNAVAILABLE`; unavailable rejects planning and only healthy permits execution |
| `DataEntitlementState` | Closed unknown/disabled/review/blocked/expired/denied/local-test/authorized states; remote execution requires exact authorized evidence |

Persistent accepted objects use the existing canonical `SourceManifest` and
`data-manifest-v1` schema. Checkpoints use a closed field set, canonical JSON,
schema version `1.0.0`, and a redundant SHA-256. Runtime readers validate
semantic bounds in addition to the JSON shape. Credentials and signed URLs are
not valid source locators or persisted fields.

## Planning and execution

Planning performs these steps without calling `fetch_range`:

1. Match request, provider, dataset, and entitlement identities.
2. Read local provider metadata and health.
3. Reject an empty, excessive, or out-of-scope object set.
4. De-duplicate byte-identical source descriptions and reject identity
   collisions.
5. Check per-object, total-size, concurrency, chunk, and signed-64-bit bounds.
6. Calculate output plus worst concurrent partial/checkpoint space.
7. Ask the repository for admission using authoritative quota and filesystem
   reserve evidence.

Execution is a separate call and returns `PLANNED` without mutation unless the
request was created with `execute=true`. An executable plan is regenerated and
compared immediately before use, and health must still be `HEALTHY`. Execution
then validates the UTC entitlement,
obtains any credential through the injected secret interface, acquires the
repository admission lease, and processes batches no larger than
`maximum_concurrency`.

Each range response must begin at the requested offset, retain the planned
object version and total size, remain within the chunk bound, make progress,
and set completion consistently. The staged prefix is synchronized before an
authenticated checkpoint is atomically replaced. A verified complete object
is content-hashed, semantically validated, and published to `raw/` with its
source manifest. Existing identical content is idempotent; different content is
never overwritten.

## Resume, correction, and failure behavior

`VERIFIED_RANGE` resume accepts a checkpoint only when its plan, request,
provider, dataset, object, locator hash, source version, expected size, staged
length, and staged-prefix SHA-256 all match. `COMPLETE_OBJECT_RESTART` retains
the durable evidence but truncates and synchronizes the staging file before
starting again at byte zero. Ordinary `fetch` refuses an existing checkpoint;
the operator must choose `resume-fetch` explicitly.

Hash, schema, count, malformed-object, and changed-object failures never become
raw data. Received bytes are moved through the repository publication boundary
to `quarantine/` when they are complete enough to preserve as evidence. Failed
publication, unknown size, quota denial, checkpoint corruption, or ambiguous
state fails closed. Correction and replacement relationships are copied into
the immutable source manifest and never rewrite a parent object.

A cooperative shutdown stops new batches, lets current bounded calls return,
and retains synchronized checkpoints. Per-object outcomes are sorted by object
identity so worker scheduling cannot change the result ordering.

## Credentials and logging

Providers declare only an external credential reference. The default
environment adapter resolves a bounded environment variable at execution time;
`SecretValue` has non-printing string and representation methods. Known secret
values, credential-like mapping keys, URL user information, and sensitive query
parameters are redacted before result or exception publication. CLI arguments
do not accept secret values.

Provider implementations must still avoid placing credentials in locators,
metadata, exception types, or third-party client debug logs. Redaction is a
containment layer, not authorization to log secrets.

## Concurrency assumptions and limitations

- This is an allocating offline path. It is not suitable for the colocated hot
  loop.
- Concurrency is bounded by a fixed-size batch executor, not an unbounded work
  queue.
- Rate limiting is process-local. A future real provider adapter must also
  respect account-wide limits and server responses.
- Each call receives a positive timeout capped at 300 seconds. A concrete
  transport must enforce it internally because Python cannot safely terminate a
  provider thread that violates the interface.
- The repository lease fences cooperating local writers. It does not coordinate
  remote object ownership or malicious processes.
- Generic verified ranges do not imply that a provider supports stable byte
  ranges. Provider-specific adapters must select the correct resume mode from
  licensed documentation.
- Redirect, endpoint, TLS, pagination, compression, and SSRF policies belong to
  a future concrete HTTP transport. No such transport exists in this phase.
