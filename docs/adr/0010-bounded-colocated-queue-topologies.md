# ADR-0010: Bounded colocated queue topologies

- Status: Accepted
- Date: 2026-08-29
- Decision owners: Edge Core
- Governing contract: [Engineering Contract](../architecture/engineering-contract.md)

## Context

Colocated components need nonblocking event handoffs with fixed memory use and
observable overload. One queue algorithm is not optimal for every ownership
topology. A generic dynamically allocated or mutex-backed queue would hide
blocking and allocator behavior, while an overwrite-on-full ring could silently
erase decision evidence.

## Decision

Use a compile-time-capacity SPSC ring whenever one producer and one consumer can
be established by construction. Producer and consumer cursors occupy distinct
64-byte cache lines. The payload array is preallocated, capacity is a power of
two, and only trivially copyable payloads are accepted.

Use a bounded sequence-cell MPSC queue only where multiple producers are an
actual requirement. Every cache-line-aligned cell carries a generation
sequence. Producers reserve a position with compare-and-exchange and publish the
payload through the cell sequence; the one consumer releases that generation
after copying the payload. Producer attempts have a compile-time retry bound and
can return `CONTENTION` independently of `FULL`.

Both structures expose monotonic producer and consumer sequences, current and
maximum consumer lag, accepted/consumed counts, full rejections, and
invalidation. The default overload policy rejects the newest input explicitly.
Safety-critical callers may select `FAIL_CLOSED`, which permanently invalidates
the instance until a quiescent restart. Neither policy overwrites unread data.
There is no implicit blocking, sleeping, allocation, logging, or callback.

Queue objects have fixed thread ownership. SPSC admits exactly one producer and
one consumer. MPSC admits multiple producers but exactly one consumer. Reset is
quiescent-only; an MPSC instance is reconstructed after process restart rather
than reset concurrently. Sequence rollover at the integer limit requires a
quiescent restart and is outside a process epoch.

## Consequences

SPSC provides the smaller synchronization cost and is the default. MPSC pays
for per-cell cache lines and producer compare-and-exchange contention, but a
stalled producer cannot cause another producer to wait without a bound. A
consumer can observe `PRODUCER_INFLIGHT` when a position is reserved but not yet
published.

Compile-time capacities increase object size and must be selected from replay
and burst benchmarks. Rejection sacrifices availability but preserves the fact
that continuity was lost. The queue itself does not decide whether a dropped
event invalidates a feed, journal, forecast, or order-intent path; the owning
component must map the explicit result to its documented fail-closed policy.

## Alternatives considered

`std::queue` plus a mutex and condition variable was rejected because it can
block and allocate. A single MPSC implementation for all paths was rejected due
to unnecessary atomic traffic and memory use for SPSC ownership. Overwrite-old
and unbounded growth were rejected because they can hide loss or exhaust memory.
Unbounded spin was rejected because it violates bounded failure behavior.

## Review triggers

Revisit this ADR before adding multiple consumers, blocking waits, dynamic
capacity, cross-NUMA placement, process-shared queues, sequence rollover within
an epoch, or a drop/overwrite policy.
