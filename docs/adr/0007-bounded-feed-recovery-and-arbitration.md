# ADR-0007: Bounded feed recovery and redundant-feed arbitration

- Status: Accepted
- Date: 2026-08-29
- Decision owners: Market Data and Edge Core
- Governing contract: [Engineering Contract](../architecture/engineering-contract.md)

## Context

SMX/1 provides a deterministic, license-clean packet format, but a decoder alone
cannot establish that normalized events are continuous or safe. The feed boundary
must validate transport identity, preserve raw evidence, reconcile redundant A/B
copies, expose every continuity failure, and recover without blocking or allocating
in the steady-state packet loop.

The repository has no licensed venue specification. Any decision that assumes a
real venue's framing, sequence, retransmission, snapshot, or session semantics would
be fabricated.

## Decision

The feed framework uses fixed-layout value types, bounded single-owner rings, and
nonblocking interfaces. Receivers supply session and feed-leg metadata alongside
raw bytes. Decoders must validate that metadata and the protocol-carried venue and
channel identities before an event reaches sequencing.

Continuity is established once, after A/B arbitration. A first valid copy is held
in a bounded arbitration window until it is the next expected sequence. A matching
copy from the other leg is redundant evidence; a conflicting hash is fatal. A
future sequence publishes degraded health immediately and starts nonblocking
retransmission. No post-gap event is published until every missing sequence is
replayed in order. A validated snapshot is the fallback after rejection or timeout;
the snapshot publisher must atomically rebuild downstream state before sequencing
is reset.

The packet-loop failure policy is fail closed:

- raw-journal rejection invalidates the channel before decoding;
- receiver overrun, malformed data, identity mismatch, conflicting A/B copies,
  out-of-window sequence, or normalized-publisher rejection invalidates the
  channel;
- individual-leg gaps degrade health but do not stop a continuous arbitrated
  stream supplied by the other leg;
- a canonical gap changes state before recovery is requested and is never erased
  from cumulative health counters; and
- health-publication failure prevents a transition to usable health.

All time is injected as process-monotonic nanoseconds. Retransmission and snapshot
interfaces enqueue or poll bounded work; they may not perform synchronous network,
disk, database, logging, or allocation work in the packet loop. The hot
`DataQualityState` is a fixed-layout projection of the canonical schema. FlatBuffer
serialization remains an off-path concern.

Real-feed support is limited to abstract licensed-adapter interfaces. A concrete
adapter requires the provider specification, redistribution rights, certification
fixtures, and reviewed recovery semantics listed in
[Licensed Integration Boundaries](../architecture/licensed-integration-boundaries.md).

## Consequences

The framework is deterministic and overload behavior is observable, but capacities
are compile-time bounded and must be selected from measured deployment workloads.
Fail-closed publisher behavior can sacrifice availability to preserve correctness.
The synthetic snapshot contains a complete bounded book image; licensed venues may
need a venue-specific snapshot decoder behind the same interface.

This phase does not add sockets, multicast membership, a real retransmission
service, venue credentials, order entry, or any trading capability.
