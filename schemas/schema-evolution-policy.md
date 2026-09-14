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

The current version is `1.9.0`. The C++ semantic validator accepts major 1 and
uses FlatBuffers unknown-field behavior for additive minor data. New behavior
must not rely on an additive field until every safety-relevant consumer has
been upgraded and its readiness is auditable.

The bounded forecasting data repository uses separate canonical JSON contracts.
`data-storage-policy-v3` expands only the target and hard root ceiling to 800
GB. `data-storage-policy-v2` records the authoritative scratch-allocation
correction and immutable `data-storage-policy-v1` remains available for legacy
evidence. `data-storage-policy-migration-v1` records the exact v2-to-v3 marker
transition.
`data-manifest-v1`, `data-storage-audit-v1`, and `data-source-policy-v1` retain
semantic version `1.0.0`. Their object fields are
closed and exact. An additive field therefore requires a new side-by-side schema
and reader-first rollout; removal, renaming, unit changes, canonicalization
changes, or hash-framing changes require a new major schema filename. Existing
manifests, policies, and audit records remain immutable and are never rewritten
during migration.

Storage-policy v1 roots are migrated by verified copy into a newly initialized
v2 root. Readers must validate the original with its v1 release; neither the v1
marker nor its manifests are rewritten. An empty v1 root may be archived and a
v2 root initialized at the operational path, as recorded in ADR 0050. A v2
root may transition to v3 only after archiving the exact marker, publishing an
immutable migration record, rechecking the source identity under the admission
fence, and atomically installing the v3 marker as recorded in ADR 0062.

Provider-neutral ingestion checkpoints use a closed internal JSON contract at
`1.0.0`. A reader rejects any other version or field set. Additive fields need a
new reader-first minor contract; identity, canonicalization, range-prefix,
source-version, or hash-framing changes require a new major checkpoint version.
Existing checkpoint bytes are retained as recovery evidence and never silently
rewritten into a new meaning. Accepted object provenance continues to use
`data-manifest-v1`.

Instrument reference snapshot v1 and instrument resolution report v1 are
side-by-side closed JSON contracts. Unknown fields, enum values, schemas,
noncanonical bytes, and digest mismatches reject. Additive fields require new
reader support before publication; changed identity derivation, temporal
semantics, rational units, or hash framing require a new major filename.
Existing reference artifacts remain immutable. See
[ADR 0054](../docs/adr/0054-bitemporal-reference-data-with-current-only-source-evidence.md).

SEC coverage report v1.1 is a side-by-side closed JSON contract. It preserves
v1.0 files unchanged and adds an explicit filing-date window plus per-issuer
and aggregate historical-shard counts. Writers publish v1.1 only after readers
support it; v1.0 evidence remains verified against the original schema and is
never rewritten. See [ADR 0055](../docs/adr/0055-bounded-sec-edgar-ingestion.md).

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

Version 1.9 appends a semantic `HorizonSpec` and explicit target exchange-event
timestamp to `ModelForecast`. Existing `horizon_ns` remains actual elapsed
nanoseconds and existing v1.8 bytes remain valid. V1.9 readers deploy before
calendar-aware writers; rollback stops those writers first. See
[ADR 0046](../docs/adr/0046-exchange-calendar-forecast-horizons.md).

The Prompt 54 GDELT JSON contracts are independent offline schema version
`1.0.0`; they do not change the FlatBuffers event envelope. A future precise
publication time, historical entity mapping, additional source collection, or
classifier output requires additive fields plus a new schema version. Existing
null publication values may never be reinterpreted, and existing metadata
records remain immutable. See
[ADR 0056](../docs/adr/0056-bounded-gdelt-gkg-query-and-advisory-events.md).

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
