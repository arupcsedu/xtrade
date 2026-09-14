# Aegis-MX canonical schemas

This directory owns the immutable, cross-component wire contracts for Aegis-MX.
It does not contain venue protocols, order routing, credentials, or live-trading
capability.

## Bounded forecasting POC minute storage

[`canonical-minute-record-v1.schema.json`](canonical-minute-record-v1.schema.json)
defines the JSON-equivalent logical schema written to Parquet. Prices are
integer ticks with an explicit currency-nanosecond tick value; quantities are
integer shares. Exchange, source-availability, receipt, and processing times
remain distinct.

[`canonical-minute-quality-report-v1.schema.json`](canonical-minute-quality-report-v1.schema.json)
defines the report published before a Parquet partition can gain an immutable
partition manifest. Missing minutes remain missing, conflicting duplicates
reject ingestion, and neither schema is trading-capable.

Operational evidence schemas also live here. The
[`performance-benchmark-report-v1.schema.json`](performance-benchmark-report-v1.schema.json)
contract identifies hardware, pinning, methodology, simulated versus real-NIC
sources, stage/scenario distributions, resource counters, and the hash of the
separately retained raw sample stream.

The
[`paper-trading-acceptance-report-v1.schema.json`](paper-trading-acceptance-report-v1.schema.json)
contract fixes the 16 full-system PAPER scenarios, safety acceptance fields,
per-stage hashes, counters, and deterministic replay comparison.

The
[`paper-soak-report-v1.schema.json`](paper-soak-report-v1.schema.json) contract
defines the aggregate long-duration PAPER stability result, fixed check map,
coverage totals, resource/latency observations, limitations, and SHA-256 worker
evidence references. It is an offline evidence contract and carries no trading
authority.

The
[`alpaca-paper-certification-authorization-v1.schema.json`](alpaca-paper-certification-authorization-v1.schema.json)
and
[`alpaca-paper-certification-report-v1.schema.json`](alpaca-paper-certification-report-v1.schema.json)
contracts bind one off-hot-path, fixed-host broker PAPER connectivity check.
The additive
[`alpaca-paper-system-path-evidence-v1.schema.json`](alpaca-paper-system-path-evidence-v1.schema.json)
contract binds one fixed outbound order to deterministic router, risk, OMS, and
final paper-gateway evidence. These schemas explicitly disable live trading,
extended hours, short selling, and account reset; they do not certify broker
responses returning through the OMS.

## Canonical format

Version 1 uses FlatBuffers 25.12.19, pinned by release archive SHA-256 in the
root `CMakeLists.txt`. Persistent and network records have two layers:

1. `ContractRecord` is a non-size-prefixed FlatBuffer with file identifier
   `AMCR`. It holds one of the 19 domain record types.
2. `AuditEnvelope` is a size-prefixed FlatBuffer with file identifier `AMAE`.
   It carries the exact `AMCR` bytes, their SHA-256 digest, an optional previous
   envelope digest for chaining, and audit identity and timestamps.

Only complete `AMAE` buffers cross a persistence or network boundary. Raw
tables are not a supported transport. The digest is an integrity and content
identity check, not an authentication signature. Journal framing will add a
checksum over the complete envelope in the journal phase.

The IDL is split only to keep responsibilities readable:

- [`aegis_mx/v1/common.fbs`](aegis_mx/v1/common.fbs) defines identifiers,
  timestamp-domain structs, units, enums, versions, and SHA-256.
- [`aegis_mx/v1/records.fbs`](aegis_mx/v1/records.fbs) defines the 19 domain
  records.
- [`aegis_mx/v1/contract_record.fbs`](aegis_mx/v1/contract_record.fbs) defines
  the typed domain union and `AMCR` root.
- [`aegis_mx/v1/audit_envelope.fbs`](aegis_mx/v1/audit_envelope.fbs) defines
  the `AMAE` transport and persistence root.

## Contract inventory

