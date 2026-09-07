# Security Incident Response

## Immediate priorities

Protect people, markets, credentials, evidence, and deterministic recovery in
that order. Suspected compromise never authorizes bypassing a halt, risk check,
clock/data check, signature, or two-person control. Prefer firm/account/venue/
strategy/symbol kill switches according to scope; engage the firm-wide switch
when scope is uncertain.

1. Record the UTC and monotonic detection times, alert identifier, affected
   service identity, configuration/model versions, image digest, host/process
   epochs, and current journal segment.
2. Engage the appropriate signed kill command and confirm edge acknowledgement.
   Do not use model/config rollout as a substitute for the kill path.
3. Isolate affected network identities and workloads. Preserve volatile state
   when doing so cannot permit further transmission.
4. Revoke the leaf certificate or signing credential, remove its URI identity
   binding, rotate the trust generation, and verify denied access from the old
   identity.
5. Copy journals, audit chains, security logs, SBOM/provenance, deployment
   manifests, and signed objects to immutable incident storage. Never repair an
   original in place.
6. Reconstruct decisions and administrative actions with deterministic replay.
   Record gaps and uncertainty explicitly.

## Scenario playbooks

### Compromised news source or malicious document

- Disable the provider and retain the exact content hash, authentication
  evidence, sandbox outcome, fast alert, corrections, and contradictions.
- Do not silently delete a previously published fast alert. Publish an explicit
  correction/dispute and disable affected specialist/model eligibility where
  warranted.
- Inspect sandbox deadline/resource rejections and prompt-injection metrics.
  Treat sandbox escape evidence as a host compromise and rotate all identities
  available to that workload.

### Stolen service or gateway credential

- Engage kill switches before revoking gateway credentials when order activity
  could still be in flight.
- Revoke certificate/credential at its authority, remove the identity from
  RBAC, rotate the trust bundle or gateway session epoch, and restart only the
  affected failure domain.
- Search structured logs by service identity and request/correlation ID. Do not
  paste the credential into tickets, chat, commands, or log queries.

### Unauthorized limit/configuration change or rollback attempt

- Compare the active hash, revision, parent hash, authority epoch, signer,
  proposal author, approver, activator, request IDs, and activation time.
- Preserve the signed snapshot and complete hash-chained audit. A corrupt or
  incomplete chain blocks activation and edge order admission.
- Roll back only through the signed compare-and-swap workflow with a distinct
  approver. Never edit the active pointer or audit journal manually.

### Model artifact replacement or data poisoning

- Disable the affected model version immediately; risk and OMS remain active.
- Verify manifest signature, artifact hash, feature schema, training-data
  manifest, dependency lock, code commit, calibration/OOD profile, and rollout
  observations.
- Recompute from the last trusted point-in-time dataset. Promote no replacement
  directly to production; repeat offline, replay, shadow, canary, and limited-
  risk controls.

### Packet corruption or denial of service

- Mark affected feeds/books invalid and enter recovery. Never infer a missing
  gap as healthy.
- Capture sequence, channel/session, checksum, A/B arbitration, queue occupancy,
  drop, request-rate, concurrency, NIC, clock, and journal-lag evidence.
- Rate-limit or network-isolate the offending identity/source. Availability
  pressure does not justify unbounded queues or weakened authentication.

## Certificate rotation procedure

1. Issue a short-lived replacement with the same approved URI SAN and intended
   EKU, or a new URI plus reviewed RBAC change. Verify validity overlap and time
   synchronization.
2. Write certificate, private key, and trust bundle as one secret-manager
   version; project it atomically into the read-only mount. Private material
   must not be world-readable.
3. Observe the TLS generation counter/startup record and establish a new
   mutually authenticated connection. Verify the exact peer URI and authorized
   endpoint.
4. Remove the old trust path or identity binding and confirm that a fresh
   connection with old material fails. Existing sessions must be drained within
   the documented connection lifetime.
5. Record issuer, serial/fingerprint, service URI, secret version, operators,
   approval, activation, verification, and revocation times without recording
   private material.

If loading the new generation fails, the service rejects new connections. Do
not enable plaintext. Restore a previously approved secret-manager version only
through an audited incident change.

## Recovery criteria

Trading readiness may be reconsidered only after credentials are rotated,
network identity and RBAC are verified, journals/audit chains validate, active
configuration and model hashes match approved versions, feeds/books/clocks/risk
are healthy, positions and live orders reconcile, replay explains affected
decisions, and two authorized operators approve recovery. `HALTED` or invalid
state transitions through recovery; it never jumps directly to `NORMAL`.
