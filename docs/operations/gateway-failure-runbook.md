# Gateway failure runbook

## Immediate response

1. Inhibit new order and replace commands at risk, OMS consumption, and the final
   gateway gate. Engage venue or firm kill when state is ambiguous.
2. Record mode, build/configuration hashes, session/process epochs, fencing token,
   heartbeat and sequence values, command/audit sequence, queue occupancy, last
   acknowledged order, and disconnect/reject reason.
3. Mark every command whose acknowledgement is uncertain as
   `UNKNOWN_RECOVERY`. Do not retry it, allocate the same client order ID, or infer
   cancellation from disconnect.
4. Preserve gateway and OMS journals, response buffers, paper-simulator state,
   drop-copy evidence, and network diagnostics. Never edit an original journal.

## Diagnose

Classify heartbeat timeout, transport disconnect, logon/session rejection,
sequence mismatch, reject burst, rate limit, audit exhaustion, stale fencing,
split brain, malformed response, or local PAPER simulator failure. If clock,
feed/book, risk, journal, or leader state is also unsafe, follow that higher-
priority runbook before gateway recovery.

## PAPER recovery

1. Fence the failed process and allocate new process and exchange-session epochs.
2. Verify gateway audit chain and rebuild OMS from the authoritative journal.
3. Obtain a complete synthetic/PAPER recovery snapshot and require account,
   session, order ID, sequence, quantity, and hash agreement.
4. Reconcile every unknown command and logical fill, including duplicates,
   partial fills, cancel races, corrections, and busts.
5. Rebuild positions/P&L and install a fresh risk snapshot. Require zero unknown
   orders and no unmatched required fills.
6. Start a new PAPER session under the current fencing token. Verify heartbeat,
   rate limits, audit capacity, mode labels, and a no-order probe.
7. Clear the kill only through authorized recovery. Recovery does not restore a
   previous session or any live authorization.

## Licensed boundary

Real logon, reconnect, reset, resend, cancel-on-disconnect, open-order, drop-copy,
and takeover semantics require licensed specifications and venue certification.
Until then, stop after evidence preservation and escalation; do not extrapolate
the synthetic protocol.
