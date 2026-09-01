# ADR 0006: License-clean deterministic synthetic exchange

- Status: accepted
- Date: 2026-08-29
- Decision owners: market-data, edge-core, order-book, replay, and testing

## Context

Aegis-MX needs high-volume market-data, packet-fault, normalization, and replay
fixtures before any licensed feed adapter can be implemented. The repository has
no authorized exchange protocol specification. Copying, approximating, or
reverse-engineering a real venue layout would violate the engineering contract
and would also make test semantics depend on undocumented venue behavior.

The canonical v1 `MarketEvent` contract contains aggregate `BookUpdate`, trade,
quote, auction-imbalance, and status payloads. It deliberately does not expose a
native venue order identifier. Tests nevertheless need both order-level and
price-level source messages.

## Decision

Create a repository-owned synthetic protocol named **SMX/1** under the
`market-data` boundary. Its magic values, field order, message types, checksums,
and semantics are designed solely for Aegis-MX testing and are not modeled on a
real exchange.

- Native order-level add, modify, and cancel messages carry a synthetic 64-bit
  order identifier. Normalization publishes the resulting aggregate level as a
  canonical `BookUpdate`; the private order identifier does not alter the v1
  schema.
- Native price-level messages directly publish the same canonical `BookUpdate`.
  Every mutation includes the complete resulting level quantity and order count,
  so canonical replay can reconstruct the book without hidden state.
- Trades are explicit tape events and do not implicitly mutate the synthetic
  book. Any book change caused by a test scenario is a separate reconstructible
  update.
- The generator core is pull-based and writes into caller-owned fixed-size
  objects. It allocates bounded state during construction and performs no
  allocation, I/O, sleep, system-clock read, or blocking operation per event.
- Use a specified SplitMix64 generator and integer sampling rather than standard
  library distributions. Configuration uses integer nanoseconds, ticks,
  quantities, and parts per million.
- Use FNV-1a-64 over explicitly encoded bytes for stable synthetic event,
  configuration, packet, and book hashes. These hashes are deterministic error
  detectors, not authentication. Canonical `AuditEnvelope` records retain the
  existing SHA-256 integrity contract.
- Keep ordered logical canonical output separate from fault-injected raw
  capture. Duplicate, drop, and reorder scenarios change packet delivery only;
  they never rewrite the logical oracle.
- Capture generation streams records to disk and canonical serialization occurs
  outside the generator hot loop. Benchmark mode measures the allocation-free
  generator path separately from storage and FlatBuffers costs.
- Verification tracks sequence, duplicate, gap, reorder, staleness, halt, and
  crossed-book state with fail-closed per-channel behavior. An intentional fault
  succeeds verification only when its exact declared scenario expectation is
  observed; an undeclared fault fails.

The four command-line tools contain no endpoint, socket destination, account,
credential, order-entry, or transmission capability. `synth-exchange-stream`
replays SMX/1 frames to a file descriptor or file only; it is not a venue
gateway.

## Consequences

Positive consequences:

- source streams, packet faults, canonical events, and final-book oracles are
  reproducible from configuration and seed;
- order-level and price-level normalization share one canonical downstream
  contract;
- parsers and books can be tested before licensed specifications exist;
- bounded core state supports realistic throughput and overload measurement;
- protocol bytes and fixtures are safe to publish with the repository.

Costs and constraints:

- SMX/1 behavior proves only internal invariants, not any venue's priority,
  auction, correction, recovery, or packet semantics;
- one SMX/1 packet carries exactly one fixed-size message in version 1, favoring
  deterministic fault isolation over bandwidth efficiency;
- the text final-book oracle is an offline artifact and is not a hot-path book
  representation;
- canonical FlatBuffer output allocates at the serialization boundary and is
  excluded from core generator throughput claims.

## Alternatives considered

Reusing a public description of a real protocol was rejected because licensing,
version, certification, and semantic authority are absent.

Adding a synthetic order identifier to canonical v1 `MarketEvent` was rejected
because it would couple normalized consumers to a source-specific concept and
require unnecessary schema evolution.

Using `std::mt19937` plus standard distributions was rejected because library
distribution mappings are not a portable byte-stream contract.

Using JSON for packet or capture output was rejected because numeric parsing,
size, allocation, and ambiguous framing are poor parser and throughput fixtures.

## Evidence and review triggers

Acceptance requires fixed-seed golden hashes and counts, repeated-run byte
identity, parser corruption/truncation tests, property tests across seeds,
scenario expectation tests, canonical-envelope validation, replay/book equality,
bounded-memory evidence, sanitizer builds, parser fuzzing, and release
throughput benchmarks.

Revisit this ADR before changing SMX/1 bytes or hash framing, adding variable or
unbounded fields, changing native-to-canonical mapping, allowing network
destinations, or using any licensed venue/provider specification.
