# Event contracts

## Purpose

Aegis-MX uses one immutable contract boundary so that market data, features,
forecasts, decisions, risk, orders, fills, positions, intelligence, and safety
events can be reproduced without component-specific interpretation. This phase
implements data contracts only. It adds no exchange adapter and no trading
capability.

## Boundary model

Producers construct a typed domain table and wrap it in `ContractRecord`. A
persistence or network adapter then wraps those exact bytes in a size-prefixed
`AuditEnvelope`. Consumers perform checks in this order:

1. enforce the envelope byte bound and exact size prefix;
2. run the FlatBuffers structural verifier and check `AMAE`;
3. reject an unsupported schema major, zero identifiers, invalid timestamps,
   and unknown `RecordType`;
4. SHA-256 the exact payload bytes and compare in constant work;
5. run the nested verifier and check `AMCR`;
6. require envelope type, record type, and union discriminator to agree;
7. validate the selected record's enums, units, identifiers, ranges,
   relationships, and timestamp presence;
8. expose read-only pointers into the verified buffer.

Any failure returns a stable `ValidationError` and no view. Malformed, missing,
unknown, oversized, late, or inconsistent data therefore fails closed. Model
forecasts and ensembles are data only; neither contract can submit an order.

## Memory and latency boundary

Generated FlatBuffers accessors and the C++ `ValidatedAuditEnvelopeView` read
directly from verified bytes. Identifier, timestamp, version, and digest types
are fixed-size inline structs. Semantic validation and SHA-256 use bounded stack
state and do not allocate.

`FlatBufferBuilder`, Python encoding, file I/O, schema generation, and digest
manifest creation allocate memory and are not permitted in the innermost hot
path. A hot producer must write through a preallocated boundary that will be
introduced with the bounded event bus and journal. The current builders are
reference serializers and test fixtures for persistence/control boundaries.

Version 1 limits a nested contract to 1 MiB and a complete envelope to 4 MiB.
Hot-path component limits will be materially smaller and must be documented by
their owners.

## Reproducibility fields

The contracts provide the chain needed to reproduce a decision:

- source market and intelligence events use `GlobalEventId` and `SessionId`;
- feature metadata identifies the source event range, feature definition
  version, feature payload hash, and as-of times;
- v1.1 forecasts identify model and model version, feature snapshot,
  configuration, exchange as-of time, monotonic production/expiration,
  integer-PPM return distribution, directional probabilities, volatility,
  confidence, calibration/data-quality/OOD scores, and optional transaction costs;
- v1.2 forecasts preserve v1.1 and append distinct slippage and adverse-
  selection cost PPM values; readers deploy before v1.2 writers;
- v1.3 forecasts preserve every v1.2 field and append an explicit target, unit,
  and generic integer point/p10/p50/p90 distribution for return, realized
  volatility, volume, spread, market-factor, and sector-factor forecasts;
  readers deploy before non-return v1.3 writers;
- v1.4 intelligence records bind authenticated source hashes, document/event
  type, fast/deep stage and immutable lineage, registry entities, integer scores,
  evidence-backed facts, exact sanitized excerpts, corrections, and
  contradictions; readers deploy before v1.4 intelligence writers;
- v1.5 ensembles bind the combined fixed-point return distribution, variance,
  effective uncertainty, disagreement, all canonical expert weights and hard
  exclusions, transaction cost, uncertainty penalty, safety margin, robust
  edge, abstention and reason codes, dominant expert, gate/artifact identity,
  market/configuration/cost/source hashes, and ensemble version; all-invalid
  results carry an explicit empty contributor vector;
- v1.6 intents additionally bind venue, account, and a stable intent hash to the
  forecast, feature snapshot, strategy, configuration, and deadline;
- v1.6 risk decisions retain the exact account/strategy/venue/instrument and
  action, mode, first failed check, intent hash and SHA-256, risk/context hashes,
  configuration, state generations, authority epoch, policy revision, reason
  and completed-check masks, journal sequence, evaluation/deadline, approved
  integer quantity/price, and decision hash;
- v1.7 position snapshots retain account scope, open cost, portfolio and local
  journal sequences, pending quantities, exposure totals, drawdown,
  health/readiness, invariant reason, and the immutable portfolio hash;
- v1.8 order events retain account/strategy scope, exact OMS input/source/
  outcome, prior and current state, state version, exchange-session epoch,
  fencing token, normalized external identity, deterministic client order ID,
  exact risk-decision hash, and journal/snapshot sequence and hash evidence;
- order, fill, and position records retain order/intent identities and their
  relevant timing and unit domains;
- every transported record is bound to exact bytes by SHA-256.

Raw news/provider bytes do not enter `EventIntelligenceRecord`. Its bounded
evidence excerpts are exact slices of inert sanitized text. Document text has no
control authority and a record has no order-entry destination.

Configuration content and feature payloads live outside these metadata
contracts but are content-addressed by their immutable version or digest. Those
stores must retain exact bytes for replay.

## Safety semantics

`UNKNOWN = 0` is the default for all safety-relevant enums and is rejected.
Out-of-range enum values are also rejected after structural verification. Units
are explicit and execution prices never use floating point. Buy and sell
intents require positive quantity and an explicit integer limit price; cancels
require a target `OrderId` and cannot carry a price or quantity. All intents
still require a later `RiskDecision`; there is no model-to-order path.

`DataQualityState`, `ClockQualityState`, `TradingStatus`, and `KillSwitchEvent`
are first-class immutable inputs. Schema v1.6 represents symbol, strategy,
venue, account, and firm kill targets and requires a command sequence and
authority epoch. `KillSwitchEvent.RESET` and operator-originated events require
a nonzero authorization digest. A digest proves linkage to an authorization
artifact; signature verification belongs to the future control plane.

Externally supplied text never enters the contract. `EventIntelligenceRecord`
contains the untrusted source hash, sanitized content hash, trust state,
classification, relevance in integer ppm, and provenance/model identifiers.
Sanitization and prompt-injection defenses remain the responsibility of the
intelligence pipeline before it can publish this record.

## Timestamp domains

All four timestamp domains use nanoseconds but are distinct schema and C++
types. Exchange and NIC timestamps are wall-epoch values from different clock
sources; code must not order them against one another without considering
`ClockQualityState.offset_from_reference_ns` and `uncertainty_ns`. Process
monotonic values are valid only within their `SessionId`. Wall-clock UTC is for
audit and presentation, never local timeout measurement.

## Compatibility and artifacts

Generated code is checked in so consumers do not need a schema compiler at
runtime. `make schemas-check` rebuilds the exact pinned compiler, compares all
generated C++ and Python files, and verifies the binary golden fixtures.
Compatibility probes prove that an older table reader ignores an appended field
and a newer reader applies the safe default when that field is absent. Semantic
tests separately prove invalid enum rejection, truncation rejection, digest
failure, and discriminator agreement.

See the [schema directory](../../schemas/README.md), [evolution
policy](../../schemas/schema-evolution-policy.md), and [serialization
ADR](../adr/0004-canonical-flatbuffers-contracts.md).