| Record | Timing class | Intended owner |
| --- | --- | --- |
| InstrumentReferenceData | Near-real-time | reference-data/control plane |
| MarketEvent | Hot path | market-data normalization |
| BookUpdate | Hot path | market-data normalization |
| TradeEvent | Hot path | market-data normalization |
| QuoteEvent | Hot path | market-data normalization |
| AuctionImbalance | Hot path | market-data normalization |
| TradingStatus | Hot path | market-data normalization |
| FeatureSnapshotMetadata | Hot path | feature engine |
| ModelForecast | Near-real-time boundary | model adapters |
| EnsembleForecast | Hot path | ensemble |
| OrderIntent | Hot path | strategy/OMS boundary |
| RiskDecision | Hot path | deterministic risk |
| OrderEvent | Hot path | OMS |
| FillEvent | Hot path | OMS/gateway normalization |
| PositionSnapshot | Hot path | risk/positions |
| EventIntelligenceRecord | Asynchronous | intelligence |
| DataQualityState | Hot path | market-data quality |
| ClockQualityState | Hot path | clock supervision |
| KillSwitchEvent | Hot path override | risk/control boundary |
| AuditEnvelope | Boundary | journal/network adapters |

`EventIntelligenceRecord` excludes raw provider bytes. Schema v1.4 permits only
bounded sanitized excerpts that exactly support structured facts; raw content
remains identified by SHA-256 and is never a hot-path input.

## Units and timestamps

Execution-path prices are signed 64-bit integer tick counts. Quantities are
unsigned 64-bit instrument units except `PositionSnapshot.net_quantity_units`,
which is signed. Monetary values are signed integer currency nanos. Probability
and confidence values use integer parts per million in `[0, 1_000_000]`. No
schema contains `float` or `double`.

Schema v1.1 extends `ModelForecast` additively with return quantiles,
directional probabilities, volatility, calibration/data-quality/OOD scores,
exchange as-of time, and explicit optional transaction costs. Every forecast
numeric is integer PPM. Deployment is reader-first because `RETURN_PPM` is a
new enum value; see [ADR 0012](../docs/adr/0012-versioned-fixed-point-model-forecasts.md).

Schema v1.2 additively appends separate slippage and adverse-selection cost PPM
fields. v1.1 readers continue to ignore appended fields; deployments must be
reader-first so these components are not lost by operational consumers. See
[ADR 0013](../docs/adr/0013-native-microstructure-model-artifacts.md).

Schema v1.3 additively appends an explicit forecast target and generic integer
point/p10/p50/p90 values, plus volatility, volume, spread, and factor units.
Return publishers populate the legacy and generic values consistently;
non-return publishers leave legacy return fields neutral. v1.2 readers can
still read return forecasts, but v1.3 readers must deploy before non-return
writers. See [ADR 0014](../docs/adr/0014-off-hot-path-timeseries-forecast-serving.md).

Schema v1.4 additively extends `EventIntelligenceRecord` with document/event/
stage/adjudication enums, revision and fast/deep lineage, source authentication,
fixed-point novelty/materiality/trust/uncertainty, registry-backed entities,
evidence-linked facts, exact sanitized excerpts, and contradiction identities.
Older readers ignore the new fields, so v1.4 readers and semantic validators
deploy before v1.4 writers. See
[ADR 0015](../docs/adr/0015-untrusted-intelligence-fast-deep-pipeline.md).

Schema v1.5 additively extends `EnsembleForecast` with its fixed-point return
distribution, variance, effective uncertainty, disagreement, canonical expert
weights and exclusions, transaction cost, penalties, robust edge, abstention,
reason codes, explanation identity, and stable hashes. An all-invalid
abstention uses an explicitly present empty contributor vector. v1.5 readers
and semantic validators deploy before writers. See
[ADR 0021](../docs/adr/0021-hard-masked-fixed-point-mixture-of-experts.md).

Schema v1.6 additively binds `OrderIntent` to venue, account, and intent hash;
extends `RiskDecision` with the exact subject, action, mode, first failed check,
approved integer values, evidence hashes, state generations, authority epoch,
policy revision, check/reason masks, journal sequence, and decision hash; and
extends `KillSwitchEvent` for symbol/account targets plus monotonic command and
fencing epochs. Readers and semantic validators deploy before v1.6 writers.
See [ADR 0022](../docs/adr/0022-journal-first-deterministic-pre-trade-risk.md).

