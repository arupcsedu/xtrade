# Aegis-MX Threat Model

## Scope and safety objective

This model covers source ingestion, colocated market processing, asynchronous
models, control-plane administration, persistence, CI, artifacts, and
deployment. Its primary security objective is to prevent unauthorized trading
authority, corrupted decision inputs, secret disclosure, audit erasure, and
unsafe availability degradation. When authenticity, integrity, freshness, or
authority is uncertain, new orders are blocked.

The model assumes the host kernel, hardware root of trust, production CA,
secret manager, CI identity provider, and two independent authorized operators
are separately administered. Compromise of all of those roots simultaneously
is outside this repository's containment capability.

## Protected assets

- gateway and broker credentials, certificate private keys, signing keys, and
  operator authentication material;
- active risk limits, kill-switch state, venue policy, model eligibility, and
  live-activation evidence;
- model artifacts, feature schemas, training manifests, calibration, and OOD
  profiles;
- raw packets, normalized events, feature snapshots, forecasts, decisions,
  orders, fills, positions, and administrative audit records;
- build inputs, dependency locks, SBOMs, image digests, provenance, and release
  signatures.

## Adversaries and abuse cases

| Threat | Primary controls | Detection and containment | Residual dependency |
| --- | --- | --- | --- |
| Compromised news source | Per-source authentication evidence, HTTPS host allowlist, content hash, deduplication, trust score, sandbox, prompt isolation | Source-quality degradation, contradiction records, fast alert retained, forecasts cannot send orders | Licensed-provider authentication and revocation specification |
| Malicious document | Byte/character limits, strict UTF-8, active-content removal, isolated process, CPU/memory/file/time limits, no tools | Rejection audit, sandbox termination, prompt-injection metric, exact evidence validation | Kernel seccomp/network namespace in the production intelligence deployment |
| Stolen credential | Short-lived mTLS identities, per-route RBAC, secret mounts, file permissions, rate limits | Identity-specific denials/rate alerts, certificate revocation and trust-bundle rotation | Production CA/secret-manager audit and revocation APIs |
| Unauthorized limit change | Auth context lifetime/epoch, role check, distinct author/approver, staged activation, signed immutable snapshot | Hash-chained administrative audit, compare-and-swap lineage, edge fails closed | External operator identity provider and two-person workflow integration |
| Model artifact replacement | Content-addressed artifact, Ed25519 manifest signature, exact feature schema, immutable approved object | Hash/signature verification on every transition and deployment; disable/rollback | Production HSM/KMS signing authority and object-lock storage |
| Configuration rollback attack | Parent hash, strictly increasing revision, active-hash CAS, signed edge cache, maximum offline age | Replay/revision rejection and audit-chain verification | Replicated monotonic authority epoch and disaster-recovery quorum |
| Packet corruption | Framing bounds, checksum, session/channel validation, sequence tracker, gaps never hidden, decoder fuzzing | Feed invalidation, recovery/snapshot state, data-quality publication | Licensed feed integrity and retransmission specifications |
| Data poisoning | Provenance, point-in-time vintages, correction history, OOD/calibration, source trust, replay manifests | Drift/OOD alerts, shadow/canary comparison, disable/rollback | Independent reference sources and governed training-data approvals |
| Denial of service | Bounded queues, input/path sizes, TLS request queue, concurrency semaphore, per-identity integer token buckets, timeouts | Explicit drops/rejections and queue/latency metrics; no authorization fallback | Network-layer DDoS controls and capacity planning |
| Insider misuse | Least privilege, two-person critical changes, immutable audit chain, deterministic replay, signed releases | Actor/request IDs, security alerts, separation-of-duty review, credential revocation | Organization IAM, surveillance, retention, and personnel controls |

## Attack paths and invariants

### Text-to-trade path

Provider bytes remain untrusted data through receipt, sandboxing, extraction,
and deep adjudication. They cannot add tools, alter a policy identifier, erase a
fast alert, mutate configuration, invoke risk, or call OMS/gateway methods.
Every resulting forecast remains advisory and expires through the common model
contract.

### Administrative path

Network identity authenticates the service connection; the control API then
checks principal role, authority epoch, request uniqueness, validity window,
two-person separation, signature, lineage, and activation time. None of those
checks alone enables live transmission.

### Supply-chain path

Dependencies are version/hash locked and scanned. CI actions and base images
are digest/commit pinned; the release image performs no mutable OS-package
resolution. Builds emit an SBOM and provenance, compare two artifact trees,
sign the immutable OCI digest, and verify the signer workflow identity before
deployment evidence is retained.

## Security test seeds and review

Deterministic adversarial tests retain seed `20260828`. Fuzzers cover audit
envelopes, synthetic packets, feed decoding, OMS state, gateway decoding, and
journal frames. Security policy and adversarial boundary tests run through
`make security-test`; dependency and secret discovery run through
`make dependency-scan`. This threat model must be reviewed when a new protocol,
provider, secret type, network listener, role, parser, build service, or live
activation dependency is introduced.
