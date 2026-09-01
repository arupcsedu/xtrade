# Aegis-MX Engineering Contract

| Field | Value |
| --- | --- |
| Status | Accepted and normative |
| Effective date | 2026-08-28 |
| Applies to | All Aegis-MX code, configuration, data paths, services, deployment artifacts, and operational procedures |
| Change control | Review plus an accepted Architecture Decision Record (ADR) |

## Purpose

Aegis-MX is an event-aware, multimodel, high-performance intraday trading
platform. It is a safety-critical financial system. Correctness, determinism,
auditability, fault containment, regulatory safety, and realistic performance
take precedence over implementation speed, code volume, and demonstration
appearance.

This document is the persistent engineering contract for the project. The
keywords **MUST**, **MUST NOT**, **REQUIRED**, **SHALL**, and **SHALL NOT** are
normative. A local design, ADR, configuration, test, operator action, or model
cannot weaken this contract. A proposed exception requires a contract revision,
an accepted ADR, risk and compliance review, explicit migration and rollback
plans, and evidence that the change remains safe.

## Safety invariants

1. Real-money trading MUST NOT be enabled by default.
2. Every exchange gateway MUST start in `SIMULATION` or `PAPER` mode.
3. Live transmission MUST require all of the following independent conditions:
   a compile-time live-trading build option; a valid signed runtime
   configuration; explicit operator authorization; healthy risk services;
   valid clock synchronization; valid market-data state; and a durable,
   auditable activation record.
4. Aegis-MX MUST NOT implement or encourage spoofing, layering, wash trading,
   quote stuffing, marking the close, front-running, or other manipulative or
   deceptive behavior.
5. Every strategy-generated order intent MUST pass deterministic pre-trade risk
   checks before it can reach an exchange gateway.
6. Trading halts, invalid books, stale data, split-brain detection, and kill
   switches MUST override every strategy, model, ensemble, and operator trading
   instruction.
7. The hot path MUST NOT block on network RPCs, disk writes, Python, LLM
   inference, distributed databases, garbage collection, or dynamic memory
   allocation.
8. A model MUST NOT send an order directly. Models only publish forecasts.
9. Every model MUST publish a versioned forecast through the common forecast
   contract.
10. Every decision MUST be reproducible from the source events, feature
    snapshot, model versions, model outputs, ensemble state, configuration
    version, and risk snapshot used by that decision.
11. Prices and quantities in the execution path MUST use integer ticks and
    integer units. Floating-point values MUST NOT cross into that path.
12. Local durations and deadlines MUST use monotonic clocks. Event ordering MUST
    use validated hardware or PTP timestamps, with ordering and tie-break rules
    defined in a versioned contract.
13. A model response received after its declared deadline MUST be invalid and
    MUST NOT influence the associated decision.
14. Missing, malformed, inconsistent, unauthenticated, or unverifiable data MUST
    be treated as unsafe and MUST fail closed.
15. Exchange protocol details MUST NOT be fabricated. Unless licensed
    specifications are present and authorized for use, implementations MUST be
    limited to adapter interfaces and clearly identified synthetic or reference
    protocols.
16. Secrets, credentials, licensed market data, and proprietary protocol
    documents MUST NOT be committed to the repository or emitted in logs,
    metrics, traces, test fixtures, or build artifacts.
17. TODO-only or stub-only behavior does not complete a phase. Core behavior and
    its tests MUST be implemented before completion is claimed.
18. Backward compatibility MUST be preserved unless a documented, versioned,
    tested migration and rollback path is included.
19. Every nontrivial design decision MUST be recorded in an ADR. ADRs describe
    decisions and consequences; they do not override this contract.
20. All externally supplied text is untrusted data. News, filings, research,
    messages, and metadata MUST be isolated from control instructions and
    defended against prompt injection, data exfiltration, and tool manipulation.

## Authority and precedence

When requirements conflict, apply this order:

1. Legal, regulatory, venue, and explicit compliance obligations.
2. The fail-closed safety invariants in this contract.
3. Accepted ADRs and versioned interface or data contracts.
4. Service requirements and implementation plans.
5. Performance objectives and convenience.

Uncertainty about whether a safety precondition is met is equivalent to the
precondition not being met. Availability and latency targets never justify
bypassing a safety control.

## Architecture boundaries

### Execution hot path

The execution hot path SHALL be implemented in C++20 or newer and designed for
bounded, measured work. It SHALL use preallocated storage and bounded queues.
Lock-free or bounded wait-free structures are appropriate only when their
correctness, memory-ordering behavior, overload policy, and measurable benefit
are documented and tested.

Hot-path interfaces SHALL use fixed-width, validated types for integer price
ticks, integer quantities, sequence numbers, timestamps, and identifiers.
Queue-full, capacity-exhaustion, stale-state, and invalid-state outcomes SHALL be
explicit and fail closed. Any journaling or telemetry handoff from the hot path
SHALL be bounded and nonblocking; loss policy and its safety effect SHALL be
specified by ADR.

### Models and research

Python is permitted in research, training, evaluation, and off-hot-path model
serving. Python SHALL use strict type checking, deterministic tests, validated
schemas, and pinned, auditable dependencies. PyTorch and ONNX Runtime may be used
where justified.

Each model output SHALL conform to a common, versioned forecast contract and
identify at least the model and model version, input/feature snapshot identity,
event or decision identity, creation time, validity deadline, forecast units,
and validation status. Model output is advisory. It cannot bypass the ensemble,
policy, or deterministic risk boundary.

