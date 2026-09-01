# ADR-0011: Colocated memory ordering and shared-state ABI

- Status: Accepted
- Date: 2026-08-29
- Decision owners: Edge Core and Model Contracts
- Governing contract: [Engineering Contract](../architecture/engineering-contract.md)

## Context

Immutable book, feature, risk, and forecast views must cross threads or
colocated processes without putting locks, allocation, serialization, or RPCs
in readers. Correctness depends on precise publication edges, reader lifetime,
process restart fencing, and detection of partial or incompatible state.

## Decision

All hot shared-state algorithms use lock-free fixed-width atomics and explicit
C++ acquire/release ordering. Relaxed operations are limited to thread-local
cursor access, metrics, or payload words protected by a separate publication
sequence. The complete reasoning is normative in
[Colocated Event Bus and Shared State](../architecture/colocated-event-bus.md).

In-process immutable state uses a reader-pinned, multi-slot snapshot store. One
writer chooses a non-active slot with zero readers, marks its sequence odd,
writes the value and checksum, makes the sequence even, then release-publishes
the active slot. A reader acquire-loads the active slot, pins it, revalidates
both sequence and active index, verifies the checksum, and holds the pin for the
returned handle lifetime. At least three slots are required so a slow reader
does not force mutation of the active image. Registration and writing are
single-owner operations.

The process-shared forecast cache uses a fixed, integer-only, aligned ABI. A
release-published format marker protects immutable magic, ABI major/minor,
layout size, capacity, schema version, region epoch, and header checksum.
Payloads are arrays of lock-free atomic 64-bit words guarded by an odd/even
sequence and an immutable payload hash. Readers retry a fixed number of times,
then return `BUSY`; malformed, mixed, stale, late, wrong-epoch, incompatible, or
corrupt data is never returned as valid.

A process identity consists of nonzero process ID, strictly increasing epoch,
monotonic heartbeat, and an identity guard. A live different owner is split
brain. Takeover requires heartbeat expiry and a higher epoch. The old epoch is
fenced from publishing, and abandoned odd slots are cleared and recovered. A
paused old process can still issue physical atomic writes after takeover;
generation compare-and-exchange, epoch validation, and payload hashes ensure
such data fails closed. Production takeover additionally requires supervisor
confirmation that the prior process has been terminated before the replacement
is declared ready.

The supported shared-memory runtime is a homogeneous host whose processes use
the same Aegis build ABI, native endianness, standard-library atomic ABI, and
64-bit lock-free atomics. Attach validation is mandatory on every mapping. This
cache is a local ephemeral acceleration structure, never an audit or recovery
source. Canonical FlatBuffer `ModelForecast` records remain the persistent and
network contract.

## Consequences

Readers never acquire a mutex and do bounded work. Slow snapshot readers consume
a bounded slot; writers report `NO_FREE_SLOT` instead of mutating pinned state.
Stale-reader detection is conservative and can mark an ambiguous lifetime stale
rather than under-reporting it. Caller-provided timestamps avoid clock reads in
critical loops.

The shared ABI cannot be copied between different machine architectures,
standard libraries, or ABI majors. A rolling upgrade must create a new region
epoch and mapping, validate it, redirect readers, then retire the old mapping.
It must not reinterpret an incompatible segment in place.

## Alternatives considered

`std::shared_ptr` atomic snapshots were rejected because reclamation and final
destruction can allocate or execute unpredictably. A two-slot seqlock over
non-atomic payload bytes was rejected because C++ readers and writers would have
a data race even if checks detected tearing. Mutexes and robust process-shared
locks were rejected for the hot read path. Treating shared memory as the durable
forecast contract was rejected because its ABI is intentionally host-local.

## Review triggers

Revisit this ADR before adding multiple snapshot writers, remote shared memory,
heterogeneous binaries, 32-bit platforms, non-lock-free atomics, automatic
takeover without supervisor fencing, mutable reader access, or shared state that
can authorize an order without fresh canonical validation.
