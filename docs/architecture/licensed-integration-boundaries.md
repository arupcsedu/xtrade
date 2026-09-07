# Aegis-MX Licensed Integration Boundaries

| Field | Value |
| --- | --- |
| Status | Normative integration constraint |
| Date | 2026-08-29 |
| Current licensed specifications in repository | None |

## Rule

Aegis-MX does not infer, reconstruct, or fabricate proprietary provider or
exchange protocol details. Generic interfaces, deterministic synthetic/reference
protocols, and license-clean test fixtures may be implemented without licensed
material. A concrete production integration remains incomplete until current,
authorized specifications and all associated legal, entitlement, certification,
security, and operational inputs are available.

Licensed specifications, credentials, raw licensed data, and proprietary
documents must not be committed to this repository unless the license explicitly
authorizes repository storage and the approved security policy provides it.
Ordinarily, the repository records only non-secret provenance: provider/venue,
document identifier/version/date, approved access location, content hash where
permitted, entitlement owner, implementation scope, and review evidence.

This document refines the [engineering contract](engineering-contract.md),
[component boundaries](component-boundaries.md), and
[live-trading safety policy](../operations/live-trading-safety.md). It does not
authorize any integration.

## Completion classes

| Class | Meaning | Permitted claim |
| --- | --- | --- |
| Interface-complete | Internal versioned adapter contract, error/state model, and synthetic harness exist | Generic boundary is implemented; no provider compatibility claim |
| Synthetic-complete | Published repository-owned reference protocol and deterministic simulator pass conformance/fault tests | Simulation behavior is implemented; no paper or real endpoint access |
| Specification-implemented | Adapter is implemented against an identified authorized specification with traceable conformance tests | Named specification fields/behaviors are implemented; not yet provider-certified |
| Paper/certification-complete | Provider/venue test environment, credentials, certification, recovery, and operational tests pass | Named non-money integration is supported within recorded scope |
| Live-eligible | All technical, venue, legal, compliance, operational, and Aegis-MX live gates approve the exact artifact/configuration | Eligibility only; live activation remains default-off, scoped, explicit, expiring, and continuously gated |

No lower class may use the name, logo, endpoint, message identifiers, or behavior
of a provider in a way that implies conformance without authorization.

## Market-data and reference-data boundary

### Implementable without licensed specifications

- Provider-neutral adapter and normalized source-event interfaces.
- Repository-owned synthetic feed framing, sequence, snapshot, gap, correction,
  status, and fault-injection behavior.
- Generic checks for bounds, integrity, duplicates, sequence continuity,
  freshness, timestamps, unknown instruments, and malformed input.
- Deterministic book algorithms for an explicitly synthetic market model.
- Instrument/reference-data abstraction and synthetic calendars, symbology, tick
  tables, corporate actions, and session states.

### Cannot be completed without authorized specifications

- Wire framing, encodings, field identifiers/types/scales, compression,
  encryption, checksums, and version negotiation.
- Multicast/unicast channel topology, session/authentication, sequence domains,
  snapshots, replay/gap recovery, retransmission, duplicate rules, and failover.
- Provider timestamp meaning, precision, clock source, ordering guarantees,
  correction/bust semantics, and receive-time requirements.
- Venue-specific order-book actions, priority/aggregation rules, implied orders,
  auctions, halts, crossed/locked states, trade corrections, and recovery
  validity.
- Instrument identifiers, symbology changes, calendars, tick/lot tables,
  corporate actions, status codes, and reference-data effective times.
- Entitlement, display/non-display, derived-data, retention, redistribution,
  audit, and data-destruction obligations.
- Production capacity/rate constraints and certification expectations.

Affected components are `market-data`, `order-book`, `feature-engine` where a
feature depends on provider semantics, `risk` where price/status/reference state
is venue-specific, `replay`, `research`, and `deployment`.

## Exchange order-entry and post-trade boundary

### Implementable without licensed specifications

- Internal OMS command/event contract and deterministic lifecycle state machine.
- Provider-neutral deterministic internal/client/external ID mapping,
  journal-first command return, leader fencing, explicit unknown recovery, and
  primary/drop-copy logical-fill reconciliation using synthetic normalized IDs.
- Gateway adapter interface, final safety predicate, mode state machine, and
  repository-owned synthetic venue.
- Generic idempotency, integer price/quantity validation, risk-approval binding,
  bounded queues, kill/inhibit hooks, and fault-injection harness.
