# Clock failure runbook

## Scope

Use this runbook when the Aegis-MX operational clock state becomes `DEGRADED` or
`UNSAFE`, a wall-clock jump is detected, synchronization becomes stale, the PTP
source changes unexpectedly, or hardware receive timestamps disappear.

This procedure cannot authorize live trading. The independent activation and
revocation rules remain defined by the
[live-trading safety policy](live-trading-safety.md).

## Automatic safety behavior

- `UNKNOWN`, `SYNCING`, and `UNSAFE` expose operation mode `BLOCKED`.
- `UNSAFE` blocks every new order regardless of model, strategy, or operator
  output.
- `DEGRADED` exposes only the configured `REDUCE_ONLY` or `BLOCKED` mode.
- A clock source change, unsafe/degraded sample, monotonic regression, or invalid
  observation clears recovery stabilization.
- Recovery requires continuous healthy samples from one source for the entire
  configured stabilization period. Restarting a process does not bypass it.

If any downstream gateway or risk component disagrees with the clock state,
treat the disagreement as unsafe and engage the independent kill path.

## Initial response

1. Confirm the machine-readable state, reason, operation mode, configuration
   hash, source identity, offset, drift, synchronization age, hardware timestamp
   flag, and transition counters.
2. Verify that new order creation is blocked when state is `UNSAFE`, `SYNCING`,
   or `UNKNOWN`. Do not rely only on an upstream dashboard; verify the final
   transmission gate when it exists.
3. Preserve the relevant audit envelopes, clock snapshots, build identity,
   configuration version, monotonic transition times, and host/PTP service logs.
4. If ordering integrity or host identity is uncertain, invalidate affected
   market data and books. Clock recovery alone does not repair data ordered under
   an unsafe clock.
5. Escalate source disagreement, split brain, or repeated backward steps to the
   platform and risk owners. Do not widen thresholds during an incident.

## Diagnosis

Check, without changing state first:

- whether the configured PTP source identity matches the observed source;
- PTP offset magnitude and drift direction/trend in their explicit units;
- age and monotonic timestamp of the last successful synchronization;
- hardware timestamp availability at the NIC/adapter boundary;
- PTP grandmaster/source election and network path health;
- host suspend, migration, CPU clock instability, process restart, or namespace
  changes that can alter a monotonic origin;
- wall-clock step history and whether it coincides with source failover; and
- whether multiple colocated processes report the same source and state.

Provider-, NIC-, daemon-, and operating-system-specific commands belong in
approved deployment supplements. This repository does not invent those
interfaces.

## Recovery

1. Restore the approved source and hardware timestamp path without changing
   safety thresholds.
2. Confirm fresh synchronization observations are arriving with increasing
   process monotonic times.
3. Observe state enter `SYNCING`; a direct transition from `UNSAFE` or
   `DEGRADED` to `HEALTHY` is a defect.
4. Wait for the configured stabilization interval through observations; do not
   use wall-clock sleeps or manually set the state.
5. Confirm state is `HEALTHY`, operation mode is `NORMAL`, offset/drift/sync age
   remain within healthy bands, the source is unchanged, and hardware timestamps
   satisfy configuration.
6. Revalidate market-data freshness, book integrity, risk health, leadership,
   kill switches, configuration authorization, and every other transmission
   precondition independently.
7. Record the incident outcome and retained evidence. Never infer authority from
   clock recovery alone.

## Failed recovery or recurrence

Keep operation blocked when stabilization restarts, observations regress, the
source changes, thresholds are exceeded, or required evidence is missing. A
third recurrence is not permission to bypass the state machine; escalate the
platform fault and retain the system in its safe mode.

## Required incident evidence

- state/reason transition sequence using process monotonic timestamps;
- source identity and source-change count;
- offset nanoseconds, drift ppb, and synchronization age for each transition;
- hardware timestamp availability;
- effective threshold configuration and configuration hash;
- detected wall-jump residuals and monotonicity failures;
- affected sessions, channels, instruments, books, intents, and orders; and
- operator actions, kill-switch records, and final recovery authorization.
