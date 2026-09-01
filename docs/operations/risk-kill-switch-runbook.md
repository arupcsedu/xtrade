# Risk kill-switch runbook

## Scope

This runbook governs the local symbol, strategy, venue, account, and firm kill
switches. It does not activate live trading. A kill blocks new approvals and
cannot be overridden by a model, ensemble, strategy, or operator mode label.

## Engage

1. Identify the narrowest certain scope. If scope or authority is uncertain,
   engage firm-wide.
2. Submit an engaged command with the current authority epoch and a strictly
   increasing command sequence. Engagement does not require the evaluation gate.
3. Confirm the next applicable evaluation is journaled as
   `KILL_SWITCH_ENGAGED`; observe kill generation and bounded command latency.
4. In the later OMS/gateway phases, independently inhibit transmission and
   reconcile outstanding orders. Do not infer cancel acknowledgement.
5. Preserve command identity, actor, source, epoch, sequence, target, timestamps,
   configuration/risk hashes, resulting generation, and decision journal range.

## Diagnose

Verify feed/book and clock state, official halt, position and P&L freshness,
pending reservation reconciliation, configuration revision/hash, authority
epoch, duplicate process ownership, journal capacity, and the triggering alert.
Missing evidence remains unsafe.

## Reset

1. Correct the cause and complete reconciliation.
2. Confirm the process owns the active authority epoch and the target is exact.
3. Obtain explicit reset authorization and a new command sequence.
4. Reset only the intended scope. A reset merely removes that inhibit; it does
   not approve an intent or restore a trading mode.
5. Revalidate all 30 checks. After a limit replacement, reseed positions/P&L,
   mark state available, then reset the automatically engaged firm kill.

Reject stale sequences, epoch mismatch, ambiguous account/firm target indices,
or unauthorized resets. Suspected split brain requires a newer fenced epoch and
full state reconstruction, never an in-place override.
