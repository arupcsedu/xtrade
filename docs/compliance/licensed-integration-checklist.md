# Licensed-integration checklist

Complete separately for every feed, reference-data source, news provider,
exchange, broker, drop-copy service, options source, index source, and research
dataset. The detailed technical boundary is
[Licensed Integration Boundaries](../architecture/licensed-integration-boundaries.md).

## Authority and provenance

- [ ] Identify provider, product, version, markets, environments, and accountable
  business/engineering/legal owners.
- [ ] Record executed agreement, license scope, entitlements, permitted use,
  retention, derived-data, redistribution, display, audit, and deletion terms.
- [ ] Receive specifications through approved access controls; record document
  identity/version/hash and authorized recipients without committing proprietary
  content to this repository.
- [ ] Record effective/expiry dates, change-notice channel, support/escalation
  contacts, service levels, maintenance windows, and termination procedure.
- [ ] Complete privacy, security, sanctions, data-residency, and third-party risk
  review as applicable.

## Technical conformance

- [ ] Map every message, field, unit, timestamp, sequence, session, correction,
  status, auction, halt, reject, recovery, and error semantic to a versioned
  canonical contract. Never guess an unspecified behavior.
- [ ] Document authentication, encryption, endpoint separation, certificate and
  credential rotation, least privilege, rate limits, and secret custody.
- [ ] Implement only behind the existing adapter boundary with strict input-size,
  enum, checksum, identity, freshness, and sequence validation.
- [ ] Add golden, negative, fuzz, gap/recovery, replay, capacity, conformance, and
  certification tests using provider-authorized fixtures.
- [ ] Prove timestamps and point-in-time availability, correction/vintage rules,
  and historical entitlement behavior without future-data leakage.
- [ ] Document disconnect, retransmission, reset, snapshot, cancel/replace,
  duplicate, ambiguous acknowledgement, drop-copy, bust, and reconciliation
  behavior where applicable.

## Operational approval

- [ ] Complete provider certification in the exact non-production environment and
  retain case IDs/results under license controls.
- [ ] Qualify capacity, burst, failover, outage, recovery, clock, journal, and
  target-host behavior with approved limits.
- [ ] Approve monitoring, contacts, incident handling, evidence retention,
  reconciliation, kill behavior, and venue-aware protective actions.
- [ ] Verify production credentials/endpoints cannot enter simulation, PAPER,
  CI, replay, logs, metrics, artifacts, or repository content.
- [ ] Obtain legal, compliance, security, risk, operations, and engineering
  sign-off for the exact artifact/configuration/environment.

Unchecked or expired items keep the integration `blocked`. Certification makes
an integration eligible for further review; it does not enable live trading.
