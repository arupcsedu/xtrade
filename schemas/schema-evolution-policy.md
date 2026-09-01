# Schema evolution policy

## Status

Normative for every Aegis-MX persistent and network event contract.

## Version model

Every domain record, `ContractRecord`, and `AuditEnvelope` carries
`SchemaVersion { major, minor, patch }`.

- Major changes may break wire or semantic compatibility. A reader supports
  only explicitly implemented major versions and fails closed otherwise.
- Minor changes are additive and preserve the meaning and numeric encoding of
  all existing fields and enum values. A v1 reader may ignore an additive field
  but must still reject a record whose known safety invariants are not met.
- Patch changes clarify documentation or implementation without changing wire
  layout or meaning.

The current version is `1.8.0`. The C++ semantic validator accepts major 1 and
uses FlatBuffers unknown-field behavior for additive minor data. New behavior
must not rely on an additive field until every safety-relevant consumer has
been upgraded and its readiness is auditable.

Version 1.1 appends fixed-point distribution and provenance fields to
`ModelForecast` and adds the reader-first `RETURN_PPM` enum. Its deployment and
rollback order are defined in [ADR 0012](../docs/adr/0012-versioned-fixed-point-model-forecasts.md).

Version 1.2 appends slippage and adverse-selection PPM fields to
`ModelForecast`. Reader-first deployment and rollback are defined in
[ADR 0013](../docs/adr/0013-native-microstructure-model-artifacts.md).

Version 1.3 appends target-specific integer distribution fields and new units
to `ModelForecast`. v1.2 return semantics do not change. v1.3 readers deploy
before non-return writers; rollback stops non-return writers first. See
[ADR 0014](../docs/adr/0014-off-hot-path-timeseries-forecast-serving.md).

Version 1.4 appends structured source, lineage, score, entity, fact, evidence,
and contradiction fields to `EventIntelligenceRecord`. Exact excerpts refer to
sanitized retained text, never raw active content. v1.4 readers deploy before
v1.4 writers; rollback stops v1.4 writers first and retains immutable fast/deep
history. See
[ADR 0015](../docs/adr/0015-untrusted-intelligence-fast-deep-pipeline.md).

Version 1.5 appends fixed-point mixture distribution, per-expert weights and
eligibility, cost/uncertainty/edge values, abstention and reasons, explanation
identity, and stable hashes to `EnsembleForecast`. v1.5 readers deploy before
writers; rollback stops writers first. See
[ADR 0021](../docs/adr/0021-hard-masked-fixed-point-mixture-of-experts.md).

Version 1.6 appends account/venue/hash identity to `OrderIntent`, complete
deterministic evaluation and evidence binding to `RiskDecision`, and hierarchical
targets plus command/fencing sequence to `KillSwitchEvent`. Existing enum values
are unchanged; detailed risk reasons and check codes are appended. v1.6 readers
deploy before writers, and rollback stops v1.6 writers before older readers.
See [ADR 0022](../docs/adr/0022-journal-first-deterministic-pre-trade-risk.md).

Version 1.7 appends portfolio/journal sequence and hash provenance, account,
open-cost, pending-quantity, portfolio exposure/drawdown, and explicit
health/readiness evidence to `PositionSnapshot`. v1.7 readers and semantic
validators deploy before writers; rollback stops v1.7 position writers before
older readers. See
[ADR 0023](../docs/adr/0023-event-sourced-bounded-portfolio-risk.md).

Version 1.8 appends exact OMS input, source, outcome, lifecycle state, authority,
external/client identity, risk-decision hash, and journal/snapshot evidence to
`OrderEvent`. Existing order-event fields retain their meaning. v1.8 readers
and semantic validators deploy before writers; rollback stops v1.8 order-event
writers before older readers. See
[ADR 0024](../docs/adr/0024-journal-first-fenced-deterministic-oms.md).

## Compatible changes

A minor release may:

- append a field to the end of a table with a fail-safe default;
- append a new table and union alternative after coordinated reader rollout;
- add a new reason or state enum value after all older safety consumers are
  known to reject or understand it;
- increase a documented bound only after memory and latency review.

Unknown enum numeric values always fail closed. This means enum expansion is
wire-additive but operationally gated: readers are deployed first, producers
second.

## Breaking changes

The following require a new major version, new namespace, new file identifier,
and a documented migration:

- removing, reordering, or retyping a field;
- changing integer units, scale, signedness, or timestamp domain;
- changing an existing default or semantic meaning;
- renumbering, deleting, or repurposing an enum value;
- changing identifier width or derivation semantics;
- changing canonicalization or hash framing;
- making previously valid bytes mean something different.

Fields and enum numbers are never reused. Deprecated fields remain in place
and are documented as ignored. Scalar defaults must be unsafe (`UNKNOWN`, zero,
or false) unless zero has a single documented safe meaning.

## Change workflow

Every nontrivial schema change requires:

1. an ADR describing semantics, ownership, failure behavior, and rollout;
2. field-level units, range, validity, absence, and ownership documentation;
3. regenerated C++ and Python bindings using the pinned compiler;
4. older-reader/newer-writer and newer-reader/older-writer tests;
5. invalid enum, missing field, bounds, truncation, hash, and discriminator tests;
6. regenerated golden files reviewed as binary digest and decoded values;
7. deserialization fuzzing with seed `20260828` and retained crashing inputs;
8. validation, serialization, and hash benchmark comparison;
9. deployment ordering and rollback instructions when behavior changes.

Golden changes are never accepted as a mechanical CI fix. The reviewer must
confirm that the byte change is expected and that any stored-data migration is
documented.

## Retention and replay

Readers needed for the full regulated retention window must remain available.
Replay records the exact envelope bytes and selects a reader by major version;
it never silently upgrades stored events. Migrations produce new envelopes,
retain links to source envelope digests, and never overwrite source bytes.
