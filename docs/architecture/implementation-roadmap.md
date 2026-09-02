# Aegis-MX Implementation Roadmap

| Field | Value |
| --- | --- |
| Status | Dependency-aware backlog; implementation progress recorded below |
| Date | 2026-08-29 |
| Current repository state | Phases 1–2 foundations plus temporal, synthetic-feed, and feed-handler vertical slices |
| Governing contract | [Engineering Contract](engineering-contract.md) |
| Target boundaries | [Component Boundaries](component-boundaries.md) |

## Roadmap policy

This roadmap sequences the smallest complete vertical slices needed to make
safety properties executable and testable. A phase number is not permission to
skip its gates. A phase completes only when its acceptance criteria and all
applicable requirements in the
[Quality Gates](../testing/quality-gates.md) pass with recorded command output.

Simulation precedes paper; paper precedes any consideration of live capability.
No phase enables real-money trading by default. Phases 14 and 15 are externally
blocked and cannot be scheduled from repository work alone.

Benchmarks establish correctness-preserving baselines. Numeric thresholds and
regression tolerances require representative hardware evidence and an accepted
ADR; this roadmap does not invent latency claims.

## Dependency summary

| Phase | Primary scope | Depends on | May proceed in parallel with |
| --- | --- | --- | --- |
| 0 | Audit and architecture roadmap | Existing contract baseline | None |
| 1 | Repository and toolchain foundation | Phase 0 | None |
| 2 | Foundational contracts and ADRs | Phase 1 | None |
| 3 | `edge-core`, journal primitives, service shell | Phase 2 | None |
| 4 | Synthetic `market-data` and `order-book` | Phase 3 | Observability adapter groundwork |
| 5 | `model-contracts` validation and `feature-engine` | Phases 2, 4 | Phase 6 after shared contracts stabilize |
| 6 | Deterministic `risk` kernel | Phases 2–4 | Phase 5 |
| 7 | Deterministic `ensemble` | Phase 5 | Risk integration fixtures from Phase 6 |
| 8 | `OMS` state machine | Phases 3, 6, 7 | Replay fixture preparation |
| 9 | Synthetic-only `gateways` | Phases 4, 6, 8 | Intelligence groundwork |
| 10 | Integrated journal and `replay` | Phases 3–9 | None for completion evidence |
| 11 | `control-plane` safety services | Phases 2, 3, 6, 9, 10 | Intelligence/research |
| 12 | `intelligence` and `research` | Phases 2, 5; Phase 10 for faithful replay data | Control-plane |
| 13 | `observability` and `deployment` completion | Phase 3 and each deployable service | Incremental throughout Phases 4–12 |
| 14 | Licensed paper/provider integrations | Phases 9–13 plus authorized specifications | None for each affected adapter |
| 15 | Conditional live-readiness program | Every prior safety phase plus external approvals | Nothing that weakens its gates |

The [dependency graph](dependency-graph.mmd) is the authority for permitted
source dependencies. This table expresses delivery order and integration gates.

## Phase 0 — Repository audit and target architecture

**Inputs**

- Engineering contract, system context, quality gates, live-trading safety
  policy, and ADR-0001.
- Complete visible repository inventory.

**Outputs**

- Repository audit, component boundaries, Mermaid dependency graph,
  implementation roadmap, licensed integration boundaries, and ADR-0002.

**Dependencies**

- Documentation baseline only.

**Acceptance criteria**

- Every current artifact is inventoried and read.
- All requested components have an owner, allowed dependencies, execution class,
  and licensed-completion status.
- The source dependency graph is acyclic.
- Documentation links resolve and Mermaid parses.
- No executable business or trading logic is added.

**Expected tests**

- UTF-8, whitespace, Markdown parse, internal-link, Mermaid syntax, required-
  section, and credential-signature checks.

**Expected benchmark categories**

- Not applicable to a documentation-only phase.

## Phase 1 — Repository and toolchain foundation

**Inputs**

- Phase 0 architecture and quality requirements.
- Approved compiler/platform support matrix and dependency-governance policy.

**Outputs**

- Version-control root, ownership/review rules, ignore/attributes policy, license
  policy, secret scanning, artifact provenance, and CI evidence retention.
- Root layout for all target components without stub business logic.
- Pinned C++20+ CMake presets for development, release, ASan/UBSan, and TSan;
  formatter, `clang-tidy`, unit-test, fuzz, and benchmark harnesses.
- Pinned Python environment with formatter, linter, strict type checker, pytest,
  dependency audit, and deterministic test seed.
- Go workspace/tooling when the control-plane choice is confirmed.
- Reproducible code-generation and software-bill-of-materials hooks.

**Dependencies**

