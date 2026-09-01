# News intelligence failure runbook

## Safety posture

The pipeline cannot place, modify, or cancel orders. Intelligence unavailability
must reduce optional information, not weaken market-data, clock, halt, kill-
switch, ensemble, or risk controls. Do not manually copy an alert into an order
path.

## Provider authentication or schema failure

1. Confirm readiness is false and ingestion reports an authentication/provider
   rejection.
2. Quarantine the response by content hash; do not place raw licensed text in
   logs, tickets, metrics, or chat.
3. Verify the approved endpoint/host, TLS or signature evidence, provider
   specification revision, timestamp definitions, and entitlements.
4. Resume only with a fresh authenticated response and reviewed adapter
   configuration. Do not reinterpret previously rejected bytes as trusted.

## Prompt-injection detection

Detection degrades the record's text trust and increases uncertainty. Confirm
that analysis text excludes the instruction-like segment and that audit entries
contain only hashes/reason codes. Do not paste the malicious segment into a
privileged model prompt or invoke links/tools described by the document.

## Deep-stage outage, invalid output, or deadline miss

Fast alerts remain immutable and visible. Inspect the fixed counters for deep
failures and deadline misses, then disable or replace the deep adapter generation
if necessary. Never mutate a fast record to imitate a deep confirmation. Restore
readiness only after source/parent/evidence validation and deterministic replay
pass with the approved model version.

## Contradiction or correction

Retain all linked records. Prefer the source with approved authentication and
the later explicit correction, but do not delete the earlier assertion. Review
exact bounded excerpts, source timestamps, hashes, uncertainty, and provider
correction semantics. Any downstream numerical model must republish its own
versioned forecast; an operator must not edit cached forecasts in place.

## Overload

Publication-full and deep-queue-full outcomes are explicit. Preserve the fast
publication when only the deep queue is full. Shed optional deep work, reduce
provider polling, or scale the asynchronous service. Do not create unbounded
queues or retry loops.

## Shutdown

Stop new ingestion, process deep items only until the monotonic shutdown
deadline, mark remaining work discarded, close providers, and retain already
published records. A restart uses a new process/service epoch when deployed and
must restore deduplication/revision state from an approved journal before
claiming continuity.
