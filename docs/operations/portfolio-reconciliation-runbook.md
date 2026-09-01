# Portfolio reconciliation failure runbook

This runbook covers `RECONCILING` or `UNSAFE` portfolio-risk state. It does not
authorize trading. New orders remain blocked through stale/unavailable
pre-trade state while reconciliation is incomplete.

1. Confirm the portfolio health, readiness, configuration hash, session ID,
   snapshot sequence/hash, source journal sequence, and last journal record
   hash. Preserve these values in the incident record.
2. Engage the narrowest applicable kill switch. Escalate to account or
   firm-wide scope if affected symbols or strategies cannot be bounded.
3. Stop new event admission only through graceful component controls. Preserve
   the journal; do not truncate, rewrite, or skip a conflicting record.
4. Classify the reason: missing mark, unmatched primary/drop-copy fill, orphan
   fill, conflicting economics, journal exhaustion, arithmetic failure, or
   unsupported post-corporate-action correction.
5. Compare broker/venue drop-copy records only through an authorized adapter.
   Verify session, account, order, logical execution identity, price ticks,
   quantity units, fees, source timestamps, correction lineage, and bust state.
6. Reconstruct a fresh service from the verified journal. Require identical
   snapshot and journal hashes. A divergence is an incident, not a candidate for
   manual total adjustment.
7. For corporate actions or fractional entitlements, obtain the authorized
   reference record and broker accounting policy. Do not infer a rounding rule.
8. Restore marks and complete source reconciliation. Confirm `HEALTHY`, ready,
   zero orphan count, required zero unmatched counts, stable clock/data state,
   and independent pre-trade snapshot installation.
9. Kill-switch reset requires the existing authorized risk procedure. Retain
   before/after snapshots, configuration, journal chain, operator identity, and
   incident approval.

Never repair an incident by fabricating a fill, changing a hash, deleting a
record, weakening a limit, or enabling a gateway.