- Phase 0 accepted. Toolchain/package-manager choices require an ADR.

**Acceptance criteria**

- A clean checkout executes the same checked-in commands locally and in CI.
- Empty/smoke targets pass format, lint, unit, sanitizer, and package integrity
  gates without network-dependent resolution during the build.
- Live capability is absent or explicitly default-off in every build preset.
- Dependencies are pinned, checksummed, license-reviewed, and represented in an
  SBOM; no secret or credential is committed.
- CI records tool versions, exact commands, exit status, and artifacts.

**Expected tests**

- Clean-build reproducibility, negative live-option/default checks, dependency
  lock verification, generated-file drift, secret-scanner fixtures, and a
  deterministic seeded smoke test.

**Expected benchmark categories**

- Clean/incremental build time and code-generation time for developer/CI
  capacity planning only; no trading latency claim.

## Phase 2 — Foundational contracts and architectural decisions

**Inputs**

- Engineering invariants, component ownership, service contract, and replay
  requirements.

**Outputs**

- Accepted ADRs for canonical serialization and compatibility; integer tick/
  quantity ranges and overflow/rounding; timestamp domains, ordering and stable
  identifiers; error/reason-code evolution; deterministic seed policy; and
  mandatory-journal capacity/durability/failure behavior.
- Machine-readable, versioned foundational schemas for identities, integer
  price/quantity, event time, monotonic deadlines, source event, feature
  snapshot, forecast, order intent, risk result, OMS/gateway event, health,
  configuration identity, and audit envelope.
- Reproducibly generated C++ and Python bindings; Go bindings only for contracts
  the future control plane uses.
- Golden compatibility corpus and schema ownership/version registry.

**Dependencies**

- Phase 1 reproducible build and code generation.

**Acceptance criteria**

- Every field has units, range, validity, missing/unknown behavior, and ownership.
- Execution prices and quantities cannot be represented as floating point.
- Unknown/incompatible versions and malformed data fail closed.
- Canonical bytes and hashes are identical across repeated supported-language
  runs and defined supported platforms.
- Upgrade, downgrade/rollback, mixed-version, and deprecation policy is tested.
- Protobuf/gRPC types are not used as the innermost hot-path representation
  unless a dedicated ADR and benchmarks justify it.

**Expected tests**

- Golden encode/decode, cross-language conformance, bounds/overflow, malformed
  and truncated payload, unknown enum/version, checksum/hash, ordering/tie-break,
  fuzz/property, ABI/API visibility, and generated-drift tests.

**Expected benchmark categories**

- Hot representation validation, canonical encode/decode, hashing, identifier
  derivation, and cross-language boundary throughput/allocation counts.

## Phase 3 — `edge-core`, audit journal, and service shell

**Implementation status (2026-08-29):** Strong identifiers, checked clocks,
bounded SPSC/MPSC communication, immutable snapshot/RCU symbol state, and the
versioned colocated forecast cache are implemented. The bounded asynchronous,
segmented, checksummed append-only audit journal, recovery scanner, sparse
indexes, copy-only repair, retention boundary, and extraction tools are also
implemented. Preallocated arenas, reusable service shell, OS shared-memory lifecycle,
and deployment capacity qualification remain open; Phase 3 is therefore not
complete.

**Inputs**

- Phase 2 schemas and ADRs; target platform/compiler matrix.

**Outputs**

- C++ foundational value types, checked arithmetic, injected monotonic/event
  clocks, stable status/reason codes, bounded queues, preallocated arenas where
  justified, and immutable configuration/build identity.
- Bounded nonblocking audit enqueue plus append-only local journal writer,
  checksummed segments, recovery scanner, and explicit capacity/failure state.
- Reusable service shell exposing version, health, readiness, configuration hash,
  bounded metrics/log hooks, and bounded graceful shutdown.

**Dependencies**

- Phase 2 accepted contracts, especially journal failure policy.

**Acceptance criteria**

- Hot APIs perform no dynamic allocation or blocking external operation after
  initialization.
- Queue full, time uncertainty, numeric failure, journal backpressure, corrupt
  tail, disk full, and shutdown return explicit safe outcomes.
- Journal recovery never treats an ambiguous/corrupt record as valid.
- Health and readiness are distinct; unknown required state is not ready.
- Public APIs contain no domain strategy, venue, model, or control-plane logic.

**Expected tests**

- Unit/property tests for types and checked arithmetic; fake-clock deadline
  tests; queue wraparound/saturation/memory-order tests; journal truncation,
  corruption, partial-write, disk-full and restart tests; service lifecycle and
  redaction tests; ASan/UBSan/TSan and focused fuzzing.

**Expected benchmark categories**

