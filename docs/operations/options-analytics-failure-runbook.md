# Options analytics failure runbook

## Safety posture

This service is advisory and off the execution hot path. It cannot submit an
order. On ambiguity, it emits a detailed abstention and no `ModelForecast`.
Risk, OMS, and gateways must continue safely without waiting for it.

## Detection

Alert on readiness false, repeated abstentions, numerical failures, stale/wide/
crossed quote increases, invalid surfaces, OOD saturation, missing open
interest, or configuration-hash drift. Metrics have the `options_` prefix and
fixed cardinality. Structured logs contain event type, input SHA-256, and local
ordinal only; they do not contain licensed payloads.

## Triage

1. Record build version, configuration SHA-256, service state, metric snapshot,
   and affected input/result digests.
2. Confirm wall-clock/PTP health and compare quote receipt wall time with the
   bundle as-of cutoff. Do not compare NIC or exchange timestamps directly to a
   wall-clock freshness threshold.
3. Inspect bounded reason codes. Stale, wide, crossed, expired, unsupported
   exercise, quote-bound, surface-arbitrage, and numerical failures are distinct.
4. Verify contract reference version, effective time, expiration, multiplier,
   deliverable, and corporate-action revision. Do not repair an adjusted
   contract by editing a historical revision.
5. Verify underlying, rates, dividends, and source availability were known by
   the input cutoff.
6. Replay the exact bundle and configuration. A digest mismatch is a separate
   determinism incident.

## Response

- Stale/crossed/wide feed: keep publication disabled for affected contracts;
  recover the provider-neutral input upstream and submit a new immutable bundle.
- Unsupported American exercise: remain abstained. Do not enable the European
  solver for it. Use a separately reviewed numerical engine in a future phase.
- Arbitrage-inconsistent quote or surface: quarantine the point set and verify
  timestamps, adjustment state, bid/ask fields, underlying, and carry.
- Numerical failure: retain inputs and solver configuration, reproduce in the
  synthetic harness, and do not widen brackets or tolerances during an incident.
- Corporate-action mismatch: mark the reference chain not ready until an
  authorized source supplies a new increasing revision.
- Unknown dealer side: this is expected under the safe default. Do not relabel
  inferred signed exposure as observed inventory.

## Recovery

Recovery requires a new content-addressed bundle that passes quote, contract,
solver, and surface checks. Confirm two repeated evaluations have identical
result SHA-256, readiness and configuration hash are correct, and publication
metrics resume. Retain failed and recovered digests for audit.

## Escalation

Escalate source semantics, symbology, adjustment, timestamp, entitlement, or
correction disputes to the licensed integration owner and compliance. Escalate
pricing/model changes to research/model risk. Escalate clock issues under the
[clock failure runbook](clock-failure-runbook.md).

