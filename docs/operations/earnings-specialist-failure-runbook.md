# Earnings specialist failure runbook

## Safety posture

The specialist is advisory and off-path. Failure, staleness, uncertainty, or
shutdown means no new earnings forecast is available. Operators must never
route detailed earnings results directly to OMS or treat pipeline recovery as
trading authorization.

## Immediate actions by reason

| Condition | Required behavior | Investigation evidence |
| --- | --- | --- |
| Future estimate/option/source detected | Reject the normalized bundle; do not publish | availability cutoff, official publication time, source event IDs and hashes |
| Missing or incompatible consensus | Publish only if the remaining bundle is valid; retain null surprise and reason | metric identities, basis, unit, period, segment |
| Invalid feature snapshot | Reject evaluation | snapshot ID, validity/freshness state, as-of times |
| Correction | Append a linked revision and re-enter `RELEASE_PROCESSING` | prior/current input hashes, revision, source event IDs |
| Replay digest mismatch | Mark replay invalid and preserve both digests | input bytes/hash, model/config version, expected/actual result hash |
| Hook rejection or lifecycle mismatch | Do not commit the transition | transition proposal, current phase, event ID, revision |
| Forecast expired | Remove/ignore it through the common forecast cache policy | production/expiration monotonic times |
| Capacity exhausted | Reject new transition or input explicitly; do not retry synchronously | capacity and rejection counter |

## Recovery

1. Verify the official source identity and exact upstream event record.
2. Reconstruct the normalized input using only artifacts available at its
   recorded cutoff.
3. Validate accounting basis, units, fiscal period, segment identity, evidence
   references, revision lineage, feature validity, and model/configuration hash.
4. Replay with the same code and compare the detailed result and common forecast
   bytes.
5. Resume publication only when replay matches. Lifecycle recovery does not
   authorize order transmission.

## Escalation boundaries

Provider timestamp semantics, estimate entitlements, correction identifiers,
transcript retention, option-data licensing, and redistribution require legal
and provider approval. Do not infer or fabricate these details.
