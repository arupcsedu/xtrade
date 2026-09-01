# Market-state controller failure runbook

## Safety posture

Only a fresh authoritative `NORMAL` snapshot may report that new orders are
permitted, and that report does not replace deterministic pre-trade risk. Empty,
busy, corrupt, stale, incompatible, journal-rejected, or not-initialized state
must block new orders. Never reconstruct state from model outputs or bypass the
controller during an incident.

## Detection

Alert on journal rejection, publisher corruption/busy counts, input ordering
failure, malformed input, repeated overdue release state, abnormal dwell hold,
or lack of a fresh snapshot. Also alert when producer state and the transition
reason disagree, such as official halt without `HALTED`.

## Immediate response

1. Engage the independent kill switch and inhibit new order transmission.
2. Preserve controller configuration hash, build identity, process epoch,
   transition/input sequences, snapshot/record hashes, timestamps, state,
   reasons, and journal handoff status.
3. Check official status first, then clock, feed/book, kill delivery,
   recovery/reopening state, calendar/news state, and predictive thresholds in
   the documented priority order.
4. Do not clear journal files, reset sequences, skip recovery, or force
   `NORMAL`. Do not copy licensed payloads into logs or incident tickets.

## Journal or publication failure

A full/stopped journal handoff prevents publication, invalidates snapshot reads,
and returns `JOURNAL_REJECTED`. Keep trading inhibited, restore bounded journal
capacity, and retry the same ordered input. An atomic publisher failure indicates
violated single-writer ownership or memory corruption; the controller becomes not
initialized and requires a quiescent process restart and process-epoch fencing.

## Recovery

Recovery requires a valid effective configuration, healthy mandatory journal
handoff, one authoritative writer, fresh ordered inputs, healthy clock/feed/book,
no halt or kill, and the complete configured reopening/recovery stabilization
period. Replay transition hashes and verify no forbidden edge occurred before
restoring readiness.

`HALTED` or `DATA_DEGRADED` never recovers directly to `NORMAL`. Operator
recovery requests can demand recovery but cannot force a normal state or shorten
stabilization.
