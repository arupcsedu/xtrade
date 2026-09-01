# ADR 0028: Offline deterministic replay orchestration

- Status: Accepted
- Date: 2026-08-31

## Context

Aegis-MX needs packet and canonical-event replay that is reproducible, can inject
faults, and can either retain recorded model results or invoke deliberately
selected model versions. Replay must exercise production component interfaces
without becoming a route to an exchange gateway or silently presenting a
counterfactual run as a faithful reconstruction.

## Decision

`cpp/replay` is an offline orchestration boundary. It depends on the verified
append-only journal, the license-clean synthetic packet format, canonical
contracts, and the injectable simulated clock. It does not depend on OMS or
exchange gateway implementations.

Every run first resolves an immutable source and computes a dataset SHA-256.
Selection and fault rules are integer-valued and evaluated in source ordinal
order. Reordering is deterministic, timing is advanced through an injected
pacer, and no replay decision reads the system clock. The ordered manifest
binds the dataset hash, configuration hash, mode, seed, selected model versions,
and build/environment metadata.

Exact mode delivers recorded model outputs byte-for-byte. Recompute mode
requires an explicitly registered recomputation provider and a selected version
for each encountered model. Missing selection or provider is a hard error.
Recomputed results are marked counterfactual and have distinct hashes. Replay
targets publish category hashes for books, features, model requests and outputs,
ensemble decisions, risk decisions, orders, fills, positions, and P&L.

Fault injection never mutates its source. Process-crash, halt/reopen, news, and
macro injections are typed replay-control records. News/macro text is opaque
untrusted data and is never parsed as an instruction.

## Consequences

- Repeated runs over the same manifest have identical deterministic hashes.
- Original-speed pacing may block only the offline replay process; tests use a
  virtual pacer and never sleep.
- A recompute run is explicitly counterfactual and cannot be certified as an
  exact reconstruction.
- Licensed feed decoding remains outside this component until licensed
  specifications and captures are available.