- Queue enqueue/dequeue under producer/consumer topologies, monotonic clock read,
  checked numeric operations, audit-envelope construction, journal enqueue,
  asynchronous writer throughput, recovery scan, allocations, and tail latency.

## Phase 4 — Synthetic market data and deterministic order book

**Implementation status (2026-08-29):** SMX/1 generation, capture, canonical
normalization, bounded feed handling/recovery, and the bounded single-writer
order-book vertical slice are implemented. Cross-thread immutable publication,
clock-quality integration, replay equivalence, and hardware capacity
qualification remain open; Phase 4 is therefore not complete.

**Inputs**

- Foundational contracts/runtime and a published synthetic/reference feed
  specification owned by the repository.

**Outputs**

- `market-data` adapter interface, synthetic decoder/encoder, validation,
  sequence/gap/duplicate/freshness state, and canonical normalized events.
- `order-book` deterministic state machine, immutable views, status/halt state,
  invariant checks, shadow recovery, and reference fixtures.
- No real provider endpoint, credential, SDK, payload, or inferred protocol.

**Dependencies**

- Phase 3. Production adapter work additionally depends on Phase 14 prerequisites.

**Acceptance criteria**

- Duplicate, gap, reorder, stale, malformed, unknown-instrument, invalid/crossed
  book, halt, recovery, and capacity states produce deterministic fail-closed
  outcomes.
- Equal event times follow the Phase 2 canonical ordering rule.
- Invalid active state cannot become valid without a complete validated recovery.
- The synthetic protocol and fixtures are clearly labeled and license-clean.

**Expected tests**

- Golden feed decode, property/fuzz parser tests, sequence/state-machine tests,
  book invariant and snapshot/recovery tests, clock uncertainty, queue overload,
  replay equivalence, ASan/UBSan/TSan, and deterministic seed recording.

**Expected benchmark categories**

- Packet/message validation and normalization, event ordering, book update/read,
  snapshot swap, recovery catch-up, burst/queue saturation, throughput, tail
  latency, cache/NUMA behavior, and allocation count.

## Phase 5 — Model contracts and online feature engine

**Implementation status (2026-08-29):** The fixed-point incremental feature
engine, bounded publisher, rolling primitives, deterministic replay-parity
harness, common model interface/runners, model/forecast validation, canonical
v1.3 forecast, deterministic baselines, calibration utilities, tests, and smoke
benchmarks, stable native artifact loader, initial seven-model microstructure
suite, and off-path time-series serving boundary are implemented. A production
registry transport, approved external TimesFM runtime/checkpoint, and
model-promotion control service remain open, so Phase 5 is not complete.

**Inputs**

- Validated market/book events, foundational model schemas, and accepted feature
  numeric/parity policy.

**Outputs**

- Strict C++/Python validators for feature snapshots, model manifests,
  forecasts, deadlines, units, validity, and model health.
- Versioned deterministic online feature definitions, bounded state, immutable
  feature snapshots, and an offline parity implementation.
- Deterministic reference forecast publisher using synthetic data; no strategy
  or order authority.

**Dependencies**

- Phases 2 and 4. Feature/model artifact promotion policy requires an ADR.

**Acceptance criteria**

- Every snapshot binds exact source-event range and feature-definition version.
- Every forecast binds model/version, snapshot and decision identity, units,
  creation time, and monotonic deadline.
- Late, malformed, incompatible, non-finite, wrong-unit, or mismatched forecasts
  are invalid and cannot affect a decision.
- Online/offline feature output matches the documented numeric tolerance or
  canonical representation across the supported platform matrix.
- Models have no compile-time or runtime route to risk, OMS, or gateway APIs.

**Expected tests**

- Schema/golden/property tests; late/deadline boundary tests with fake clocks;
  unit/range/non-finite checks; feature reset/gap/stale/capacity tests; online/
  offline parity; model isolation dependency tests; fixed-seed reproducibility.

**Expected benchmark categories**

- Incremental feature update, snapshot publication, forecast validation,
  deadline check, feature-state capacity, offline backfill throughput, and
  allocation/tail-latency profiles.

## Phase 6 — Deterministic pre-trade risk kernel

**Implementation status (2026-08-30):** The local fixed-capacity kernel now
implements the ordered 30-check policy, checked integer projections, immutable
hashed limits, pending reservations, fill/P&L updates, hierarchical kills,
fencing, journal-before-return decisions, v1.6 contracts, deterministic replay
tests, concurrency tests, and latency benchmarks. The administrative
distribution service, exact OMS reservation reconciliation, durable journal
adapter, and organization/venue-approved policies remain open; Phase 6 is not
an integrated trading capability.

**Inputs**

- Foundational order-intent types, validated market/book state, positions and
  exposures from synthetic fixtures, immutable policy/configuration, clock
  health, and kill/halt signals.

