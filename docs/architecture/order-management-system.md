# Deterministic order-management system

## Scope and safety boundary

`aegis::oms::DeterministicOms` is a provider-neutral, simulation-only order
lifecycle kernel. It accepts normalized inputs, returns a committed normalized
command, and never opens a socket or calls an exchange adapter. No live trading
capability, venue encoding, endpoint, or credential exists in this component.

The owner thread is the only writer. Readers use immutable snapshot handles or
quiescent inspection. Fixed capacities are 1,024 orders, 4,096 logical
executions, 8,192 receipt identities, and 8,192 journal records. Exhaustion
fails closed; there is no eviction.

## State and transition authority

Unlisted triples are forbidden. A forbidden venue rejection of an already
working order is safety-ambiguous and moves the order into explicit recovery.
Duplicate terminal acknowledgements do not change state.

| Input | Allowed transition(s) |
| --- | --- |
| Accept approved intent | absent → `CREATED` |
| Mark ready | `CREATED` → `READY` |
| Dispatch | `READY` → `PENDING_ACK` |
| New acknowledgement | `PENDING_ACK` → `WORKING`; preserves partial/cancel/replace pending state after an earlier race |
| New rejection | `PENDING_ACK` → `REJECTED` |
| Partial fill | open → `PARTIALLY_FILLED`; preserves `PENDING_CANCEL`, `PENDING_REPLACE`, or `CANCELED` race state |
| Final fill | open, pending, or `CANCELED` → `FILLED` |
| Cancel intent | open or replace-pending → `PENDING_CANCEL` |
| Cancel acknowledgement | open or pending → `CANCELED`; a filled order remains `FILLED` |
| Cancel rejection | `PENDING_CANCEL` → `WORKING` or `PARTIALLY_FILLED` |
| Replace intent | `WORKING` or `PARTIALLY_FILLED` → `PENDING_REPLACE` |
| Replace acknowledgement | `PENDING_REPLACE` → `WORKING`, `PARTIALLY_FILLED`, or `FILLED` |
| Replace rejection | `PENDING_REPLACE` → `WORKING` or `PARTIALLY_FILLED` |
| Expire | created, ready, pending-ack, working, or partially-filled → `EXPIRED`, subject to source rules |
| Recovery begin | any live state → `UNKNOWN_RECOVERY` |
| Recovery observation | `UNKNOWN_RECOVERY` → observed state or remains unknown |

The exhaustive property test evaluates all 2,304 state/input/target triples
against a separately declared allow-list.

## Identity and idempotency

- `IntentId` deduplicates business requests. Reusing it with different immutable
  intent bytes is rejected.
- `GlobalEventId receipt_id` deduplicates delivery. The same ID and bytes are a
  duplicate; the same ID with different bytes makes the service unsafe.
- `GlobalEventId execution_id` identifies one logical fill across primary and
  drop-copy sources. Matching economics reconcile without applying twice.
- Internal order IDs and 32-character lowercase hexadecimal client order IDs
  are deterministically domain-derived from session, account, configuration,
  intent, and exchange epoch.
- External order IDs are normalized opaque 128-bit values. They imply no
  provider representation.

## Commit and publication order

For each valid service call the OMS reserves bounded journal capacity, computes
the transition, updates the immutable snapshot, constructs the result, commits
the hash-chained journal record, and only then returns a gateway command. The
snapshot records the source journal sequence and hash. Canonical `OrderEvent`
serialization binds the transition and its risk, journal, and snapshot evidence
into schema v1.8.

The in-memory journal has a nonblocking hot-path API. `write_binary` and
`load_binary` are explicit off-hot-path operations. Durable storage scheduling,
fsync policy, and segment retention belong to the later journal/replay service.

## Restart and ambiguous state

Replay verifies the chain, configuration, input hashes, deterministic results,
and snapshot hashes. Historical gateway commands are never returned. Every
reconstructed live order receives a new journaled recovery-begin event and
becomes `UNKNOWN_RECOVERY`. An explicit recovery observation may establish
working, partially filled, filled, canceled, rejected, or expired state. An
ambiguous absence remains inhibited.

See [ADR 0024](../adr/0024-journal-first-fenced-deterministic-oms.md), the
[OMS recovery runbook](../operations/oms-recovery-runbook.md), and the
[licensed integration boundaries](licensed-integration-boundaries.md).

