# Aegis-MX Live-Trading Safety

| Field | Value |
| --- | --- |
| Status | Normative safety policy; implementation not yet authorized |
| Effective date | 2026-08-28 |
| Default mode | `SIMULATION` or `PAPER` |

## Safety objective

Real-money transmission is prohibited by default. Aegis-MX SHALL remain unable
to transmit to a real venue unless live capability is intentionally compiled,
every independent runtime interlock is currently valid, and the activation is
explicitly authorized and durably audited. This document defines the minimum
control model. It does not enable live trading or provide exchange protocol
details.

The [engineering contract](../architecture/engineering-contract.md) takes
precedence. Unknown, missing, stale, malformed, inconsistent, or unverifiable
state is unsafe and fails closed.

The checked-in [colocated edge deployment](../architecture/colocated-edge-deployment.md)
contains only simulation and paper profiles. Its production-shaped profile is
explicitly named `production-disabled`, remains in simulation mode, and creates
a local inhibit before any edge service starts. It is not a live-capable
configuration or an activation path.

The boundary and mode-state decisions behind this policy are recorded in
[`../adr/0001-safety-boundaries-and-live-activation.md`](../adr/0001-safety-boundaries-and-live-activation.md).

## Operating modes

| Mode | Venue behavior | Permitted by default | Persistence across restart |
| --- | --- | --- | --- |
| `SIMULATION` | Uses deterministic synthetic/reference venue behavior; no real endpoint or credential is reachable | Yes | May restart in `SIMULATION` |
| `PAPER` | Uses a venue-approved non-money environment or isolated internal paper adapter | Yes, when explicitly configured | May restart in `PAPER` only if configuration remains valid; otherwise `SIMULATION` |
| `LIVE_ARMED` | Live-capable build and all interlocks validated, but transmission still inhibited pending final activation commit | No | Never restored automatically |
| `LIVE_ACTIVE` | Gateway may transmit only risk-approved commands while every interlock remains continuously true | No | Never restored automatically |
| `HALTED` | New transmission inhibited; reconciliation and venue-aware protective actions follow approved policy | Safe transition from every mode | Restart remains non-live |

`LIVE_ARMED` and `LIVE_ACTIVE` SHALL NOT exist in builds compiled without the
live-trading option. A simulation or paper binary SHALL have no real venue
credentials, routes, or live protocol plugin available. Names and UI styling are
not safety boundaries; the final gateway must enforce mode.

## Live activation predicate

All conditions below are independently required at the final transmission
boundary:

1. **Compile-time capability:** the binary was built with an explicit live option
   that defaults to off, and its immutable build manifest records the option,
   source revision, toolchain, artifact digest, and approval provenance.
2. **Signed runtime configuration:** the complete effective live configuration
   has a valid signature from an authorized key, a recognized schema version,
   environment and gateway scope, issue and expiry times, monotonic revision or
   anti-replay data, and a verified content hash. Secrets are referenced, not
   embedded.
3. **Explicit operator authorization:** a strongly authenticated, authorized
   operator grants a bounded, expiring authorization for the exact environment,
   gateway, account, strategy set, instrument scope, configuration hash, and
   build identity. Generic or cached consent is invalid.
4. **Healthy risk services:** deterministic pre-trade risk is ready, its policy
   and snapshot are valid and current, limits are loaded, required independent
   risk/kill-switch channels agree, and the request has a valid risk approval.
5. **Valid clock synchronization:** approved PTP/hardware sources report offset,
   uncertainty, state, and freshness within limits established by ADR and
   operational policy. Wall-clock appearance alone is insufficient.
6. **Valid market-data state:** required feeds are authenticated, sequenced, and
   fresh; book state is valid; instrument and session state are known; no
   applicable halt exists; and recovery from gaps is complete.
7. **Auditable activation record:** the requested transition and all evidence
   above are written to the mandatory audit journal, integrity protected, and
   durably acknowledged before live transmission is possible.
8. **Single authoritative gateway:** leadership/fencing proves that exactly one
   authorized transmitter owns the account/session scope and split brain is not
   suspected.
9. **No kill or inhibit condition:** local, remote, compliance, venue, and
   automated kill switches are clear, and shutdown or maintenance inhibition is
   not active.

Every predicate is evaluated as `true` or `not true`; there is no permissive
`unknown`. An implementation SHALL evaluate them again at transmission time,
not only at activation time.