Schema v1.7 additively extends `PositionSnapshot` with account identity,
portfolio and journal sequences, the snapshot hash, integer open cost and
pending quantities, gross/net/beta/liquidity/event exposure, drawdown, health,
invariant reason, and readiness. One row remains scoped to one
account/strategy/instrument; portfolio totals repeat across rows in the same
sequence. See
[ADR 0023](../docs/adr/0023-event-sourced-bounded-portfolio-risk.md).

Schema v1.8 additively extends `OrderEvent` with exact OMS input kind, event
source, outcome, prior/current lifecycle state, state version, account/strategy
scope, exchange-session epoch, fencing token, normalized external order ID,
deterministic client order ID, exact risk-decision hash, and journal/snapshot
sequence and hash evidence. Readers and semantic validators deploy before v1.8
writers. See
[ADR 0024](../docs/adr/0024-journal-first-fenced-deterministic-oms.md).

Schema v1.9 additively extends `ModelForecast` with a semantic horizon unit,
value, explicit halt policy, deterministic session endpoint, calendar version,
and target exchange-event timestamp. `horizon_ns` remains the actual elapsed
interval for compatible readers and cannot define a trading-session horizon.
See [ADR 0046](../docs/adr/0046-exchange-calendar-forecast-horizons.md).

Timestamp types are not interchangeable:

- `ExchangeEventTimeNs`: venue-originated nanoseconds since the Unix epoch;
- `NicReceiveTimeNs`: synchronized NIC/PTP nanoseconds since the Unix epoch;
- `ProcessMonotonicTimeNs`: nanoseconds from a session-local monotonic origin;
- `WallClockUtcTimeNs`: local UTC nanoseconds since the Unix epoch.

Persisted monotonic timestamps are always accompanied by `SessionId`. Code must
not compare timestamps from different domains without clock-quality and
uncertainty context.

## Operational report schemas

The [chaos result report v1](chaos-result-report-v1.schema.json) is a strict
JSON Schema for offline CI and operational evidence. It is not an execution
contract and never enters the hot path. Its version, catalog hash, deterministic
seed, per-fault expectations/observations, attempt-chain hashes, and final
content hash make a result self-describing. Breaking report changes require a
new schema file and side-by-side reader support; existing result files are
immutable.

The [edge deployment profile v1](edge-deployment-profile-v1.schema.json) and
[local edge health v1](edge-health-v1.schema.json) schemas govern offline
systemd rendering and bounded local readiness files. They permit only
simulation/paper modes; live transmission and automatic activation are fixed
to false. These JSON contracts do not enter the execution hot path and carry no
credentials, endpoints, or provider protocol data.
The [reviewed host facts v1](edge-host-facts-v1.schema.json) contract binds CPU
and NIC topology plus NUMA-local huge-page capacity for offline qualification.
The [rollback manifest v1](edge-rollback-manifest-v1.schema.json) binds every
archive member to SHA-256 and records the profile and reproducible source epoch.
The [regional release lock v1](regional-release-lock-v1.schema.json) binds a
production configuration digest to the exact six regional OCI image digests,
SBOMs, provenance records, signature bundles, trusted CI identity, and
verification result. It is an administrative deployment contract and conveys
no order-entry authority.

The
[PAPER operator simulation report v1](operator-simulation-report-v1.schema.json)
is a strict offline drill result. It fixes the mode to `PAPER`, fixes live
compilation and production activation to false, records all four required kill
scopes, and binds the extracted risk-decision audit stream by SHA-256. It has no
OMS, gateway, network, credential, or live-activation authority.

Each NDJSON line in that stream conforms to the
[operator drill audit record v1](operator-drill-audit-record-v1.schema.json),
which fixes the record vocabulary and types for independently verifying the
sequence and SHA-256 chain.

