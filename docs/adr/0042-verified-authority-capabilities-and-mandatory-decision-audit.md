# ADR 0042: Verified authority capabilities and mandatory decision audit

- Status: Accepted
- Date: 2026-09-07
- Decision owners: edge execution, high availability, security, compliance
- Related findings: PRD-C001, PRD-C002, PRD-C003, PRD-B005

## Context

The hostile production-readiness review found three unsafe authority boundaries.
The dormant live adapter accepted an opaque frame without an unforgeable proof
that the exact bytes had passed current authorization. The HA coordinator
accepted a public fencing structure whose stable hash could be recomputed by its
caller. The PAPER composition could expose an order to the gateway before its
decision explanation had entered a durable journal path.

Stable hashes detect accidental changes; they do not authenticate authority.
Likewise, a best-effort telemetry queue is not an audit commit point. These
properties become critical even while live compilation remains disabled because
they define the shape a future licensed implementation must use.

## Decision

1. Shared cryptographic primitives use the reviewed OpenSSL `libcrypto`
   implementation. HMAC-SHA-256 comparison is constant-time, and Ed25519 is used
   for independent witness grants. Private production keys remain outside the
   repository and process configuration.
2. A fencing grant is authoritative only after `FencingGrantVerifier` validates
   its Ed25519 signature, signer identity, trust-root identity, scope, lease, and
   stable hash. The coordinator accepts only `VerifiedFencingGrant`; public grant
   fields cannot directly create leadership.
3. A live transmission capability binds the exact protocol-frame SHA-256,
   command, risk decision, safety snapshot, signed configuration, operator
   authorization, durable activation record, fencing grant, session epoch,
   lease, and key identity. It is bounded and single use. A live adapter accepts
   only `VerifiedLiveTransmissionCapability`, which only the verifier can create.
4. The complete `DecisionExplanationRecord` has a canonical little-endian binary
   encoding that excludes ABI padding. The order path must publish it as a
   mandatory record to the bounded, fail-closed `AsyncJournal` before calling a
   gateway. Publication performs no disk I/O; the journal consumer owns storage.
   Queue saturation permanently inhibits that journal instance.
5. The PAPER acceptance harness shuts down the journal, scans its checksum chain,
   and decodes every explanation before reporting audit completeness.

## Consequences

- Future live code cannot compile against a frame-only transmission interface.
- Key custody, witness consensus, physical session fencing, licensed socket
  behavior, and target-host certification remain external production blockers.
- Mandatory queue acceptance is the nonblocking hot-path commit point. It proves
  ownership by the durable journal pipeline, not that an individual disk sync
  completed before network transmission. A site must therefore qualify the
  configured journal durability and replicated recovery policy.
- Canonical explanation bytes can be recovered without compiler-layout
  assumptions. Schema-major changes require a new decoder and compatibility
  fixture.
- OpenSSL is now a build and SBOM dependency.

## Rejected alternatives

- FNV or SHA-only grant fields: integrity without signer authentication.
- Caller-supplied authorization booleans: no provenance or replay protection.
- Synchronous `fsync` before every order: violates the hot-path blocking
  contract.
- Best-effort telemetry as audit evidence: records may be dropped under overload.
- Raw C++ object bytes: padding and ABI layout are not stable persistence
  contracts.

## Verification

Regression coverage includes RFC HMAC/Ed25519 vectors, forged and replayed
fencing grants, exact-frame capability binding and replay rejection, canonical
explanation round trips, mandatory-journal saturation, static pre-send ordering,
and all deterministic PAPER scenarios.