- A proof that synthetic/replay binaries cannot resolve or reach a real venue.

### Cannot be completed without authorized specifications

- Order-entry/session wire format, transport, logon/authentication, sequence,
  heartbeat, reconnect, resend, reset, encryption, and session ownership.
- Order types, time-in-force, price/quantity scales, tick/lot rules, identifiers,
  flags, capacity/agency fields, and venue validation/error codes.
- New/cancel/replace/mass-cancel behavior, acknowledgements, fills, busts,
  corrections, unsolicited cancels, cancel-on-disconnect, and duplicate handling.
- Rate limits, throttling, backoff, reject recovery, kill interfaces, drop copy,
  session fencing, and exchange-side self-trade prevention.
- Ambiguous acknowledgement and disconnect reconciliation; open-order, position,
  and account recovery; safe cancel/flatten behavior.
- Trading sessions, auctions, halts, price collars, restricted states, venue
  surveillance/reporting fields, and certification test cases.
- Network connectivity, certificates/keys, IP allowlists, account/session
  allocation, test/live endpoint separation, operational contacts, and incident
  procedures.

Affected components are `OMS`, `gateways`, `risk`, `control-plane`, `replay`,
`observability`, and `deployment`. A generic OMS may be complete for the
synthetic contract, but venue-specific recovery and lifecycle support cannot.

## News, filing, and intelligence-provider boundary

### Implementable without licensed specifications

- Provider-neutral untrusted-document envelope with source provenance,
  content hash, receipt time, size/encoding limits, and schema version.
- Synthetic news/filing provider, malicious-content corpus, prompt-injection
  isolation, schema-constrained extraction, and explicit rejection behavior.
- Model-contract publication, deadline handling, and lack of order/control tools.
- Provider-neutral macro calendar, frozen-consensus, release/completeness,
  prior/revision, source-quality, cross-asset response, correction, abstention,
  and deterministic replay contracts with fictional fixtures.

### Cannot be completed without authorized specifications and terms

- API/feed/webhook transport, authentication, pagination, throttles, retry,
  replay, message identifiers, schemas, encodings, and delivery guarantees.
- Provider timestamps, embargoes, priority/urgency, language, topic/entity and
  instrument identifiers, correction/retraction/update chains, attachments, and
  canonical source links.
- Entitlement, user/display/non-display, automated-analysis, model-training,
  storage/retention, derived-output, redistribution, attribution, audit, and
  deletion requirements.
- Provider-specific service levels, outage/recovery behavior, support/escalation,
  and permitted test fixtures.
- Production macro calendar/release field mappings, units, seasonal and
  annualization conventions, statement/auction semantics, publication versus
  availability rules, consensus construction/dispersion, revision/correction
  identifiers, and provider-conflict policy.
- Licensed equity-index, Treasury, FX, volatility, and sector response data,
  including symbology, timestamp semantics, derived-data rights, and retention.

Affected components are `intelligence`, `model-contracts` where provider
provenance fields are required, `research`, `replay`, `observability`, and
`deployment`. Provider content remains untrusted even after authentication and
entitlement checks.

## Options market and reference-data boundary

### Implementable without licensed specifications

- Public OSI symbol parsing without provider transport assumptions.
- Provider-neutral normalized contracts for option reference revisions, quotes,
  trades, open interest, underlying state, carry, expiration, and corporate-
  action revision identity.
- License-clean synthetic European surfaces, bounded BSM inversion, numerical
  Greeks, static-arbitrage checks, and deterministic replay.
- Explicitly assumption-labelled gamma/vanna/charm, pinning, and hedging-
  pressure estimates that never represent dealer inventory as observed.

### Cannot be completed without authorized specifications and terms

- Options feed/API framing, authentication, channel/session behavior, field
  identifiers/scales, quote/trade condition semantics, corrections, cancels,
  sequence/recovery rules, exchange timestamps, and symbol mapping.
- Authoritative contract master, exercise/settlement style, expiration and last-
  trading calendar, multiplier, deliverable, OCC adjustment memo mapping,
  corporate-action effective time, rates/dividends, and open-interest publication
  semantics.
- Entitlement, non-display, derived-data, analytics, retention, redistribution,
  audit, and model-training rights for quotes, trades, open interest, and
  reference data.
- Production surface conventions, American/discrete-dividend numerical policy,
  validation tolerances, outage behavior, capacity, and provider certification.