The
[live-mode-disabled evidence v1](live-mode-disabled-evidence-v1.schema.json)
binds the configured build, operator drill, checked-in edge profiles, systemd
guard, control-plane rejection, and still-open production blockers. A passing
record means the inspected state is non-live while `production_ready` remains
false and activation remains `PROHIBITED`.

The bounded forecasting POC uses three off-hot-path JSON contracts. The
[storage policy v2](data-storage-policy-v2.schema.json) fixes the verified
10 TiB scratch allocation while retaining the decimal 80/100/20/50 GB POC
limits. [Storage policy v1](data-storage-policy-v1.schema.json) remains for
immutable legacy evidence. The [data manifest v1](data-manifest-v1.schema.json) describes
content-addressed source and partition envelopes, explicit timestamp domains,
and correction/replacement lineage. The
[storage audit v1](data-storage-audit-v1.schema.json) binds each administrative
result to the root, policy, evidence, reason codes, and the invariant that no
network access occurred. Runtime decoders additionally verify canonical bytes,
semantic time ordering, object size and SHA-256, references, and manifest
identity. These contracts do not enter a trading path.

Provider-neutral ingestion reuses `data-manifest-v1` for accepted and
quarantined source provenance. Its closed, authenticated `DownloadCheckpoint`
JSON is an internal recovery record rather than a cross-component event schema;
its semantics and migration boundary are documented in
[ADR 0049](../docs/adr/0049-provider-neutral-bounded-ingestion.md). It contains
no credential or signed locator.

The [data source policy v1](data-source-policy-v1.schema.json) is a separate,
closed administrative authorization contract. It fixes deny-by-default remote
access, contains no secret-bearing fields, inventories the independently gated
provider datasets and rights, and requires an approval hash, expiry, declared
use class, and resolved rights before any source may request adapter or network
authority. The checked-in example remains entirely disabled.

The
[instrument reference snapshot v1](instrument-reference-snapshot-v1.schema.json)
is a closed offline bitemporal contract for immutable instrument revisions,
symbol mappings, exact corporate actions, calendar days, and known halt
intervals. It fixes `live_trading_capable` to false and carries explicit
historical and halt completeness. The
[instrument resolution report v1](instrument-resolution-report-v1.schema.json)
records complete ticker-universe coverage without representing current mappings
as historical truth. Provider-derived instances remain owner-only outside Git.
See [ADR 0054](../docs/adr/0054-bitemporal-reference-data-with-current-only-source-evidence.md).

The [SEC EDGAR approval v1](sec-edgar-approval-v1.schema.json) is an external,
self-hashed authorization contract for individually allowed content classes;
the repository does not contain an enabled instance. The
[SEC artifact manifest v1](sec-edgar-artifact-manifest-v1.schema.json) binds a
content-addressed official-source object to exact local receipt/processing
times, nullable source publication time, selected acceptance-time range, and
optional correction lineage. The
[SEC coverage report v1](sec-edgar-coverage-report-v1.schema.json) records every
requested ticker, including unresolved current SEC associations, without
filing text. The side-by-side
[v1.1 contract](sec-edgar-coverage-report-v1_1.schema.json) adds the closed
filing-date window and per-issuer and aggregate historical-shard counts. All
three are offline
research contracts and convey no order-entry
authority. See [ADR 0055](../docs/adr/0055-bounded-sec-edgar-ingestion.md).

Prompt 54 adds four closed, offline GDELT JSON contracts: the external
[approval](gdelt-approval-v1.schema.json), immutable
[artifact manifest](gdelt-artifact-manifest-v1.schema.json), metadata-only
[record](gdelt-metadata-record-v1.schema.json), and combined
[run report](gdelt-run-report-v1.schema.json). Publication time is explicitly
null in v1 while GKG observation time and local receipt/processing time remain
separate. Records and reports are advisory-only and carry no order-entry
authority. See [ADR 0056](../docs/adr/0056-bounded-gdelt-gkg-query-and-advisory-events.md).

