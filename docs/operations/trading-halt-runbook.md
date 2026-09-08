# Trading halt and reopening runbook

## Detection and authority

Official venue status is authoritative and overrides inferred state. An inferred
halt or missing/contradictory status is unsafe until confirmed; it cannot be
treated as open. Preserve venue, instrument, session, source sequence, exchange
event time, NIC receive time, process monotonic time, status version, and content
hash.

## Immediate response

1. Inhibit new and replace intents for the affected symbol/venue. If scope or
   status is uncertain, engage the broader venue or firm kill.
2. Require authoritative market state `HALTED`; verify risk and the final gateway
   independently reject fresh orders.
3. Freeze strategy participation and invalidate forecasts whose state/horizon is
   incompatible with the halt.
4. Preserve outstanding-order, acknowledgement, fill, auction, book, feed, clock,
   risk, gateway, and journal evidence. Silence is not a cancel acknowledgement.
5. In PAPER, only exercise the explicitly modeled local cancel behavior. For a
   real venue, cancel/flatten and session semantics remain prohibited until the
   licensed procedure is approved.

## Reopening

1. Validate official reopening/auction status and sequence continuity. Resolve
   gaps through the market-data recovery runbook.
2. Rebuild the book from an authorized snapshot, then validate auction imbalance,
   indicative values, uncrossed transition rules, and final clearing events.
3. Reconcile all orders, executions, positions, P&L, and risk reservations.
4. Transition `HALTED` → `REOPENING` → `RECOVERY`; a direct transition to
   `NORMAL` is forbidden.
5. Require fresh clock, feed/book, configuration, risk, model, fencing, and
   journal state for the full stabilization period.
6. Reset only the scoped kill with explicit authorization. Resume PAPER at
   restricted participation and verify audit correlations before normal PAPER
   limits are reconsidered.

## Exit criteria

Official status is open; recovery sequences and book hashes agree; no order or
fill is unknown; positions/risk reconcile; all mandatory state is fresh; the
stabilization timer completed; and the complete transition/audit chain verifies.
Reopening never creates live authority.