**Outputs**

- Independent `risk` kernel with versioned immutable snapshots, checked integer
  arithmetic, stable rejection reasons, approval integrity/scope/deadline
  binding, and health/readiness state.
- Initial synthetic policies for quantity/notional, price collars, position/
  exposure, order/cancel rate, loss, restricted instrument, self-trade
  prevention hooks, stale data, halts, kills, and snapshot validity.

**Dependencies**

- Phases 2–4. Concrete organizational/venue limits require risk/compliance input
  and are not invented.

**Acceptance criteria**

- Every possible intent result is deterministic approval or rejection; unknown
  and arithmetic failure reject.
- Approval is short-lived and bound to exact immutable fields, policy/snapshot,
  configuration, and decision identity; mutation or reuse rejects.
- Kill, halt, stale/invalid market state, unhealthy risk, and clock failure
  override all intents.
- The kernel is callable without RPC, allocation, disk, Python, or distributed
  state on the hot path.

**Expected tests**

- Boundary/overflow/property matrix for every rule; each safety predicate
  independently false; approval mutation/reuse/expiry; snapshot skew; kill/halt
  races; self-trade and rate windows; restart/reconciliation; replay/golden;
  ASan/UBSan/TSan and deterministic seeds.

**Expected benchmark categories**

- Per-intent rule evaluation, rule-count scaling, rate-window update, approval
  construction/validation, kill-read propagation, burst/capacity behavior,
  allocation count, throughput, and p50/p95/p99/tail latency.

## Phase 7 — Deterministic ensemble

**Implementation status (2026-08-30):** The fixed-capacity hard-masked rule,
linear, and quantized learned gates; exact capped PPM weighting; mixture
uncertainty/disagreement; transaction-cost-aware abstention; v1.5 audit
contract; explanation hashes; deterministic tests; and evaluation benchmarks
are implemented. Order-intent translation and Phase 6 risk integration remain
open, so Phase 7 is not complete.

**Inputs**

- Validated on-time forecasts, immutable ensemble state/configuration, stable
  decision identity, and generic order-intent schema.

**Outputs**

- Versioned `ensemble` policy producing a deterministic fixed-point forecast
  and abstention result, followed by a separately reviewed future
  integer-tick/integer-quantity intent translation for risk evaluation.
- Explicit eligibility and degradation policy for missing, late, invalid, or
  unhealthy models.

**Dependencies**

- Phase 5. Integration uses Phase 6 risk fixtures but ensemble never imports the
  risk implementation.

**Acceptance criteria**

- The ensemble never waits for a model and never submits an order.
- Input ordering, tie-breaking, numeric conversion/rounding, model eligibility,
  and state transitions are versioned and replayable.
- Late or mismatched forecasts are recorded but cannot alter the current or
  completed decision.
- Identical canonical inputs produce identical intent/no-action bytes and reason.

**Expected tests**

- Model count/order permutations, missing/late/malformed/incompatible forecasts,
  equal scores/ties, numeric boundaries, state reset, configuration version,
  replay/golden, property tests, dependency isolation, and fixed seeds.

**Expected benchmark categories**

- Forecast validation/combination by model count, deadline checks, state update,
  intent construction, burst/capacity behavior, allocations, and tail latency.

## Phase 8 — OMS lifecycle

**Implementation status (2026-08-30):** A bounded single-writer deterministic
OMS, exact risk binding, strict transition matrix, independent receipt/intent/
execution idempotency, primary/drop-copy fill reconciliation, leader fencing,
deterministic IDs, hash-chained journal persistence, immutable snapshots,
verified restart inhibition, explicit recovery observations, canonical v1.8
`OrderEvent`, independent reference model, property/fuzz tests, and benchmark
targets are implemented. Durable segmented journal writing, a normalized
synthetic gateway recovery-query transport, and Phase 10 end-to-end replay
integration remain open, so the broader phase is not complete.

**Inputs**

- Risk-approved immutable intents, normalized synthetic venue events, stable
  identities, clock state, and journal interface.

**Outputs**

- Deterministic `OMS` state machine, idempotency keys, command/event contracts,
  position event derivation, outstanding-order snapshot, and reconciliation
  interface.

**Dependencies**

- Phases 3, 6, and 7. Venue-specific state/recovery remains blocked by Phase 14.

**Acceptance criteria**

- OMS accepts no intent without a valid exact risk approval.
- Duplicate, late, reordered, impossible, and ambiguous events have explicit
  deterministic outcomes and do not create duplicate commands.
- Restart begins inhibited and reconstructs only from verified journal state;
  reconciliation cannot silently assume venue state.
- Integer price/quantity invariants hold for every transition.