Prompt 55 adds four closed, offline ALFRED JSON contracts: the external,
self-hashed [approval](alfred-approval-v1.schema.json), immutable
[artifact manifest](alfred-artifact-manifest-v1.schema.json), complete
[series snapshot](alfred-series-snapshot-v1.schema.json), and bounded
[run report](alfred-run-report-v1.schema.json). The snapshot separates the
economic observation date, ALFRED vintage date, conservative knowledge
boundary, date-only release fact, actual local receipt time, and processing
time. Values are exact integer microunits or explicitly missing; no schema
field can carry an API key or order-entry authority. See
[ADR 0057](../docs/adr/0057-bounded-alfred-vintage-and-date-only-availability.md).

Prompt 56 adds the closed
[canonical minute record](canonical-minute-record-v1.schema.json) and
[partition quality report](canonical-minute-quality-report-v1.schema.json).
Prices are integer ticks with an explicit currency-nanos-per-tick grid; volume
is integer shares. Exchange event, source availability, local receipt, and
local processing timestamps remain distinct. A partition is not accepted
until its quality report is published and its immutable data manifest is the
final acceptance marker. See
[ADR 0058](../docs/adr/0058-sqlite-spool-and-parquet-minute-partitions.md).

Prompt 57 adds the closed
[feature/label row](feature-label-row-v1.schema.json), minute-derived
[session summary](derived-session-summary-v1.schema.json), complete
[ticker/horizon coverage](feature-label-coverage-v1.schema.json), and immutable
[dataset manifest](feature-dataset-manifest-v1.schema.json). Each feature row
has an exchange-event cutoff and a separate point-in-time knowledge cutoff;
every valid label has an explicit future exchange timestamp, return in PPM,
direction, future integer price ticks, and corporate-action version. The
manifest fixes seed `20260831`, the chronological split and two 42-session
purges, TRAIN-only normalization statistics, input/output hashes, and the
100 GB storage ceiling. These are offline research contracts and grant no
trading authority. See
[ADR 0059](../docs/adr/0059-multi-pass-leakage-safe-feature-datasets.md).

The Prompt 57-to-58 boundary is the closed
[training-readiness report](training-readiness-report-v1.schema.json). It binds
the accepted dataset, verified object bytes, exact source/dependency hashes,
per-horizon label and model-ready split coverage, deterministic feature mask,
known degraded-input policy, approval validity and expiry evidence, and
unsupported-symbol abstentions. Blocked reports retain schema-valid nullable
authorization evidence; ready reports require complete valid authorization.
`READY_FOR_INFRASTRUCTURE_VALIDATION` does not authorize economic claims or
trading. See
[ADR 0061](../docs/adr/0061-training-admission-with-degraded-source-evidence.md).

## Identifiers

Each identifier, including `AccountId`, is a distinct 16-byte `(high, low)`
unsigned struct. The
canonical text representation is 32 lowercase hexadecimal characters, high
word first. All-zero is invalid except where an optional FlatBuffers struct is
absent. Identifiers are opaque: ownership, allocation, and domain-separated
derivation are defined by the component that owns the entity. Truncating a
SHA-256 digest to form an identifier is permitted only when the owning contract
documents its domain-separation prefix.

## Generated bindings and commands

Generated C++ headers are checked into `schemas/generated/cpp`; generated
Python bindings are checked into `python/intelligence/aegis`. Never edit them by
hand.

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make schemas-check
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make schemas-generate
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make test-fuzz
```

`schemas-check` builds the pinned `flatc`, regenerates into a temporary
directory, compares every generated byte, and checks all golden AMAE fixtures.
The deterministic fuzz seed is `20260828`. The fuzz binary is instrumented with
UBSan; ASan is intentionally exercised by the separate `make test-sanitizers`
gate so hosts with constrained virtual address space can still run libFuzzer.
Each run starts from only the checked-in golden seed; crash artifacts are
retained under `build/fuzz-artifacts/audit-envelope`.

See the [event contracts](../docs/architecture/event-contracts.md),
[serialization ADR](../docs/adr/0004-canonical-flatbuffers-contracts.md), and
[schema evolution policy](schema-evolution-policy.md).
