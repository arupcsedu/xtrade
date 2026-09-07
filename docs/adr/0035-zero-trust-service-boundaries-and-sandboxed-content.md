# ADR 0035: Zero-trust service boundaries and sandboxed content

- Status: Accepted
- Date: 2026-09-02
- Owners: security, control-plane, intelligence, model-serving, operations

## Context

Aegis-MX already signs model manifests and configuration snapshots, maintains
hash-chained administrative audit records, bounds its binary and JSON parsers,
and treats provider text as untrusted. Those controls do not authenticate a
network caller, rotate transport credentials, contain a parser compromise, or
bound concurrent connections. Plain cluster networking and process identity are
not authorization. Environment variables are also inappropriate secret values
because they are routinely exposed through process inspection and diagnostics.

## Decision

1. Every Aegis-MX network service requires TLS 1.3 mutual authentication. The
   only plaintext exception is an explicit test/development flag restricted to
   a numeric loopback address or `localhost`. The colocated hot path continues
   to use ABI-validated shared memory and does not add TLS, RPC, or certificate
   work to its critical loop.
2. Service identities are URI subject-alternative names in the
   `spiffe://aegis-mx/<environment>/<service>` trust domain. Chain validation is
   necessary but not sufficient: each endpoint maps the authenticated identity
   to a bounded permission set and rejects unknown routes and identities.
3. Certificates, private keys, trust bundles, and signing keys are obtained by
   logical name from a read-only secret-manager/CSI mount. Secret values are not
   accepted in command-line arguments, ordinary configuration, images, logs,
   metrics, traces, or source control. Private files must not be world-readable.
4. The time-series service calculates a digest across its mounted TLS material
   and builds a fresh context before subsequent handshakes when the digest
   changes. A malformed rotation fails closed. Existing sessions are not
   silently reauthenticated under the new identity.
5. Request bodies, paths, connections, concurrent handlers, authenticated
   identities, and per-identity request rates are bounded. Admission rejects
   without retry amplification. TLS negotiation and HTTP I/O have hard
   deadlines within bounded worker slots, so an incomplete handshake cannot
   monopolize the accept loop. Network policies deny egress and limit ingress
   to labeled workloads in the production trust domain.
6. News sanitization executes by default in a fresh spawned process. The child
   receives no ambient environment, changes to an empty private directory,
   disables Python socket construction, and applies CPU, address-space, file,
   file-size, and wall-time limits. Its response is bounded, schema checked,
   and content-hash verified. Production deployment must additionally supply a
   kernel-enforced no-network sandbox using namespaces/seccomp or an equivalent
   workload runtime. Inline parsing is an explicit test and offline-replay
   adapter, never a production default.
7. Release images are built by digest, include BuildKit provenance and SBOM
   attestations, and are signed keylessly with the CI workload identity. The
   workflow immediately verifies the exact digest and certificate identity.
   Deployment policy must reject tags, unsigned digests, and an all-zero
   placeholder digest.
8. Package builds use `SOURCE_DATE_EPOCH`, path trimming, disabled VCS mutation,
   disabled Go build IDs, hash-locked dependencies, deterministic SBOM content,
   a digest-pinned Python runtime image with no mutable OS-package install, and
   a two-build byte comparison. Reproducibility failure blocks release.
9. Security policy tests validate action pins, container and pod privileges,
   secret mounts, service identities, network policy, SBOM coverage, mTLS/RBAC,
   rate bounds, prompt isolation, and sandbox behavior without production
   credentials.

## Consequences

- Certificate or trust-bundle loss makes a network service unready rather than
  opening a plaintext listener.
- A caller needs both a CA-valid certificate and an authorized service URI for
  the requested operation. Stolen material can be contained through trust
  bundle rotation, identity removal, rate limits, and network policy.
- Certificate parsing and rotation remain outside the C++ hot path.
- Kubernetes TCP probes prove listener availability only. Authenticated
  monitoring must call `/healthz` and `/readyz` over mTLS to determine semantic
  readiness.
- Python socket denial is defense in depth, not a kernel security boundary.
  Production news workers remain blocked until a reviewed seccomp/network-
  namespace policy and isolated workload manifest are supplied.
- Keyless container signing depends on CI OIDC and the transparency service at
  release time; an outage blocks a release but does not affect a running edge.

## Rejected alternatives

- Trusting Kubernetes source IPs was rejected because IP identity is mutable
  and does not provide cryptographic workload authentication.
- Server-only TLS was rejected because it does not authenticate callers.
- Long-lived bearer tokens in environment variables were rejected because
  they are replayable and easily exposed.
- Reusing provider document text as an LLM instruction was rejected because it
  crosses the data/control boundary.
- Retaining the old certificate after a failed rotation was rejected because
  it can extend revoked authority without an auditable decision.
- Signing mutable image tags was rejected because a tag is not immutable
  deployment evidence.