**Expected tests**

- Exhaustive/property state-machine transitions; partial fill/cancel/replace/
  reject; duplicates and reordering; approval expiry/mutation; ID collisions;
  journal restart/corruption; reconciliation ambiguity; queue saturation;
  ASan/UBSan/TSan; replay and fixed seeds.

**Expected benchmark categories**

- State transition by event type, lookup/storage capacity, command generation,
  burst events, outstanding-order snapshot, restart reconstruction, allocations,
  throughput, and tail latency.

## Phase 9 — Synthetic-only gateway and final safety gate

**Implementation status (2026-08-31):** The provider-neutral gateway interface,
session lifecycle, sequence and heartbeat management, final safety predicate,
fixed-window throttles, bounded hash-chained audit journal, synthetic response
decoders, deterministic `SyntheticExchangeGateway`, isolated
`PaperBrokerGateway`, recovery snapshots, certification-style synthetic venue
tests, fuzz target, and benchmark categories are implemented. Deterministic
venue-free execution objectives, all ten bounded execution policies, hard-masked
fixed-point smart routing, route-loop and concentration controls, per-venue
explanations, and integer execution-cost attribution are also implemented and
tested through the paper gateway. The ordinary
build contains no live states, endpoint, transport, credential, or proprietary
protocol implementation. Durable Phase 10 replay integration and any licensed
venue/broker adapter remain open.

**Inputs**

- OMS command/event contract, risk approval and health, market validity, clocks,
  kill/halt state, fencing fixture, mode policy, and synthetic venue spec.

**Outputs**

- Venue-free execution-objective, deterministic policy-slice, smart-routing,
  explanation, and execution-cost-attribution contracts. A routed child always
  requires a fresh exact risk decision and normal OMS transition.
- `gateways` mode state machine and final transmission predicate.
- Deterministic synthetic venue adapter and optional isolated internal paper
  adapter interface; no real endpoint or credential support.
- Build-time live capability default-off and absent from ordinary artifacts;
  mode/config/build identity included in audit records.

**Dependencies**

- Phases 4, 6, and 8 plus ADR-0001. Activation/journal/fencing details require
  dedicated ADRs before live states are implementable.

**Acceptance criteria**

- The delivered binary can be proven incapable of contacting a real venue.
- Gateway starts in `SIMULATION` or explicitly configured isolated `PAPER`; all
  unknown state rejects.
- Every command is revalidated at the final boundary for mode, risk, data/book,
  clock, halt/kill, fencing, capacity, idempotency, and scope.
- Models/intelligence cannot link to or address the gateway.
- Restart and reconciliation never restore live authority.

**Expected tests**

- Independent false case for every final predicate; compile-time/default mode;
  endpoint/credential denylist and network isolation; bypass attempts; duplicate
  commands; session ambiguity; split brain; restart/shutdown; queue saturation;
  synthetic venue faults; final-boundary observation; sanitizers and replay.

**Expected benchmark categories**

- Final predicate evaluation, risk-approval validation, synthetic encode/decode,
  session event handling, burst/rate capacity, queueing, allocation count,
  throughput, and tail latency.

## Phase 10 — Integrated journaling and faithful replay

**Implementation status (2026-08-31):** The authoritative local journal and
offline replay orchestrator now provide verified journal/synthetic-capture
loading, exact artifact replay, an explicit pluggable counterfactual model mode,
deterministic clocks and pacing controls, time/instrument selection, bounded
source loading, deterministic market/process/event fault injection, immutable
manifests, and per-output comparison hashes. An offline event-level backtester
now adds deterministic synthetic/capture evaluation, price-time queueing,
latency and partial-fill simulation, forecast/execution/portfolio metrics,
shadow-order isolation, event-period attribution, and explicit zero-versus-
realistic-cost reports. Component-local adapters for the complete production
pipeline, licensed historical-data normalization, model-artifact resolution,
cross-platform normalization/divergence policy, and the deterministic full-path
golden corpus remain open, so Phase 10 is not complete.

**Inputs**

- Complete synthetic event-to-gateway path, checksummed journals, and exact
  versioned artifacts from Phases 2–9.

**Outputs**

- `replay` verifier/orchestrator, artifact resolver, faithful replay mode,
  separately labeled counterfactual mode, divergence reports, and a deterministic
  end-to-end simulation corpus.
- `backtesting` event-level exchange model, strategy-isolated hypothetical order
  evaluation, execution/forecast/portfolio scoring, and guarded reports that do
  not reduce economic evaluation to raw directional accuracy.

**Dependencies**

- Phases 3–9. Replay links only the synthetic gateway interface and cannot load a
  real transmission plugin.

**Acceptance criteria**

