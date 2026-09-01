# Colocated Event Bus and Shared State

## Scope and implementation plan

This edge-core slice provides bounded communication and immutable state only. It
contains no strategy, risk bypass, OMS, gateway, exchange protocol, or live
transmission capability.

The implemented vertical slice is:

1. fixed-capacity SPSC and MPSC handoff with explicit overload outcomes;
2. reader-pinned immutable snapshots and a bounded per-symbol registry;
3. a host-local forecast cache with version, schema, checksum, epoch, freshness,
   and corruption validation;
4. process heartbeat, crash takeover, and split-brain fencing; and
5. deterministic unit, mapped-memory, sustained race, sanitizer, and benchmark
   coverage.

Persistent journals, operating-system segment discovery/permissions, service
supervision, NUMA placement, capacity qualification, and model-forecast
FlatBuffer conversion remain later integration work.

## Execution and ownership model

| Primitive | Writer ownership | Reader ownership | Storage | Failure bound |
| --- | --- | --- | --- | --- |
| `SpscRing<T, N>` | One fixed producer | One fixed consumer | `N` inline values | One attempt |
| `MpscQueue<T, N, R>` | Multiple producers | One fixed consumer | `N` cache-line cells | At most `R` producer retries |
| `ImmutableSnapshotStore<T, N>` | One writer | Multiple pinned readers | At least three inline slots | Eight reader retries; one bounded slot scan |
| `RcuSymbolState<T, S, N>` | Quiescent registration, one writer per registry | Multiple readers | `S` stores of `N` slots | Bounded linear symbol lookup |
| `ForecastCacheView<N>` | One live process epoch | Multiple colocated readers | `N` fixed ABI slots | Eight retries per slot |

All payload types are trivially copyable. Memory is allocated by the owner when
the containing object or mapping is created; none of these operations allocates
in steady state. All capacities are compile-time constants. Caller lifetimes
must exceed every queue operation and snapshot read handle.

## Queue publication rules

### SPSC

The producer owns the producer cursor and reads the consumer cursor with
`acquire` before reusing a slot. It copies the payload, then stores the producer
cursor with `release`. The consumer acquire-loads that cursor before reading the
payload and release-stores its cursor only after the copy completes. Local
cursor reads and metrics use `relaxed` where they do not publish payload.

### MPSC

Each cell sequence is initialized to its index. A producer acquire-loads the
cell sequence, reserves the global position with compare-and-exchange, copies
the payload, and release-stores `position + 1`. The single consumer acquire-loads
that sequence, copies the payload, then release-stores `position + capacity`.
The next producer generation therefore cannot overwrite a value before the
consumer finishes copying it.

`FULL` means the consumer has not released the target generation. `CONTENTION`
means the bounded compare-and-exchange retry budget expired. Neither status is
silently retried by the primitive. A reserved but unpublished position yields
`PRODUCER_INFLIGHT` to the consumer.

## Overload and observability

`REJECT_NEWEST` preserves all previously accepted inputs and reports rejection.
`FAIL_CLOSED` atomically invalidates the queue; subsequent producer and consumer
operations return `INVALIDATED`, so a downstream user cannot silently drain and
treat a discontinuous stream as healthy. The owning data path must expose the
error in its health state and require its defined recovery process.

Metrics are fixed counters: producer/consumer sequence, current and maximum lag,
accepted and consumed events, full and contention rejection, and invalidation.
Current lag is a bounded lock-free sample and may be conservative while both
cursors move; maximum lag is sampled by publishers. They carry no dynamic labels
and do no logging. Counter rollover requires a quiescent process-epoch restart.

## Immutable snapshots

The store never modifies its active slot or any reader-pinned slot. The odd/even
sequence protects the write claim; reader counts protect lifetime. A release
store of the active index is the publication point. A reader must retain its
move-only handle while dereferencing the value. Checksums detect unexpected
memory corruption and are supplied by the owning record contract.

Reader acquisition and publication times are injected unsigned monotonic
nanoseconds. Stale readers are visible through handle checks and aggregate
metrics. Time regression is treated as stale. If every inactive slot remains
pinned, publication returns `NO_FREE_SLOT`; a writer never waits or reclaims a
live reader's memory.

Symbol registration is an initialization or quiescent control operation. It is
not valid to mutate the registry concurrently with readers. Unknown or invalid
instrument identifiers return explicit failure.

## Forecast shared-memory ABI

The region begins with immutable layout identity followed by a release-published
ready marker, writer epoch state, counters, and fixed slots. Attach checks:

- address alignment and mapped size;
- magic, layout size, capacity, and static-header hash;
- ABI major and minimum minor version;
- canonical forecast schema version;
- deployment-supplied region epoch; and
- optionally, a live writer heartbeat.

A forecast record uses canonical 128-bit identifiers and integer values only.
It binds instrument, model/version, forecast, feature snapshot, configuration,
session, units, horizon, creation/deadline, schema, writer epoch, cache
generation, and payload hash. A late forecast, stale writer, epoch mismatch, bad
identifier, invalid range, hash mismatch, or mixed generation returns an error
and no usable record.

The cache does not create or name an OS segment. A deployment owner maps aligned
shared memory with restrictive permissions, creates a fresh nonzero region
epoch, formats once, and passes the mapping to `ForecastCacheView`. Production
readiness must additionally prove supervisor fencing of a timed-out writer and
remove stale mappings during controlled startup. Tests use an anonymous
`MAP_SHARED` mapping and a child-process crash to verify actual interprocess
visibility and takeover.

## Restart and corruption behavior

An epoch claimant cannot replace a live different owner. A stale owner can be
replaced only by a strictly higher epoch. Same-epoch/different-process claims
are split brain. Identity-field corruption fails inspection. A prior writer
cannot heartbeat or publish after losing the epoch.

An odd cache slot represents an interrupted publication. A stale takeover
clears its atomic words and advances it to an even generation. If the old
process was paused rather than dead and resumes physical writes, sequence CAS,
record hash, and writer epoch cause readers to reject the result. The service
must remain not-ready until the supervisor has fenced that process.

Shared memory is never authoritative. Audit and restart recovery use the
canonical append-only journal when that later component exists.

## Validation

The deterministic test seed remains `20260828`. Tests cover empty/full,
capacity/index wrap, rejection/invalidation, sustained SPSC and MPSC races,
per-producer order, immutable-reader races, no-free-slot, stale readers, bounded
symbols, process crash/restart, split brain, schema/ABI/region mismatch, stale
heartbeat, partial publication recovery, late forecasts, and record/header
corruption. See [Event Bus Testing](../testing/event-bus-testing.md).

Benchmark results are host-specific evidence, not admission thresholds. A
production capacity decision requires pinned CPU/NUMA topology, frequency and
turbo policy, compiler/build identity, representative payloads, occupancy, and
multi-hour burst tests.