The dormant adapter boundary enforces this shape with a single-use
`VerifiedLiveTransmissionCapability`. Its authenticated evidence binds the exact
encoded frame and all authority digests above. Only the verifier can construct
the type accepted by a future licensed transmitter. The non-virtual transmission
wrapper consumes and invalidates it before invoking an adapter and rejects a
different frame or a second use. The issuer itself is bound to the configured
session, configuration digest/hash, epoch, fencing token, and clock-age policy;
it refuses unsafe final state. This is a structural guard, not a live
implementation or venue certification.

## Activation sequence

1. Start the gateway in `SIMULATION` or `PAPER`; publish build identity, mode,
   health, readiness, configuration hash, clock state, data state, risk state,
   leadership/fencing state, and kill-switch state.
2. Verify the live-capable build manifest and artifact integrity. A non-live
   build rejects the request with a stable reason code.
3. Load and validate the signed configuration without applying unsafe defaults.
   Bind the validated hash to all later evidence.
4. Establish and validate time, market data, risk, account/session, and unique
   gateway authority. Reconcile any prior session before arming.
5. Obtain an explicit, scoped, expiring operator authorization through a
   strongly authenticated control-plane action. At least two-person approval
   may be required by a later compliance policy; no design may prevent it.
6. Construct an activation record containing all evidence and append it to the
   mandatory journal. Wait off the hot path for durable acknowledgement.
7. Transition atomically to `LIVE_ARMED`, re-evaluate every predicate, and
   record the transition.
8. Require the documented final activation action, bind it to the armed record,
   re-evaluate every predicate, durably record the result, and only then enter
   `LIVE_ACTIVE`.
9. Continue evaluating all predicates. Authorization is a lease, not a permanent
   mode bit.

Retries are idempotent and tied to the same activation identity. A partial,
timed-out, ambiguous, or restarted sequence remains non-live. There is no
automatic transition from paper, halted, recovered, or restarted state to
`LIVE_ACTIVE`.

## Activation audit record

The append-only activation record SHALL contain or cryptographically bind:

- activation identity, request time, decision time, and mode transition;
- operator identity, authentication context, authorization scope, expiry, and
  control-plane request identity;
- binary digest, source revision, build version, toolchain identity, compile-time
  live flag, and artifact/signing provenance;
- effective configuration hash, schema/revision, signer identity, signature
  validation result, scope, issue/expiry, and anti-replay value;
- gateway, environment, account/session, strategy, and instrument scope;
- risk service identities, health, policy version, snapshot identity/freshness,
  and kill-switch state;
- clock sources, synchronization state, offset/uncertainty, last update, and
  validation result;
- market-data sources, sequence/freshness/book/session state, and validation
  result;
- gateway leadership term, fencing token, and split-brain check;
- journal segment/sequence, record schema, previous-record linkage, and checksum;
- each predicate result and stable reason code; and
- the resulting state, including rejected activation attempts.

The record SHALL exclude credentials and raw secret material. Activation history
is retained according to compliance policy and must support independent replay
and verification.

## Continuous revocation and halt behavior

Any of the following immediately revokes permission for new live transmission:

- local, remote, compliance, automated, or venue kill/halt signal;
- expired/revoked authorization or configuration, signature failure, or scope
  mismatch;
- risk service unavailable/unhealthy, stale snapshot, failed approval, or
  inconsistent independent state;
- clock loss, excess uncertainty/offset, timestamp regression, or stale
  clock-health evidence;
- market-data gap, stale feed, invalid/crossed book, unknown instrument/session
  state, or trading halt;
- leadership loss, fencing failure, session ambiguity, network partition that
  invalidates authority, or suspected split brain;
- journal inability to record mandatory safety/transmission evidence according
  to its future approved failure policy;
- malformed or inconsistent gateway/venue state, capacity exhaustion, invariant
  violation, or internal corruption; or
- shutdown, restart, deployment, configuration replacement, or operator logout
  when the authorization policy requires the active identity.

The gateway atomically inhibits new transmissions and records a transition to
`HALTED` when possible. The inhibit mechanism must remain effective even if
models, strategies, the control plane, or observability systems fail.

Outstanding-order handling is not universally “cancel all.” During a partition
or uncertain venue session, blind retries can duplicate actions. Each venue
adapter requires an approved, tested policy for idempotency, cancels, mass
cancels, reconciliation, and flattening. Until that policy exists and current
venue state is known, Aegis-MX remains halted and escalates to the operator.

## Kill switches

Kill switches are independent of model and strategy logic and are enforced at
the gateway. They SHALL be authenticated where remote, idempotent, monotonic
toward safety, observable through more than one channel where practical, and
testable without real transmission. A kill action requires no model consensus.

Resetting a kill switch does not reactivate trading. Reset only clears one
inhibit after cause investigation and authorization; the complete activation
sequence and every current predicate remain required.