- Every decision and rejection reproduces from source events, feature snapshot,
  models/outputs, ensemble state, configuration, risk snapshot, build, and clock
  evidence.
- Canonical output is byte-identical or matches an accepted explicit
  normalization contract; divergence never passes silently.
- Sub-minute strategies use event-level queue and latency simulation rather than
  bar-close fills; every report discloses fill-model quality, costs, assumptions,
  and uncertainty limitations.
- Corrupt/missing artifacts and journal gaps fail closed with stable reasons.
- Counterfactual output is unambiguously separated and cannot be mistaken for
  faithful audit replay or transmitted.

**Expected tests**

- End-to-end golden replay; repeated/platform matrix; corrupt/truncated/missing/
  duplicate segments; artifact mismatch; late model; rejected intent; restart;
  journal full; fault injection; counterfactual isolation; fixed seed recorded.
- Queue priority, cancellations/executions ahead, partial fill, latency/reject,
  stale order, halt/auction/reopening, shadow isolation, multiple strategy/venue,
  forecast scoring, cost sensitivity, and repeated report-hash tests.

**Expected benchmark categories**

- End-to-end synthetic hot path, journal enqueue/write, faithful replay events
  per second, artifact lookup, recovery scan, divergence detection, allocation,
  queue occupancy, throughput, and latency percentiles.
- Offline generated-event-to-backtest throughput and scenario scaling by order,
  instrument, venue, strategy, latency distribution, and queue-model fidelity.

## Phase 11 — Control plane and safety coordination

**Implementation status (2026-09-02):** A standard-library Go model-registry
vertical slice now provides content-addressed artifacts, signed immutable
provenance manifests, exact feature-schema compatibility, signed hash-chained
approval/deployment audit, strict lifecycle transitions, explicit two-person
production promotion, global disable, compatible rollback, lineage inspection,
CLI/API operations, deterministic tests, race coverage, and control-plane
benchmarks. A paired shadow/canary coordinator adds signed deterministic limits,
identical-snapshot hypothetical comparison, exact regime-specific confidence
gates, explicit second-person canary approval, bounded symbol/strategy/capital/
rate/risk-freshness scope, all ten automatic rollback triggers, fail-closed
disable, restart recovery, Prometheus metrics, dashboard, and runbook. It is local
and off hot path. A general configuration slice now adds canonical signed
immutable full snapshots, active/bootstrap RBAC,
persistent anti-replay authorization, two-person critical approval, staged
activation, compatible rollback, hierarchical engage-only emergency kills,
signed hash-chained audit, and a lock-free-read local edge cache that expires
closed without RPC. Simulation and paper are the only accepted venue modes.
Production identity/HSM adapters, authenticated network transport, multi-host
consensus/fencing, fleet status, and signed online rollback distribution remain
open, so Phase 11 is not complete. See the
[configuration control-plane design](configuration-control-plane.md) and
[ADR 0033](../adr/0033-signed-two-person-configuration-control.md).

**Inputs**

- Versioned control/configuration/health contracts, service shell, risk and
  gateway state machines, journal/replay evidence, and approved identity/signing
  architecture.

**Outputs**

- Go or C++ `control-plane` service for authenticated/authorized operator
  actions, signed configuration validation/distribution, scoped expiring
  authorization, kill/inhibit delivery, desired-state audit, and fleet status.
- Test-only signing identities stored outside production paths; no real
  credentials in the repository.

**Dependencies**

- Phases 2, 3, 6, 9, and 10; ADRs for trust roots, anti-replay, authorization,
  fencing, partition/lease behavior, and audit durability.

**Acceptance criteria**

- Commands are authenticated, authorized, scoped, versioned, replay-protected,
  idempotent where required, expiring, and audited.
- A partition, delay, restart, signer failure, or unknown state cannot grant or
  extend trading readiness.
- Local gateway/risk gates remain authoritative and operational when the control
  plane is unavailable.
- Service contract and bounded graceful shutdown are complete.

**Expected tests**

- Signature/config schema, wrong signer/scope/environment/build, expiry/replay,
  RBAC, idempotency, partition/lease, clock skew, revocation, split brain, kill
  delivery, restart, audit corruption, API fuzzing, race tests, and integration
  with simulation only.

**Expected benchmark categories**

- Signature/config verification, authorization throughput, kill/inhibit
  propagation distribution, reconnect storms, status fan-in, audit write rate,
  resource/capacity saturation, and shutdown time. None are hot-path latency
  substitutes.

## Phase 12 — Intelligence and reproducible research

