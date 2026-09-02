# Model registry and signed artifact lifecycle

## Boundary

The model registry is an asynchronous Go control-plane component. It registers,
validates, approves, deploys, disables, rolls back, and inspects immutable model
artifacts. It cannot publish forecasts or create orders. The C++
`ModelRegistryClient` remains an adapter interface used outside inference; a
runner is constructed from a resolved metadata snapshot and never calls this
registry on the hot path.

```text
training output
  | artifact bytes + point-in-time manifest + ordered feature schema
  v
signed immutable manifest ----> content-addressed artifact
  |                                      |
  +---- exact hash/schema verification --+
                       |
                       v
signed append-only lifecycle events ---> signed deployment pointer
                       |
                       +--> offline activation snapshot (future adapter)
```

The persisted v1 layout is:

```text
registry-root/
  artifacts/<artifact-sha256>.bin
  manifests/<model-id>/<semantic-version>.json
  events/<model-id>/<semantic-version>.jsonl
  deployments/<environment>/<model-id>.json
  .registry.lock
```

Artifact and manifest paths are immutable. The event file is append-only and
hash chained. A deployment pointer is a signed, atomically replaced
materialized view; it can be rejected or rebuilt from verified events and
explicit operator recovery evidence.

## Manifest contract

An `ArtifactManifest` stores:

- stable model ID, canonical semantic version, and optional parent version;
- point-in-time training dataset ID, manifest SHA-256, `as_known_at` UTC
  nanoseconds, split policy, and deterministic seed;
- exact ordered feature names, integer types, units, scales, required flags,
  schema identity/version, and canonical SHA-256;
- full source commit, dependency-lock digest, and training-configuration digest;
- integer fixed-point metrics with their dataset and unit;
- calibration-data and OOD-profile digests;
- artifact SHA-256 and UTC creation timestamp; and
- schema version and Ed25519 signer identity/signature.

The registry stores references and digests, not licensed datasets, provider
payloads, source documents, credentials, or dependency contents. Floating-point
feature types are not accepted by v1. Metric evidence uses signed integer PPM or
another explicitly named integer unit.

## Lifecycle and authorization

| Current state | Operation | Next state | Additional gate |
| --- | --- | --- | --- |
| none | `register` | `DRAFT` | valid signature and artifact hash |
| `DRAFT` | `validate --stage trained` | `TRAINED` | exact runtime feature schema |
| `TRAINED` | `validate --stage offline` | `OFFLINE_VALIDATED` | immutable bytes reverified |
| `OFFLINE_VALIDATED` | `validate --stage replay` | `REPLAY_VALIDATED` | immutable bytes reverified |
| `REPLAY_VALIDATED` | `approve` | `REPLAY_VALIDATED` | approval actor and external reference |
| approved `REPLAY_VALIDATED` | `deploy-shadow` | `SHADOW` | named environment |
| `SHADOW` | coordinator-backed `promote-canary` | `CANARY` | complete paired regime evidence, second approval, configuration hash/reference |
| `CANARY` | `promote --target LIMITED_RISK` | `LIMITED_RISK` | explicit call |
| `LIMITED_RISK` | `promote --target PRODUCTION` | `PRODUCTION` | explicit flag, external authorization, two-person control |
| deployed state | `rollback` | `ROLLED_BACK` | compatible approved prior version |
| any non-retired state | `disable` | `DISABLED` | safety action; artifact integrity not required |
| `DISABLED` or `ROLLED_BACK` | API `Retire` | `RETIRED` | explicit audited call |

Forbidden transitions fail before an event is appended. Unknown or malformed
states, signatures, hashes, schemas, pointers, or chain revisions fail closed.
There is no automatic full-production promotion and no full-production default.
Model `PRODUCTION` means approved model lifecycle only; it does not grant a
gateway permission, trading mode, strategy authorization, or live-transmission
predicate.

## API and CLI

The Go package exposes `Register`, `Validate`, `Approve`, `DeployShadow`,
`PromoteCanary`, `Promote`, `Rollback`, `Disable`, `Retire`, `InspectLineage`,
`Health`, manifest signing/verification, and exact feature compatibility.
The `model-registry` CLI exposes the requested operator subset plus `version`,
`health`, and explicit limited-risk/production `promote`.

All mutation commands require `--root`, `--key-id`, an externally supplied
`--private-key`, `--actor`, and `--reason`. Read-only inspection accepts a public
key. Coordinator-driven canary, production, and rollback additionally retain a
reviewed authorization reference. Output is structured JSON and never includes
key bytes. In a deployed system, lifecycle signing authority belongs only to the
coordinator/control service; the low-level CLI is an administrative primitive
and must not be available to inference or strategy identities.

Shadow comparison, explicit canary approval, signed scope limits, and automatic
rollback are defined by
[model deployment control](model-deployment-control.md). The registry remains
the durable lifecycle primitive and does not independently infer statistical
readiness.

Exact commands and test-key preparation are in
[model registry testing](../testing/model-registry-testing.md). Incident actions
are in the [rollback and disable runbook](../operations/model-registry-rollback-runbook.md).
The serialization, trust, and recovery decision is [ADR 0031](../adr/0031-signed-content-addressed-model-registry.md).

## Failure and recovery semantics

- A missing or invalid artifact, manifest, signature, feature digest, event,
  prior hash, or deployment pointer makes validation/deployment/inspection fail.
- A read-only registry is healthy for verification but not ready for mutation.
- Registration is idempotent only when the complete signed manifest is identical.
- Filesystem and in-process locks serialize mutation. A partial or malformed
  event tail is detected and is never silently repaired.
- Disable remains available when artifact bytes are corrupted, provided the
  authoritative lifecycle chain is valid.
- A failed pointer update after a durable event leaves an explicit event/pointer
  discrepancy; operators keep the model disabled and follow the runbook.
- Shutdown is process-bounded because the current implementation is a CLI and
  library, not a long-running service. A future network service must add the full
  health/readiness, metrics, structured logging, and graceful-drain contract.

## Compatibility and migration

Schema version 1 readers reject unknown JSON fields and versions. Feature
compatibility is deliberately exact. A v2 persisted contract requires a
reader-first migration, dual-read verification over existing objects and event
chains, an accepted ADR, and a tested rollback to v1. Existing files are never
rewritten in place.