## Order authorization boundary

Models publish forecasts only. Strategies and ensembles produce order intents
only. Every intent traverses deterministic pre-trade risk, and a risk approval is
bound to the exact immutable command fields, snapshot, limits, deadline, and
configuration. Mutation, reuse, expiry, or scope mismatch invalidates approval.

Immediately before serialization/transmission, the gateway checks:

- `LIVE_ACTIVE` and the activation identity;
- current safety predicates and fencing token;
- risk-approval identity, integrity, scope, and freshness;
- integer price ticks and integer quantity validity and bounds;
- instrument, account, session, trading status, and book/data validity;
- duplicate/idempotency and rate/capacity controls; and
- manipulative-behavior and applicable compliance controls.

Failure produces a deterministic rejection and audit event. There is no direct
model-to-gateway interface and no “operator override” that skips deterministic
risk.

## Manipulation and compliance controls

Live approval requires compliance review of strategy intent and observed
effects, including controls against spoofing, layering, wash trading, quote
stuffing, marking the close, front-running, self-trading, restricted-instrument
violations, and venue rule breaches. Rate and cancellation limits, self-trade
prevention, surveillance events, and reviewable rationale are mandatory before
live operation. Simulation and paper testing must exercise these controls; those
modes are not exemptions for developing abusive behavior.

## Operational readiness

The consolidated non-live operator entry point is the
[operational-readiness package](operational-readiness-package.md). Its production
activation checklist is currently `PROHIBITED` and cannot grant authority.

Before any live-capable deployment, operators need approved runbooks and drills
for at least:

- activation rejection and configuration/signature failure;
- stale/invalid market data and feed recovery;
- clock degradation and loss of synchronization;
- risk-service degradation or conflicting risk state;
- split brain, leadership loss, and network partition;
- local and remote kill switches;
- journal degradation, disk-full, corruption, and recovery;
- venue disconnect, ambiguous acknowledgement, session recovery, and order
  reconciliation;
- safe shutdown and restart; and
- credential or signing-key compromise.

Readiness also requires access-control review, credential separation, on-call
ownership, alert routing, retention policy, backup/restore evidence, capacity
tests, fault-injection results, venue certification where applicable, and a
successful paper-trading soak under representative load.

## Service health, readiness, and observability

Gateway health and readiness SHALL expose machine-readable reason codes without
secrets. At minimum, publish build/version, current and requested mode,
configuration hash, activation identity, risk state, clock state, data/book
state, leadership/fencing state, journal state, kill-switch state, queue/capacity
state, and last safe transition.

Metrics use bounded labels. Structured logs and traces carry stable correlation
identifiers but not credentials, raw signed secrets, licensed payloads, or
unbounded external text. Alerting must distinguish process liveness from safe
trading readiness. Failure of metrics or tracing cannot grant readiness.

## Graceful shutdown and restart

Shutdown first inhibits new order transmission, records the state change,
coordinates venue-aware handling of outstanding orders under a bounded approved
policy, flushes mandatory audit state within a bounded deadline, and terminates.
If graceful completion is impossible, the process remains inhibited and records
or externally signals the incomplete state when possible.

After every restart, live activation and operator authority are absent. The
gateway reconciles journal, account/session, outstanding orders, market data,
clock, risk, configuration, and unique leadership before it may be armed again.
Stale cached mode or authorization state cannot restore `LIVE_ACTIVE`.

## Break-glass policy

There is no break-glass path that bypasses compile-time gating, deterministic
risk, kill switches, market-data validity, clock validity, single-writer
authority, or activation audit. Emergency controls may make the system safer by
halting transmission or invoking an approved venue-specific protective action;
they cannot create live authority. Manual actions outside Aegis-MX remain under
the broker/venue and organizational incident procedures and must be reconciled
before reactivation.

## Current limitation and next dependency

This repository now has a local simulation/paper gateway, final deterministic
safety predicate, bounded audit journal, session/recovery model, synthetic
decoders, and a default-off compile boundary for future live-only adapters. It
still has no live adapter, socket/transport, endpoint, credential path,
configuration signer, operator authorization service, durable activation
journal, or approved account/venue policy. The `PAPER` implementation is an
isolated internal broker model; it does not connect to a broker paper endpoint.
Therefore no real or provider paper order transmission is implemented or
authorized.

Before any real adapter work, accepted ADRs must complete configuration signing
and trust roots, operator authorization and expiry, durable activation-journal
policy, venue-specific recovery, and approved risk/account/session policies.
Authorized specifications and certification evidence are mandatory; the local
synthetic/paper semantics cannot be assumed for a venue.
