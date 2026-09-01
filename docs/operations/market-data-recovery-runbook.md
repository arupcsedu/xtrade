# Market-data recovery runbook

This runbook applies to the synthetic feed-handler framework. It defines the
evidence and operator response that future paper or live-capable deployments must
retain. No real venue connectivity exists in this repository.

## Automatic response

1. A canonical sequence gap publishes `GAP_DETECTED` with degraded data quality.
2. Later events are held in the bounded arbitration window and are not normalized.
3. The coordinator requests bounded synthetic retransmission and enters
   `REPLAYING_GAP` when accepted.
4. Replayed events remain degraded until the missing interval is complete.
5. Rejected or timed-out retransmission requests trigger `RECOVERING` and a
   validated snapshot rebuild.
6. Corruption, identity mismatch, conflicting A/B copies, capacity exhaustion,
   publisher rejection, or recovery failure changes the channel to `INVALID`.

`STALE` and `INVALID` are unsafe market-data states. They must block new orders in
the future risk and gateway layers. A model or operator preference cannot override
them.

## Operator checks

- Confirm session, venue, and channel identity against the active signed
  configuration.
- Inspect cumulative missing, malformed, duplicate, out-of-order, overrun, and
  publisher-drop counters; do not infer health only from the current state.
- Confirm both feed legs and the recovery source agree on the recovered sequence
  and event hash.
- Verify snapshot session, sequence, and stable book hash before acknowledging
  recovery.
- Preserve raw-journal offsets, state transitions, health snapshots, configuration
  hash, build version, and injected monotonic timestamps for incident review.

## Recovery and escalation

The library deliberately has no operator command that changes `INVALID` back to
`HEALTHY`. Stop the handler, diagnose the evidence, construct a fresh instance with
reviewed configuration, and establish state from a validated snapshot. Repeated
gaps, any A/B conflict, receiver overrun, or raw-journal/normalized-publisher
backpressure requires escalation to market-data and risk owners before service is
restored.

Licensed venue escalation contacts, replay endpoints, and sequence reset rules
cannot be added until provider specifications and operational agreements are
available.