Affected components are `intelligence`, `model-contracts`, `research`, `replay`,
`observability`, and `deployment`. The repository implementation is synthetic
and provider-neutral; it does not imply conformance with an options venue,
clearing organization, or data vendor.

## Index, auction, and liquidity-inference boundary

### Implementable without licensed specifications

- Provider-neutral normalized index announcement, weight, float, tracker-asset,
  ETF, auction, displayed-depth, execution, replenishment, and historical-
  behavior contracts with point-in-time provenance.
- License-clean synthetic closing-auction, rebalance-day, and replenishment
  fixtures with deterministic fixed-point estimates and replay hashes.
- Explicitly assumption-labelled passive flow, auction allocation, iceberg
  probability, latent-size range, and uncertainty outputs.

### Cannot be completed without authorized specifications and terms

- Production index-provider announcement identifiers, correction chains,
  eligibility rules, float and weight conventions, effective timing, tracker-
  asset methodology, and redistribution or derived-data rights.
- Venue auction message fields, side/imbalance semantics, reference and
  indicative-price rules, cutoff phases, allocation priority, order eligibility,
  corrections, halts, extensions, and closing-print finality.
- Venue-specific displayed/hidden/reserve order behavior, refresh priority,
  trade conditions, partial-fill semantics, aggregation, attribution limits,
  and rights to infer, retain, or redistribute derived liquidity analytics.
- Calibrated production impact, reversal, fill, and venue-behavior histories
  with point-in-time licensing, symbology, timestamp, correction, retention, and
  model-training rights.

Affected components are `intelligence`, `market-data`, `order-book`,
`model-contracts`, `research`, `replay`, `observability`, and `deployment`.
Current estimates are synthetic/provider-neutral infrastructure validation and
do not claim compatibility, observed hidden liquidity, or predictive value.

## Point-in-time research-data boundary

### Implementable without licensed specifications

- Immutable provider-neutral event/publication/receipt/processing/revision time,
  business-validity interval, source identity, content hash, and version.
- Append-only corporate-action, symbology, delisting, index-membership,
  estimate, macro, news-correction, and filing-amendment fixture contracts.
- Deterministic as-known/effective-time queries and leakage rejection for
  future data, survivorship, label overlap, and randomized time-series splits.

### Cannot be completed without authorized specifications and terms

- Production source timestamp meaning, revision/correction identity, latency,
  embargo, deletion, and late/canceled publication semantics.
- Authoritative historical symbology, constituents, delistings, corporate
  actions, estimate contributors, macro vintages, and document amendment chains.
- Dataset/model-training rights, derived-data restrictions, retention,
  redistribution, audit, and required deletion or correction propagation.

The reference implementation proves internal semantics only. It cannot claim
point-in-time completeness for a named provider or dataset without licensed
source conformance and historical completeness evidence.

## Other external production inputs

Some necessary inputs may not be “licensed protocol specifications” but are
still external blockers and must not be invented:

| Input | Needed by | Required authority |
| --- | --- | --- |
| Concrete risk limits and restricted-instrument policy | `risk`, `OMS`, `gateways`, `control-plane` | Independent risk and compliance owners |
| Manipulation surveillance thresholds and escalation | `risk`, `observability`, operations | Compliance/legal and venue rules |
| Clock source/offset/uncertainty limits | `edge-core`, `market-data`, `gateways` | Platform engineering, venue requirements, risk approval |
| Account ownership, broker clearing, drop copy, and reconciliation | `OMS`, `gateways`, `risk` | Broker/venue operations and finance |
| Drop-copy execution keys, correction/bust lifecycle, fee semantics, and session watermarks | `OMS`, `risk`, `replay` | Licensed broker/venue specifications and clearing operations |
| Corporate-action entitlements, fractional rounding, tax lots, and broker cost-basis policy | `risk`, `OMS`, `research` | Authorized reference data, broker accounting, tax/legal review |
| Retention, privacy, evidence, and data-destruction policy | Journal, `replay`, `research`, `observability` | Legal/compliance/data governance |
| Signing roots, operator roles, authorization duration, and two-person policy | `control-plane`, `gateways`, `deployment` | Security, risk, compliance, operations |
| Hardware, colocation, network, and capacity objectives | Hot-path components, `deployment` | Platform/venue/network owners and measured evidence |
| Site NIC identity, VLAN/multicast/route data, IRQ/RSS rules, and PTP domain/source | `market-data`, `gateways`, `deployment` | Licensed provider/venue materials and site network/platform approval |