**Implementation status (2026-08-30):** The provider-neutral mock, filesystem
replay, and injected official-public boundaries; strict untrusted-text
sanitization; registry entity resolution; deterministic fast classification;
bounded deep adjudication; immutable corrections/contradictions; v1.4 canonical
publication; lifecycle/metrics; an integer-only, leakage-safe earnings
specialist with common forecast publication and phase hooks; a frozen-consensus
macroeconomic calendar/release specialist with strict abstention, linked
  corrections, cross-asset response provenance, and common forecast publication;
  a provider-neutral options analytics signal service with strict public OSI
  parsing, fixed-point quote/reference contracts, bounded European BSM IV and
  Greeks, static-arbitrage surface checks, observed options features,
  assumption-labelled dealer pressure, deterministic replay, and common
  volatility forecast publication;
  provenance-bound index-rebalance, auction, and hidden-liquidity specialists
  with integer ranges, explicit assumptions, symbol disablement, data-quality
  abstention, synthetic closing-auction fixtures, deterministic replay, and
  common return-forecast publication;
  an offline bitemporal point-in-time store covering corporate actions,
  symbology, delistings, index membership, analyst/macro vintages, news
  corrections, and filing amendments, with fail-closed leakage validation for
  future data, survivorship, label overlap, and randomized time-series splits;
and adversarial replay tests are implemented.
Production provider connectivity and a reviewed external deep-model runtime
remain blocked. The local signed artifact-promotion slice is implemented, but
distributed identity/HSM-backed promotion and production research governance
remain open, so Phase 12 is not complete.

**Inputs**

- Feature/model contracts, synthetic or properly licensed datasets/providers,
  artifact/provenance policy, untrusted-text threat model, and replay exports.

**Outputs**

- `intelligence` constrained model-serving boundary and isolated synthetic news/
  filing pipeline with provenance, schema-constrained output, deadline handling,
  and no privileged tools.
- `research` reproducibility manifest, dataset abstraction, training/evaluation
  harness, offline feature parity, model cards, and reviewed artifact-promotion
  workflow.

**Dependencies**

- Phases 2 and 5; Phase 10 for faithful replay datasets. Production provider
  connectors are blocked by Phase 14 evidence.

**Acceptance criteria**

- Externally supplied text is always data and cannot alter prompts/policy,
  invoke privileged tools, exfiltrate secrets, submit orders, or change mode.
- Model outputs conform to contracts and are rejected when late, malformed,
  incompatible, non-finite, or detached from expected inputs.
- Training/evaluation records include code, dependencies, dataset provenance/
  split, preprocessing/features, seeds, config, artifact hash, and metrics.
- Promotion is explicit and does not automatically deploy or authorize a model.

**Expected tests**

- Prompt injection, nested/encoded instruction, malicious link, oversized and
  malformed document, schema escape, exfiltration/tool attempt, provider retry/
  correction, forecast deadline, model isolation, dataset leakage, fixed-seed
  repeatability, and online/offline feature parity.

**Expected benchmark categories**

- Text validation/extraction throughput, queue/backpressure, model load and
  inference latency/capacity by artifact, deadline-miss rate, training/evaluation
  throughput, dataset read, and parity-check throughput. These remain off the
  execution hot path.

## Phase 13 — Observability and deployment completion

**Inputs**

- Service shells and deployable artifacts, telemetry schemas, topology/capacity
  ADRs, incident requirements, secret references, and environment separation.

**Outputs**

- `observability` collectors/exporters, bounded-cardinality metrics, redacted
  structured logs, non-hot-path traces, dashboards, alerts, and evidence links.
- `deployment` systemd units for colocated edge processes; container/Kubernetes
  artifacts for non-colocated services; network/identity policies; signed
  configuration references; reproducible rollout/rollback and attestations.
- Initial runbooks and compliance evidence for every implemented failure mode.

**Dependencies**

- Phase 3 foundation and each service as it appears. Full completion follows
  Phases 4–12; implementation should be incremental rather than deferred.

**Acceptance criteria**

- Every service exposes all contract fields with health/readiness reason codes,
  configuration hash, bounded telemetry, and graceful shutdown.
- Exporter/collector outage and telemetry saturation cannot block the hot path,
  leak secrets/licensed data, or grant readiness.
- Simulation/paper environments are isolated by identity, network, artifact,
  configuration, and credential references.
- Rollback, restart, disk-full, partition, clock, data, risk, split-brain, and
  kill drills have reviewable evidence.

**Expected tests**

- Cardinality/redaction, exporter outage/backpressure, alert rules, health versus
  readiness, bounded shutdown, systemd restart, container/Kubernetes probes,
  network policy, secret injection/rotation, rollout/rollback, disaster recovery,
  chaos/fault injection, and environment isolation.

**Expected benchmark categories**

- Hot emit-hook overhead, telemetry queue saturation, exporter throughput,
  metric cardinality/memory, log/trace volume, service startup/readiness,
  shutdown/drain, rollout/recovery, and resource utilization under representative
  simulation load.

