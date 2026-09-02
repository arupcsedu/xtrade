# ADR 0033: Signed two-person configuration control

- Status: Accepted
- Date: 2026-09-02
- Owners: control-plane, risk, edge-core, operations, compliance

## Context

Strategy, venue, risk, model, ensemble, session, calendar, kill, trading-mode,
and operator-role policy affect whether the edge may admit a new order. A
control-plane outage must not put an RPC in pre-trade evaluation or extend an
expired authorization. A stolen old snapshot must not refresh the edge's
offline lease. Critical changes require attributable separation of duties, and
an emergency inhibit must remain usable without creating an unsafe reset path.

## Decision

1. The complete administrative state is one bounded, versioned, immutable JSON
   snapshot. Set-like fields are sorted canonically, SHA-256 hashed, and signed
   with Ed25519. The first revision has no parent; each subsequent normal
   revision binds the exact active parent hash.
2. Every snapshot is safety-critical. An authorized author proposes it, a
   distinct authorized principal approves it, and an activator may select it
   only after its staged activation time and inside its UTC validity interval.
3. Operator authentication is an external boundary. The service API accepts
   only short-lived authenticated contexts with authority epoch, principal,
   request ID, authentication time, and expiry, then enforces snapshot RBAC and
   persistent request replay protection. Bootstrap roles apply only before the
   first configuration activates.
4. Accepted mutations are written to an fsynced, Ed25519-signed, SHA-256-chained
   append-only audit before becoming visible in memory. Configurations are
   content-addressed immutable files. Startup verifies every signature, chain
   link, sequence, replay ID, and referenced active artifact; corruption makes
   the service unready.
5. Rollback can select only an older, still-valid snapshot with the same
   configuration identity and environment. It uses compare-and-swap against
   the expected active hash and requires a rollback operator plus a distinct
   approver. It does not edit or resign the target.
6. Emergency control is engage-only, scoped to firm, account, venue, strategy,
   or symbol, authority-epoch fenced, strictly sequenced, signed, and durably
   journaled. Clearing an emergency inhibit is deliberately excluded; recovery
   uses the reviewed configuration workflow.
7. An edge process verifies and atomically swaps a prebuilt local snapshot. Its
   read operation does no RPC, disk I/O, logging, signing, or locking. It binds
   every result to the configuration hash and fails closed for missing,
   malformed, not-yet-valid, expired, offline-old, clock-regressed, unauthorized,
   session-closed, model-ineligible, or killed state. Reinstalling identical
   bytes cannot refresh offline age.
8. The ordinary build recognizes `LIVE` only to reject it. Only `SIMULATION`
   and `PAPER` venue modes validate; this component has no exchange transport
   capability.

## Consequences

- Control-plane availability is not required for edge reads, but loss of fresh
  signed state eventually blocks new orders at the smaller of the global edge
  offline bound and account risk-configuration age.
- Wall UTC enforces signed validity; process monotonic time enforces local age.
  A backwards observation on either axis fails closed.
- API callers must obtain `AuthContext` from an approved identity provider or
  mutually authenticated service boundary. The repository CLI is an
  operator/CI adapter protected by OS key-file access, not that identity
  provider.
- Ed25519 software key files validate the workflow and are not production key
  custody. HSM/KMS integration, multi-host consensus/fencing, and authenticated
  network transport remain deployment dependencies.
- An online edge rejects lower revisions. Applying an authorized rollback to a
  running edge requires a future signed selection-envelope distributor; until
  then operations must safely stop/restart the edge from authority-selected
  local state. This limitation cannot enable trading and is documented in the
  runbook.

## Rejected alternatives

- Per-field mutable configuration was rejected because partial activation
  cannot reproduce one decision identity.
- Automatic activation after approval was rejected because an operator must
  explicitly activate at the staged time.
- A synchronous configuration RPC from risk was rejected because latency,
  outage, and split-brain behavior would enter the order path.
- One-person approval was rejected for changes that can alter risk or trading
  authority.
- Emergency kill reset was rejected because a compromised kill credential
  must only move the system toward safety.
