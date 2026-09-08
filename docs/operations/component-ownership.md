# Component ownership and escalation

These are accountable role definitions. Before any site commissioning, the
operator-owned roster must bind every role to named primary/secondary responders,
a paging route, time zone, and escalation deadline. Those identities and contact
details do not belong in this repository.

| Boundary | Engineering owner | Operational owner | Authoritative state | Escalate when |
| --- | --- | --- | --- | --- |
| Edge core, clocks, event bus, journal | Core platform | Colocation SRE | Process/leader epoch, clock state, journal chain | Clock unsafe, queue/audit loss, epoch conflict, storage failure |
| Market data and feed recovery | Market-data engineering | Market-data operations | Channel/session sequence and `DataQualityState` | Gap, stale feed, A/B conflict, invalid snapshot |
| Order books and features | Market-core engineering | Trading operations | Book version/validity and feature snapshot | Crossed/invalid book, parity failure, stale snapshot |
| Model contracts and serving | Model platform | Model operations | Artifact/version, deadline, forecast validity | Late/malformed output, OOD/calibration regression |
| Intelligence specialists | Intelligence engineering | Event operations | Source provenance and structured event version | Malicious input, contradiction, missing official source |
| Ensemble and market state | Decision systems | Trading operations | State transition and ensemble explanation | Forbidden transition, unexplained abstention/weight |
| Pre-trade and portfolio risk | Risk engineering | Independent risk operations | Limit/risk/position snapshot and kill generation | Any unavailable/stale risk, exposure breach, reconciliation mismatch |
| OMS and router | Trading systems | Trading operations | Order state/version and routing decision | Unknown order, duplicate, state conflict, route loop |
| Gateway boundary | Trading systems | Venue operations | Mode, session epoch, fencing token, audit sequence | Disconnect, ambiguous response, reject burst, ownership uncertainty |
| Control plane and registry | Control-platform engineering | Change authority | Signed immutable configuration/model lineage | Signature, RBAC, audit, freshness, rollback failure |
| Observability | Reliability engineering | On-call SRE | Alert rule/version and evidence retention | Export loss, cardinality breach, missing decision evidence |
| Security and identity | Security engineering | Security incident commander | Service identity, trust generation, revocation state | Credential theft, artifact/config tamper, unauthorized access |
| Deployment and disaster recovery | Release engineering | Incident commander | Artifact, profile, host, backup and restore hashes | Site outage, rollback/restore uncertainty, unsigned artifact |
| Licensing and regulatory review | Legal/compliance | Designated compliance officer | Approval record and obligation matrix | Missing rights, changed rule/specification, reporting uncertainty |

## Decision authority

- Any operator may report an unsafe condition. The authorized kill role engages
  the narrowest certain kill; uncertainty requires firm scope.
- Risk operations owns limit interpretation and kill-reset approval. Engineering
  cannot waive a failed risk check.
- Market-data operations owns feed recovery evidence but cannot declare a book
  valid; the market core validates the rebuilt state.
- Trading operations owns PAPER session sequencing but cannot bypass fencing,
  risk, halt, data, clock, journal, or configuration gates.
- Security may isolate identities and systems. Isolation never grants trading
  authority.
- Legal/compliance determines applicable external obligations and license scope.
  Code or operator convenience cannot substitute for approval.
- The incident commander coordinates recovery; each authoritative-state owner
  must separately attest its own exit criteria.