## Phase 14 — Licensed paper and provider integrations (externally blocked)

**Inputs**

- Completed generic/synthetic boundaries, authorized current specifications,
  legal entitlement and use approval, secure credentials outside the repository,
  provider/venue test access, certification plan, and approved derived fixtures.

**Outputs**

- One isolated provider or venue adapter per reviewed vertical slice, beginning
  with non-money paper/certification environments.
- Specification/version provenance, conformance matrix, sanitized synthetic
  fixtures, certification evidence, operational runbooks, and explicit
  unsupported behavior.

**Dependencies**

- Phases 9–13 and every prerequisite in
  [Licensed Integration Boundaries](licensed-integration-boundaries.md). Work on
  a concrete adapter remains blocked until its own evidence is present.

**Acceptance criteria**

- No protocol field or behavior is inferred or fabricated.
- Adapter output conforms to internal contracts and unknown provider state fails
  closed.
- Paper/certification is isolated from real endpoints/accounts and cannot enable
  live transmission.
- Sequence/session recovery, corrections, throttles, timestamps, status, order
  lifecycle, reconciliation, entitlements, retention, and provider error
  semantics are verified against authorized specifications.
- Required venue/provider certification and soak evidence is accepted.

**Expected tests**

- Licensed conformance suite using authorized or sanitized fixtures; malformed/
  unknown message fuzzing; sequence/session recovery; reconnect/throttle;
  corrections/retractions; account/order reconciliation; certification scripts;
  network/credential isolation; fail-closed unknown-version tests; full paper
  fault-injection soak.

**Expected benchmark categories**

- Provider decode/normalize, gap recovery, order encode/decode, session burst,
  rate-limit behavior, book update, gateway final gate, reconnect/reconciliation,
  throughput, queue capacity, allocations, and tail latency on representative
  hardware and approved feeds.

## Phase 15 — Conditional live-readiness program (not authorized or scheduled)

**Inputs**

- Every prior phase complete; legal/compliance/venue approvals; current licensed
  specifications; accepted live-safety ADR set; production identity, signing,
  clock, market-data, risk, fencing, journal, surveillance, on-call, incident,
  and disaster-recovery evidence; successful representative paper soak.

**Outputs**

- Only if separately authorized: live-capable build profile default-off from all
  ordinary builds, signed configuration/authorization workflow, two-stage
  activation evidence, independently tested final gateway gates, and operator/
  compliance runbooks.

**Dependencies**

- All previous phases and external organizational authority. Repository
  completeness alone cannot satisfy this gate.

**Acceptance criteria**

- Each live predicate is independently verified continuously and at the final
  transmission boundary; `unknown` is false.
- Non-live builds contain no live state, real credentials, real routes, or live
  adapter plugin. Restart/recovery never restores live authority.
- Every activation/rejection/transmission is durably auditable and replayable.
- Independent safety, security, compliance, venue certification, capacity,
  resilience, and operational reviews approve the exact artifact/configuration.
- No break-glass path can create or bypass live authority.

**Expected tests**

- Complete negative activation matrix, final-boundary bypass testing, signed
  config/auth expiry/replay/scope, risk/data/clock/journal/fencing/kill failures,
  split brain and partition, restart/recovery, venue ambiguity, surveillance,
  security penetration, chaos, disaster recovery, operator drills, and long
  paper/canary evidence where legally and operationally approved.

**Expected benchmark categories**

- Full representative end-to-end latency/throughput percentiles, allocation,
  queue occupancy/capacity, feed bursts, risk and gateway gates, journal impact,
  kill propagation, failover/recovery, and sustained soak on production-equivalent
  hardware. Performance never overrides a failed safety gate.

## Critical blocking decisions

The following decisions are dependencies, not implementation details to guess:

1. Canonical schemas, serialization, compatibility, ordering, identifiers, and
   numeric semantics.
2. Bounded queue capacities and overload outcomes.
3. Mandatory journal durability and failure policy.
4. Clock-health sources and thresholds.
5. Market-data validity/freshness and recovery semantics.
6. Risk snapshot, approval lifetime, reconciliation, and concrete limit ownership.
7. OMS idempotency and venue-specific ambiguous-state policy.
8. Configuration trust roots, signing, anti-replay, authorization, and expiry.
9. Single-writer leadership and fencing.
10. Artifact promotion, retention, access, and rollback.
11. Performance workloads, target hardware, objectives, and regression policy.
12. Licensed provider/venue semantics and organizational compliance requirements.

Until these dependencies are resolved at their named phase, downstream work may
use only explicit synthetic fixtures and interfaces and cannot claim production
completion.
