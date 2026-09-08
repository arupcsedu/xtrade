# ADR 0044: Final non-live operational-readiness package

- Status: Accepted
- Date: 2026-09-08

## Context

Aegis-MX has component-specific architecture, deployment, incident, recovery,
and validation documents, but an operator should not need to infer the governing
procedure during a fault. The final package must provide one indexed entry point,
make responsibility boundaries explicit, and prove that its drill cannot activate
or reach a production gateway.

Several production blockers remain open. Packaging existing evidence must not be
misrepresented as production approval, licensed-integration certification, or a
live-activation procedure.

## Decision

The repository owns one
[operational-readiness package](../operations/operational-readiness-package.md)
that maps each required checklist and runbook to a single authoritative document.
It includes architecture and data-flow diagrams, role-based component ownership,
startup/shutdown and daily PAPER checklists, incident routing, licensed and
regulatory review gates, and a production-activation review checklist whose
current terminal state is `PROHIBITED`.

An executable C++ operator drill calls the production deterministic pre-trade
risk engine directly. It exercises symbol, strategy, venue, and firm kill scopes,
proves that each blocks a fresh PAPER intent, proves that an unauthorized clear is
rejected, applies an authorized clear, reevaluates a fresh intent, and drains the
bounded risk-decision journal into a SHA-256-linked NDJSON extract. The drill has
no OMS, gateway, network, credential, or activation API. It refuses a live-capable
build. Recovery in the drill validates kill clearing and fresh risk evaluation;
external order/position reconciliation remains an operational prerequisite and
is not simulated as completed.

A separate evidence generator binds the configured CMake option, build metadata,
all six edge profiles, paper-gateway systemd inhibit guard, control-plane LIVE
rejection, operator-drill evidence, and the still-open production blockers. A
passing result means live mode is disabled; it deliberately reports
`production_ready=false` and `activation_status=PROHIBITED`.

## Consequences

Operators receive a concise entry point while detailed procedures retain their
component owners. Automated tests detect broken links, malformed Mermaid,
operator-audit tampering, missing risk decisions, a reachable gateway boundary,
or weakening of non-live constants.

The package is usable for synthetic/PAPER training and incident drills. It does
not close target-site, licensed-integration, physical fencing, authoritative
journal/reconciliation, or regional-platform blockers. Named people, paging
routes, regulatory determinations, recovery objectives, venue contacts, and
external credentials remain site-owned controlled records.
