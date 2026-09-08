# Regulatory-review checklist

This is an engineering evidence checklist, not legal advice or a determination
of applicable law. Designated legal and compliance owners must identify the
jurisdictions, entities, products, venues, accounts, and activities in scope and
maintain the authoritative obligation matrix outside the source repository.

## Scope and governance

- [ ] Record regulated entities, registrations, jurisdictions, account/customer
  types, asset classes, venues, trading hours, and responsible supervisors.
- [ ] Map every applicable market-access, capital/credit, short-sale/locate,
  restricted-list, best-execution, order-handling, recordkeeping, reporting,
  surveillance, privacy, cybersecurity, outsourcing, and business-continuity
  obligation to an owner and evidence artifact.
- [ ] Obtain counsel/compliance approval for licensing, market-data use, news and
  filing sources, model governance, electronic communications, and retention.
- [ ] Define two-person control, segregation of duties, access review, incident
  escalation, exception handling, and periodic recertification.

## Trading controls and conduct

- [ ] Independently validate all 30 pre-trade checks, hierarchical kills, halts,
  clock/feed/book/configuration freshness, position/exposure limits, order/cancel
  rates, collars, tick rules, self-trade prevention, and venue authorization.
- [ ] Review strategies and execution policies for spoofing, layering, wash
  trading, quote stuffing, marking, front-running, manipulation, conflicts, and
  inappropriate use of material nonpublic or restricted information.
- [ ] Validate event, auction, rebalance, hidden-liquidity, and options inference
  controls, including uncertainty labels and prohibited claims.
- [ ] Define venue/account-specific cancel, flatten, rejection, disconnect,
  correction, bust, allocation, and reconciliation procedures from authorized
  rules—not synthetic assumptions.

## Records, models, and evidence

- [ ] Prove every order is reconstructable from source events through features,
  model versions/outputs, ensemble state, configuration, risk, OMS, routing,
  gateway responses, fills, positions, and operator actions.
- [ ] Approve clock sources, timestamp granularity/ordering, immutable retention,
  legal holds, access, export, redaction, and regulator/exam response procedures.
- [ ] Review point-in-time datasets, leakage prevention, model validation,
  calibration/OOD, explainability, change control, shadow/canary limits, rollback,
  disablement, and human approval.
- [ ] Validate required transaction/order/position/reporting outputs against the
  exact applicable specifications and perform reconciled certification.

## Final determination

- [ ] All findings are closed or formally accepted by authorized governance; no
  BLOCKER or CRITICAL item remains.
- [ ] Legal, compliance, risk, security, operations, and accountable executives
  approve the exact build, configuration, integrations, site, limits, and scope.
- [ ] Approval has an effective time, expiry/review time, revocation path, and
  immutable evidence identity.

The current repository cannot complete this checklist because external
jurisdictional determinations, licensed integrations, target-site evidence, and
open production blockers remain absent.
