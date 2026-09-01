# Market-specialists failure runbook

## Safety posture

These specialists are advisory and have no order-entry connection. On unknown,
stale, malformed, unauthenticated, disabled, or low-quality state, stop forecast
publication and preserve the content-addressed abstention. Never reuse an
expired forecast or convert missing evidence into a zero-valued fact.

## Detection

Alert on bounded-cardinality counts of `SYMBOL_DISABLED`, `MISSING_PROVENANCE`,
`LOW_DATA_QUALITY`, `UNAUTHENTICATED_SOURCE`, `STALE_INPUT`,
`INSUFFICIENT_HISTORY`, and `INSUFFICIENT_LIQUIDITY_EVIDENCE`. Also alert on
replay-digest mismatch, schema rejection, abnormal abstention rate, forecast
expiry, or an input-source health transition.

## Immediate response

1. Disable the affected instrument through a new versioned symbol-policy
   snapshot. If the scope is uncertain, disable the complete affected source
   universe.
2. Stop publishing new forecasts from the affected specialist. Do not alter
   ensemble, risk, market-data, clock, or kill-switch safeguards.
3. Capture event IDs, input/result hashes, model/configuration versions, policy
   hash, timestamps, data-quality scores, reason codes, and host build identity.
   Do not copy licensed payloads into tickets or logs.
4. Quarantine suspect provider input and determine whether the failure is
   freshness, authentication, entitlement, schema/version, point-in-time, or
   model-calibration related.

## Recovery

Recovery requires fresh authenticated provenance, a current effective symbol
policy, acceptable data quality, valid timestamp ordering, sufficient
history/evidence, and deterministic replay that reproduces the candidate result
digest. Corrections create new immutable source and result identities; they do
not overwrite prior records.

Re-enable one instrument with an explicit new policy version, observe bounded
publication and abstention metrics, then expand scope. A process restart alone
is not recovery evidence.

## Escalation

Escalate provider semantics, licensing, or entitlement uncertainty to data
governance and compliance. Escalate venue auction/allocation ambiguity to the
market-data integration owner. Escalate unexplained replay mismatch or symbol-
policy inconsistency to platform and risk owners. The system remains abstaining
until accountable owners accept the evidence.
