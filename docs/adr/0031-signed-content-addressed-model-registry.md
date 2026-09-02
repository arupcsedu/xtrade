# ADR 0031: Signed content-addressed model registry

- Status: Accepted
- Date: 2026-08-31
- Owners: control-plane, models, risk, research

## Context

Model artifacts must move from training through validation, shadow, canary, and
limited-risk environments without losing their exact data, feature, code,
dependency, calibration, or OOD provenance. A mutable database row is not
sufficient evidence: an overwrite could make a prior decision impossible to
reproduce, and a deployment pointer alone cannot explain who approved a change.
The C++ forecast path must not perform a registry RPC, filesystem read, signature
verification, or allocation.

The repository does not yet contain production identity infrastructure, an
object-store credential, PostgreSQL deployment, or organization-specific RBAC.
The initial complete slice therefore needs a local authority that is safe,
testable, and replaceable without pretending those external controls exist.

## Decision

1. The registry is a standard-library-only Go control-plane package and CLI. It
   has no model inference, strategy, risk bypass, order, OMS, gateway, or live-
   trading capability. C++ runners continue to receive already resolved local
   metadata snapshots.
2. Artifact bytes are addressed by lowercase SHA-256. A canonical JSON manifest
   binds the model ID and semantic version, point-in-time training manifest,
   ordered feature schema, code commit, dependency lock, training configuration,
   integer metrics, calibration data, OOD profile, parent version, artifact hash,
   and creation time.
3. Manifests use detached Ed25519 signatures over the SHA-256 of Go's canonical
   struct JSON encoding. Approved manifest and artifact files are never rewritten.
   Every read before validation, approval, deployment, promotion, rollback, or
   lineage inspection rechecks the signature and stored artifact hash.
4. Feature compatibility is exact: schema identity, semantic version, ordered
   fields, integer type, unit, scale, required flag, and canonical digest must
   match. Additive or coercing compatibility requires a new reviewed policy;
   unknown compatibility fails closed.
5. Lifecycle changes are signed append-only JSON Lines records. Each record
   contains a per-version revision and the prior event SHA-256. The chain stores
   actor, bounded reason, UTC receipt time, approval actor, environment, and any
   external authorization reference. Revision order, not wall time, is
   authoritative.
6. Deployment pointers are signed atomic materialized views. Lifecycle events
   remain authoritative. A process-local mutex plus a host filesystem lock
   serializes writers; immutable creation uses exclusive create; append and
   pointer replacement are synced to disk and parent directories.
7. Registration starts at `DRAFT`. Validation advances exactly one step through
   `TRAINED`, `OFFLINE_VALIDATED`, and `REPLAY_VALIDATED`. Approval is explicit
   and audited. Shadow, canary, and limited-risk each require a separate call.
8. Full production can only follow limited risk. It requires an explicit
   production flag, a nonempty external authorization reference, and a promoter
   different from the recorded approver. No method automatically promotes to
   production. This is model deployment control, not live-trading authorization.
9. Disable is idempotent and safety-biased. It does not require intact artifact
   bytes, so an artifact integrity failure cannot prevent revocation. It appends
   a signed `DISABLED` event and replaces every pointer for that version with a
   signed disabled pointer.
10. Rollback first appends a signed `ROLLED_BACK` event, disables all outgoing
    version pointers, and then points the selected environment to a previously
    approved, deployed, hash-valid, exactly compatible version. Neither artifact
    is changed. A rolled-back or disabled version may subsequently be retired.
11. The CLI accepts key paths only from the operator environment. Keys and
    credentials are not generated, stored in the repository, logged, or emitted
    in JSON output. Tests use a deterministic non-production Ed25519 seed.

## Consequences

- A local registry can be audited and recovered without a database, and all
  approval/deployment evidence remains content-verifiable.
- Signature verification and filesystem work occur only in the asynchronous
  control plane. Hot-path model activation still consumes a versioned local
  snapshot and may fail closed if registry material is unavailable.
- JSON v1 is bounded and strict but is not a network protocol. A future service
  may put a Protobuf/gRPC API over the same semantics and move immutable objects
  to object storage and indexed metadata to PostgreSQL.
- The local filesystem lock assumes all writers share a host and filesystem.
  Multi-host leader election, organization RBAC, key rotation/certificates,
  threshold approval, remote replication, retention, and disaster recovery are
  prerequisites for production operations.
- Ed25519 manifest approval does not satisfy signed live runtime configuration,
  operator trading authorization, gateway enablement, or any live-safety gate.

## Rejected alternatives

- A mutable manifest row was rejected because it weakens lineage and replay.
- Hashes without signatures were rejected because integrity alone does not
  authenticate an approving identity.
- Automatic metric-threshold promotion was rejected because model quality
  evidence cannot authorize production deployment.
- Runtime schema coercion was rejected because reordered, rescaled, missing, or
  renamed inputs can silently change model meaning.
- Registry lookups during inference were rejected because they add RPC or disk
  dependency to a bounded decision path.
