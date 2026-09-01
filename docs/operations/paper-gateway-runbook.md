# Synthetic and paper gateway runbook

## Identification

Confirm build identity, configuration hash, mode, health, readiness, session
state, exchange-session epoch, fencing token, and last audit sequence. Every
record and metric must say `SIMULATION` or `PAPER`. Any missing or conflicting
mode is unsafe.

`PAPER` means the isolated in-process model described in the
[gateway architecture](../architecture/exchange-gateway-framework.md); it does
not mean a broker connection.

## Start

1. Verify the default-off build manifest and absence of endpoints, credentials,
   routes, and concrete native/FIX adapters.
2. Load a valid simulation or paper configuration with explicit synthetic
   instrument mappings and fixed capacities.
3. Confirm risk, clock, market-state, feed/book, kill, audit, and fencing inputs
   are current.
4. Start the local session and verify `ACTIVE`, healthy, and ready.
5. Run a no-order heartbeat and sequence check before submitting a fixture.

## Inhibit conditions

Immediately stop new/replace submission on sequence gap, heartbeat timeout,
stale fencing, command identity conflict, audit exhaustion, invalid risk
evidence, unsafe clock/data/book/market state, halt, kill, capacity exhaustion,
or ambiguous recovery. Do not reinterpret a rejected predicate as degraded
permission.

Synthetic/paper cancels may be exercised after market degradation because local
semantics are known. This is not a real-venue recovery policy.

## Recovery

1. Enter explicit recovery; queued local responses are invalidated.
2. Capture the journal sequence/hash and the reason for inhibition.
3. Obtain an explicit normalized local recovery snapshot.
4. Reject configuration, session, account, venue, epoch, sequence, or hash
   mismatch.
5. Keep any `UNKNOWN_RECOVERY` order inhibited. Absence is not cancellation.
6. Apply a complete snapshot and verify session sequence, order quantities,
   external IDs, journal chain, health, and readiness.
7. Resume only simulation/paper. Recovery never creates live authority.

## Shutdown

Inhibit submission, record shutdown, move the local session to stopped, and
preserve audit evidence. Outstanding local paper state is a test artifact; it
must not be confused with a broker position or used for financial reporting.