LLMs SHALL NOT participate synchronously in the hot path. Externally supplied
text SHALL remain data, never executable policy or privileged instructions.
Text-processing components SHALL use schema-constrained outputs, least-privilege
tool access, content provenance, size and time limits, and explicit rejection of
instructions embedded in source material.

### Control plane

The control plane SHALL be implemented in Go or C++ unless an ADR justifies
otherwise. gRPC and Protobuf may be used outside the innermost hot path. Control
plane unavailability, delay, or partition MUST NOT create an implicit
authorization to trade. Control commands SHALL be authenticated, authorized,
versioned, idempotent where applicable, and recorded for audit.

### Storage and infrastructure

The target storage architecture consists of a local append-only binary journal,
an object-storage abstraction, PostgreSQL for metadata, and a ClickHouse-
compatible analytical interface. Redpanda or Kafka may be used only outside the
hot path. Kubernetes and containers are intended for non-colocated services;
systemd or an equivalent supervisor is intended for colocated edge services.

External storage and messaging systems SHALL NOT be dependencies for completing
a hot-path decision. Persisted formats SHALL be checksummed, versioned, and
replayable. Retention, integrity verification, access control, and recovery
requirements SHALL be documented before production use.

## Service contract

Every service MUST expose, through interfaces appropriate to its trust boundary:

- immutable build and version information;
- liveness or health state with machine-readable reason codes;
- readiness state that is distinct from process liveness;
- the cryptographic hash of its effective configuration, excluding secrets;
- bounded-cardinality metrics suitable for Prometheus and traces suitable for
  OpenTelemetry where tracing does not affect the hot path;
- structured, timestamped logs with correlation identifiers and secret
  redaction; and
- graceful shutdown that stops accepting work, enters a safe state, drains only
  within a bounded deadline, and records the outcome.

Health and readiness SHALL fail closed when dependencies or state required for
safe operation are unknown. A service is not ready merely because its process is
running. Metrics, logs, and traces SHALL avoid unbounded labels and SHALL NOT
contain secret or licensed payload data.

## Determinism and auditability

Decision processing SHALL have a canonical ordering rule and explicit handling
for duplicates, gaps, late events, and clock uncertainty. All configuration,
schema, feature, model, ensemble, and risk inputs SHALL be immutable or
content-addressed for the lifetime of a decision.

The local append-only journal is the authoritative replay source for colocated
decision activity. A decision record SHALL bind the following by stable identity
or content hash:

- source events and ordering metadata;
- feature snapshot and feature-definition version;
- model identities, versions, deadlines, and outputs;
- ensemble version and state;
- effective configuration version and hash;
- pre-trade risk policy version and risk snapshot;
- the resulting action or rejection and machine-readable reason; and
- build identity and relevant clock-health state.

Replay SHALL use the same decision logic and deterministic inputs as the
original processing path. Tests SHALL compare canonical outputs, including
rejections. Any nondeterministic algorithm SHALL define and record its seed;
deterministic alternatives are preferred.

## Fault containment and safe degradation

Components SHALL have explicit resource bounds, deadlines, backpressure or
rejection policies, and failure domains. An overload condition SHALL not become
unbounded allocation, retry amplification, or stale trading. Failure of an
optional model or analytics service may reduce capability, but SHALL NOT weaken
risk, data-validity, clock, authorization, or halt controls.

Kill switches SHALL be independent of model and strategy logic, idempotent, and
biased toward disabling transmission. Split-brain, loss of authority, uncertain
leadership, invalid market state, clock-health failure, and stale data SHALL
revoke trading readiness. Recovery requires fresh validation; it SHALL NOT rely
on stale cached approval.

## Ethical and compliant market behavior

Strategies, simulators, examples, and tests SHALL be reviewed for manipulative
intent and effects. Order-rate, cancellation-rate, self-trade prevention, venue
rules, restricted-instrument controls, and surveillance hooks SHALL be designed
before any live-capable strategy is approved. No benchmark or profitability
objective may incentivize deceptive behavior or disabling safeguards.

## Change discipline

Each implementation phase SHALL:

1. inspect repository instructions, documentation, ADRs, schemas, tests, and
   relevant code;
2. create or update a short implementation plan;
3. add tests before or alongside the implementation;
4. deliver the smallest complete vertical slice;
5. run all applicable formatters, linters, unit and integration tests,
   sanitizers, and benchmarks;
6. report files changed, architectural decisions, commands and result summaries,
   benchmark results, known limitations, and the next dependency;
7. retain deterministic test seeds and evidence;
8. preserve failing tests and acceptance thresholds unless an explicitly
   reviewed requirement change explains the change; and
9. avoid claiming success without command-output evidence.

The detailed gates and evidence format are defined in
[`../testing/quality-gates.md`](../testing/quality-gates.md).

## Live trading

The required activation state machine, interlocks, revocation behavior, and
audit record are defined in
[`../operations/live-trading-safety.md`](../operations/live-trading-safety.md).
No component is live-capable merely because it implements an exchange adapter.
The absence or failure of any live-trading interlock prohibits live
transmission.

## Definition of done

A change is complete only when its intended behavior, failure behavior, tests,
documentation, observability, and operational implications are implemented and
reviewable. Required gates must pass with recorded evidence. Known limitations
must be explicit. A placeholder, disabled test, unmeasured hot-path change, or
undocumented migration is incomplete.