Generic safe mechanisms may be built before these values exist, but production
configuration and completion claims remain blocked.

## Specification intake and provenance

Before work begins on a concrete integration:

1. Name an accountable integration owner and compliance/security reviewers.
2. Record legal authorization, entitlement scope, permitted environments, data
   use, storage, derived use, testing, redistribution, and retention terms.
3. Store the specification in an approved access-controlled system outside the
   repository unless explicit policy authorizes otherwise.
4. Record a stable document/API identifier, revision/effective date, content hash
   where permitted, source, access location, and supersession status.
5. Create a requirements-to-code-and-test conformance matrix without copying
   proprietary prose into repository artifacts.
6. Obtain provider-issued credentials through an external secret manager with
   environment/account scope, rotation, revocation, and access audit. Never use
   production credentials in development, fixtures, logs, traces, or CI.
7. Create license-clean synthetic or approved sanitized fixtures. Raw licensed
   payloads enter CI only if storage/use is explicitly permitted and access is
   appropriately restricted.
8. Define unknown-version and spec-drift behavior as fail closed. Detect and
   alert on provider version change rather than guessing compatibility.
9. Complete security review, conformance testing, provider/venue certification,
   fault recovery, capacity testing, and operational runbooks in the authorized
   non-money environment.
10. Record approval for the exact adapter version, build artifact, configuration,
    provider/spec revision, environment, and supported/unsupported feature set.

## Change and expiry handling

A provider notice, schema/version change, entitlement change, credential
rotation, endpoint change, certification expiry, or undocumented behavior puts
the affected adapter into not-ready state until revalidated. Compatibility is
not assumed from a successful decode or session connection.

Adapters publish their supported specification set and current observed
provider version. Unknown or out-of-scope messages are rejected or quarantine
the feed/session according to the approved protocol policy. Rollback must account
for provider compatibility and journal replay; it is not merely a binary
deployment reversal.

## Repository and telemetry exclusions

The following must never be committed or emitted without explicit authorization:

- credentials, private keys, session tokens, certificates with private material,
  account identifiers, production endpoints, or allowlist details;
- proprietary protocol documents or copied specification sections;
- raw licensed market/news payloads, production journal records, or derived data
  whose license prohibits storage or redistribution; and
- secrets embedded in model artifacts, prompts, logs, metrics, traces, crash
  dumps, test failures, benchmark output, or generated code.

CI must scan the current tree and history for credentials and prohibited files.
Logging/redaction tests use synthetic sentinel secrets. Metrics use bounded
provider-neutral labels unless an approved operational requirement permits
otherwise.

## Current blockers

The generic `LicensedKernelReceiver`, `LicensedBinaryDecoder`, and
`LicensedRecoveryProvider` interfaces now exist as interface-complete skeletons.
They contain no provider fields or behavior and do not change the blockers below.

The repository contains no licensed exchange, market-data, reference-data, news,
filing, broker, or provider specification and no evidence of authorization to
use one. Therefore the following are explicitly blocked:

- every real market-data/reference-data decoder and production book-semantic
  claim;
- every real order-entry, drop-copy, session, recovery, paper-venue, and live-
  venue adapter;
- venue-specific OMS reconciliation, cancel/flatten, and error handling;
- every production news/filing transport remains blocked. The implemented
  official-public boundary accepts only responses from an injected reviewed
  HTTPS client; provider-specific provenance, identity headers, correction,
  entitlement, retention, or redistribution behavior remains blocked; and
- every production macro calendar, consensus, official-release, statement,
  auction, and cross-asset response adapter remains blocked. The implemented
  macro subsystem is normalized-contract and synthetic-replay complete only;
  provider field semantics and data rights are not inferred; and
- every production options quote, trade, open-interest, contract-reference,
  corporate-action, rate, dividend, and expiration-calendar adapter remains
  blocked. The implemented options subsystem accepts only normalized contracts
  and synthetic surfaces; provider wire and semantic behavior is not inferred;
  and
- provider/venue certification, production connectivity, and live eligibility.

The allowed next work is repository tooling, versioned internal contracts,
synthetic/reference protocols, deterministic cores, simulation, replay, and
license-clean adversarial test harnesses in the order defined by the
[implementation roadmap](implementation-roadmap.md).
