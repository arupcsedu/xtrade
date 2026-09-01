# ADR 0004: Canonical FlatBuffers event contracts

- Status: accepted
- Date: 2026-08-28
- Decision owners: edge-core, data contracts, replay, and risk

## Context

Aegis-MX needs a stable C++ and Python contract for immutable intraday events.
Hot consumers cannot allocate, invoke an RPC, or parse a text representation.
Persistent bytes must remain replayable across additive evolution, reject
malformed input before access, make units and clocks explicit, and bind audit
records to exact content. The repository contains no licensed exchange protocol
specification, so this decision must remain venue-neutral.

## Decision

Use FlatBuffers 25.12.19 for version 1, pinned by source archive SHA-256. Check in
generated C++ and Python bindings and reproduce them with the pinned `flatc`.
C++ ingress uses structural verification followed by project-owned semantic
validation before it returns a zero-copy view.

Use `AMCR` for a typed `ContractRecord` payload and a size-prefixed `AMAE`
`AuditEnvelope` for persistence/network framing. SHA-256 covers the exact AMCR
bytes. An optional previous-envelope digest supports later journal chain checks.
All canonical builders follow generated field insertion order. A golden AMAE
fixture must be byte-identical in C++ and Python.

Use distinct inline 128-bit structs for every identifier, four distinct
nanosecond timestamp structs, explicit unit enums, integer price/quantity/value
fields, and `SchemaVersion`. Default enum zero means `UNKNOWN` and fails semantic
validation. A major mismatch fails closed. Additive fields follow the
[schema evolution policy](../../schemas/schema-evolution-policy.md).

The integrity digest is not a signature or authorization proof. Authentication,
signed configuration, and full journal frame checksums are separate boundaries.

## Consequences

Positive consequences:

- verified C++ consumers access tables without object materialization;
- fixed inline identifiers and timestamps need no string allocation;
- the same IDL generates C++ and Python bindings;
- additive table fields support controlled backward and forward reading;
- file identifiers, redundant discriminators, explicit versions, SHA-256, and
  semantic validation provide layered failure containment;
- protocol-neutral contracts avoid inventing licensed venue fields.

Costs and constraints:

- builders allocate unless a caller supplies a bounded allocator, so reference
  builders are outside the hot path;
- generated files are numerous and must be drift-checked;
- FlatBuffers verification establishes structural safety, not business
  validity, so every new record field needs project validation;
- enum additions require reader-first rollout because unknown values fail
  closed;
- SHA-256 adds deterministic CPU cost measured by the schema benchmark suite.

## Alternatives considered

Protocol Buffers were rejected for the innermost event representation because
normal generated access decodes into object state and does not provide the same
direct table view. Protobuf/gRPC remains allowed outside the hot path.

Cap'n Proto provides direct access but would introduce a second RPC-oriented
ecosystem and a more complex segment/capability model than this local immutable
record boundary needs.

A handwritten packed C++ ABI was rejected because padding, endianness,
evolution, Python support, and verifier correctness would become project-owned.

JSON and MessagePack were rejected for hot records because field names,
dynamic objects, and numeric ambiguity conflict with bounded, typed,
low-allocation access. JSON remains acceptable for human-readable golden
manifests, never as the canonical event bytes.

## Evidence and review triggers

The implementation is gated by cross-language golden tests, additive
compatibility tests, bounds and enum tests, SHA-256 standard vectors,
deserialization fuzzing, sanitizers, and validation/serialization/hash
benchmarks.

Revisit this ADR before a v2 wire format, a change to hash framing, an
unbounded field, a new timestamp domain, a licensed venue adapter boundary, or
any proposal to build/serialize dynamically in the hot path.

FlatBuffers is maintained by Google under Apache-2.0. Project references:
[repository](https://github.com/google/flatbuffers), [release
history](https://github.com/google/flatbuffers/releases), and
[changelog](https://github.com/google/flatbuffers/blob/master/CHANGELOG.md).
