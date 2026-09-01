# ADR 0025: Default-off synthetic and paper gateway boundary

- Status: Accepted
- Date: 2026-08-31
- Owners: gateways, OMS, risk, replay, operations

## Context

The OMS now returns a journal-committed normalized command, but it deliberately
does not transmit. Phase 9 needs an independently testable final safety boundary,
session behavior, and realistic local execution fixtures without inventing a
venue protocol or creating an accidental path to a real endpoint. Licensed
order-entry, session, recovery, reject, fill, and FIX dictionary semantics are
not present in the repository.

## Decision

The `cpp/execution` boundary owns `IExchangeGateway`, a fixed-layout session and
sequence model, bounded rate limiters, a hash-chained local audit journal,
normalized response events, and synthetic acknowledgement, reject, and
execution decoders. OMS continues to own the normalized command and lifecycle
input contract; no reverse OMS dependency on a gateway is introduced.

Ordinary builds contain `SIMULATION`, `PAPER`, and `HALTED` only. The
`AEGIS_ENABLE_LIVE_TRADING` option defaults off. The live-only adapter interface
is conditionally absent unless that option is explicit. Even an enabled build
starts only in simulation or paper and contains no live implementation,
transport, endpoint, credential support, activation API, or protocol encoder.
The build flag records compile provenance; it is not live eligibility.

Every new or replace command is independently checked at the final boundary in
deterministic order for command integrity, session state, configuration and
account/venue/instrument scope, fencing, current exact risk approval, mode,
journal readiness, authoritative market state, clock state and freshness, feed
health, book validity, official halt, kill state, rate capacity, and bounded
storage. Unknown fails closed. Synthetic/paper cancels may remain available as
protective actions under degraded market state because their complete semantics
are repository-owned; no real adapter may inherit that policy without a
venue-specific decision.

`SyntheticExchangeGateway` and `PaperBrokerGateway` are in-memory only. Both
publish mode in every normalized event, audit record, and metrics snapshot. The
paper model uses fixed arrays and integer arithmetic for acknowledgement,
reject, partial-fill, queue-ahead, cancel-race, fee, slippage, impact, halt, and
auction fixtures. It is infrastructure validation, not a claim about real fill
probability, economics, queue priority, or broker behavior.

Native and FIX-compatible boundaries accept only opaque bounded frames and
normalized internal contracts. They define no fields, tags, dictionaries,
wire layouts, logon messages, endpoints, transports, or credentials. Concrete
implementations remain blocked on authorized specifications.

## Consequences

- Models and intelligence still cannot link to or address a gateway.
- Steady-state submission, scheduling, decoding, throttling, and audit append use
  fixed storage and no intentional dynamic allocation.
- Duplicate command identity is idempotent; identity reuse with different bytes
  is an unsafe split-brain condition.
- Sequence gaps, heartbeat loss, journal exhaustion, ambiguous recovery, and
  stale fencing inhibit submission and are never forwarded as healthy events.
- The local audit chain is an in-memory Phase 9 record. Durable integrated
  journaling and faithful end-to-end replay remain Phase 10 work.
- Real session, resend, cancel-on-disconnect, drop-copy, fee, queue, auction, and
  recovery behavior cannot be inferred from the local model.

## Rejected alternatives

- A runtime-only `live=true` flag cannot enforce build provenance or remove live
  code from ordinary artifacts.
- Reusing the synthetic wire layout for a venue would fabricate compatibility.
- Trusting the OMS risk decision without final revalidation would leave mode,
  freshness, halt, kill, data, clock, fencing, and capacity bypasses.
- Blocking on disk, control-plane RPC, or broker API during the final predicate
  violates the hot-path contract.
