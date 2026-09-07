# Observability Alert Runbook

## Universal first actions

1. Confirm the alert timestamp, environment, build version, configuration hash,
   process epoch, and affected service health/readiness.
2. Check whether telemetry itself is dropping. Missing telemetry is unknown
   state, never proof that trading is healthy.
3. Preserve journal segments, structured logs, active configuration, slot
   inventory, and the correlated decision explanation. Do not edit originals.
4. If clock, feed, book, risk, journal, split-brain, or kill state is unsafe,
   block new orders through the existing safety control. Observability does not
   override or clear safety state.
5. Escalate using the environment incident policy. Never enable live transport
   to diagnose a monitoring problem.

## Feed gap or stale feed

Alerts: `AegisMarketDataGap`, `AegisFeedStale`, `AegisBookInvalid`.

- Compare A/B leg sequence state and packet/NIC errors.
- Confirm the feed handler publicly reports recovery or invalid state; no gap
  may be hidden as healthy.
- Follow the [market-data recovery runbook](market-data-recovery-runbook.md).
- Resume eligibility only after snapshot/retransmission recovery and configured
  stabilization prove a valid non-stale book.

## Clock unsafe

Alert: `AegisClockOffsetUnsafe`.

- Confirm source identity, PTP offset/drift, hardware timestamps, sync age, and
  clock-quality state.
- Follow the [clock failure runbook](clock-failure-runbook.md).
- A wall-clock correction does not by itself establish recovery; wait for the
  configured monotonic stabilization interval.

## Telemetry or queue loss

Alerts: `AegisTelemetryDrop`, `AegisQueueDrop`, `AegisRingNearCapacity`.

- Identify the bounded queue and compare producer/consumer sequence and high
  watermark. Confirm the producer remains nonblocking.
- Reduce off-path export volume, restore the consumer, or increase capacity only
  after replay/benchmark evidence. Do not add retries to the producer.
- If mandatory risk/order audit records are affected, the journal owner must
  fail closed independently. Operational metric loss alone cannot authorize or
  reject an order.

## Model health or disagreement

Alerts: `AegisModelDeadlineMisses`, `AegisModelOodHigh`,
`AegisModelCalibrationLow`, `AegisModelDisagreementHigh`.

- Resolve each slot against the active signed slot inventory and configuration
  version.
- Inspect forecast age, exact model/version, feature provenance, deadline,
  calibration and OOD in the correlated decision explanation.
- Disable or rollback through the governed model workflow. Hard ensemble masks
  remain authoritative; never change weights from a dashboard.
- Follow the [model deployment rollback runbook](model-deployment-rollback-runbook.md)
  for shadow/canary models.

## Risk, venue, or execution degradation

Alerts: `AegisRiskRejectBurst`, `AegisVenueRejectBurst`,
`AegisExecutionLatencyHigh`, `AegisDrawdownHigh`.

- Separate expected safety rejects from malformed/unavailable-state rejects by
  the exact risk reason and failed check in decision explanations.
- Check market, clock, feed/book, kill, position, limits, and configuration age
  before examining model output.
- Correlate route venue/reason, gateway session state, acknowledgements/fills,
  slippage and adverse selection. Do not reroute around an ineligible venue or
  bypass fresh pre-trade risk.
- Engage the narrowest sufficient kill switch when exposure or loss state is
  unsafe; escalation may engage firm-wide kill. Clearing requires the governed
  recovery workflow.

## Journal lag

Alert: `AegisJournalLag`.

- Inspect disk/NVMe health, free space, async queue occupancy, sync latency, and
  writer error counters.
- Preserve the original and follow the
  [journal recovery runbook](journal-recovery-runbook.md).
- Never repair in place. A mandatory audit backlog that exceeds configured
  capacity blocks new orders through the owning component.

## Evidence closure

Record alert start/end, operator actions, hashes for journal/configuration/model
artifacts, affected correlations, recovery checks, and approval identities.
Attach dashboard exports only as derived evidence; the immutable journals and
signed artifacts are authoritative.
