# Macroeconomic release failure runbook

## Safety posture

The macro specialist is advisory and has no order-entry capability. Any missing,
late, partial, conflicting, unauthenticated, low-quality, or temporally invalid
input fails closed. Operators must not manufacture a consensus, mark a partial
release complete, backdate availability, discard a correction, or substitute a
neutral forecast for abstention.

## Detection

Alert on bounded-cardinality counts by event type, phase, decision, and reason:

- scheduled event passed without official receipt;
- partial release still unresolved;
- missing or incompatible required field/consensus;
- provider value conflict;
- consensus hash changed after freeze;
- stale or broken correction lineage;
- source authentication or quality failure;
- timestamp-order rejection;
- missing/invalid cross-asset group; and
- replay digest mismatch.

Payload values, source excerpts, licensed content, field IDs with unbounded
cardinality, and document identifiers must not be metric labels or logs.

## Response

1. Confirm the event ID, calendar version, scheduled UTC time, frozen consensus
   digest, release revision, and configuration hash.
2. Keep macro publication disabled for that event. Do not bypass the specialist
   or construct forecast bytes manually.
3. Verify the official source authentication and compare its publication and
   receipt timestamps. The receipt time is the availability boundary.
4. For a partial release, wait for an authenticated append-only revision. Do not
   merge unrelated provider fields into the official release.
5. For a conflict, preserve every observation and excerpt. Resolve through an
   approved official correction; never silently choose the value that matches a
   preferred market direction.
6. For consensus mutation, restore the original content-addressed pre-release
   snapshot. A post-release estimate is never eligible for that event.
7. For a correction, require increasing revision, the prior release digest, and
   the same frozen consensus digest.
8. Rebuild cross-asset features only from point-in-time market events whose
   response windows straddle the receipt and end before snapshot availability.
9. Replay the exact normalized input and require the recorded detailed-result
   digest before restoring publication.

## Recovery criteria

- calendar, consensus, source, release, and feature identities resolve;
- source authentication and configured quality floor pass;
- all required official fields and exact frozen consensus values are present;
- no provider conflicts remain without an official correction;
- timestamps are ordered and available by the recorded cutoff;
- every required cross-asset group is valid and fresh;
- correction lineage and consensus freeze are unchanged; and
- deterministic replay matches the retained result and common forecast bytes.

Recovery authorizes only advisory forecast publication. It does not authorize a
strategy, risk approval, live mode, or order transmission.

## Escalation and retained evidence

Escalate unresolved source conflicts, suspicious timestamp changes, calendar
changes near release, or correction lineage failures to data operations and
compliance. Retain normalized source metadata/excerpts, calendar and consensus
digests, release revisions, response snapshot ID, configuration/model versions,
abstention reasons, and replay digest. Licensed payload retention must follow
the provider agreement and repository secret/data policy.
