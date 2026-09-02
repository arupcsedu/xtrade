# Aegis-MX Component Boundaries

| Field | Value |
| --- | --- |
| Status | Target monorepo architecture |
| Date | 2026-08-28 |
| Decision record | [ADR-0002](../adr/0002-target-monorepo-boundaries.md) |
| Dependency graph | [Mermaid source](dependency-graph.mmd) |

The language-oriented physical layout adopted in
[ADR-0003](../adr/0003-monorepo-build-and-ci-foundation.md) implements these
logical boundaries without changing their ownership or dependency direction.

## Physical monorepo mapping

| Logical boundary | Physical foundation path |
| --- | --- |
| `edge-core` | `cpp/common`, `cpp/event_bus`, `cpp/journal` |
| `market-data` | `cpp/market_data` |
| `order-book` | `cpp/order_book` |
| `feature-engine` | `cpp/features` |
| `model-contracts` | `schemas`, `cpp/models`, Python generated bindings when introduced |
| `ensemble` | `cpp/ensemble` |
| `risk` | `cpp/risk`, `control/risk_admin` |
| `OMS` | `cpp/oms` |
| `gateways` | `cpp/execution` |
| `intelligence` | `python/intelligence`, `python/model_serving` |
| `replay` | `cpp/replay`, `python/backtesting` for offline consumers only |
| `research` | `python/research`, `python/training` |
| `control-plane` | `control/config_service`, `control/model_registry`, `control/risk_admin` |
| `observability` | Future language-owned adapters plus `infra`; no catch-all hot-path import |
| `deployment` | `infra` |

`cpp/benchmarks`, `python/tests`, `docs`, and `tools` are validation/support
roots rather than domain components. Empty directories are reserved physical
boundaries only; they are not stub implementations or phase-completion evidence.

The implemented `control/model_registry` boundary also contains the asynchronous
shadow/canary deployment coordinator. It consumes paired forecast observations,
publishes only non-executable hypothetical/scope records, and invokes signed
registry rollback or disable. It is forbidden from importing or bypassing C++
risk, OMS, execution, or gateway paths.

## Boundary rules

The target is a modular monorepo, not a distributed-service mandate. A component
is an ownership, API, build, and test boundary; several hot-path components may
be linked into one colocated C++ process. A component becomes a separate process
only when latency, fault isolation, deployment, security, or scaling evidence
justifies it through an ADR.

The following rules apply to every boundary:

- Dependencies must follow the arrows in the component dependency graph. An
  arrow `A --> B` means source in `A` may depend on the public contract of `B`.
- Runtime data flow does not imply reverse source dependency. Consumers use
  producer-owned immutable contracts, injected interfaces, or composition-root
  wiring.
- Public APIs are versioned and minimal. Internal headers/modules are not
  imported across boundaries.
- Hot-path libraries never import control-plane, observability exporter,
  deployment, research, replay-driver, Python, or provider SDK code.
- `replay` may invoke production decision libraries, but production libraries
  never import `replay`.
- `research` may consume exported contracts and replay data; no production
  component imports `research`.
- `gateways` is the only boundary permitted to contain venue transmission code.
  Models and intelligence have no gateway dependency.
- Component-owned generated code is reproducible from a checked-in,
  machine-readable schema. Missing or unknown fields fail according to a
  versioned compatibility policy, never an unsafe default.
- Cross-language RPC and Protobuf remain outside the innermost hot path. Hot-path
  representation and serialization require their own ADR.
- Any deployable service implements the common service contract: build/version,
  health, readiness, configuration hash, bounded metrics, structured logs, and
  bounded graceful shutdown.

Repository-wide `docs/`, build/tooling files, CI policy, and third-party
dependency declarations support the component roots but are not domain
components. A generic catch-all `common` or `utils` component is prohibited;
shared behavior must have a named owner and a stable reason to be shared.

## Execution classes

“Hard real-time-like” describes the engineering discipline of a bounded,
nonblocking colocated path. It does not claim formal hard-real-time scheduling or
an absolute deadline until platform measurements and an ADR establish one.

| Class | Definition | Permitted behavior |
| --- | --- | --- |
| Hard real-time-like hot path | Per-event path from validated market input through state, features, ensemble, risk, OMS transition, and final gateway gate | C++20+, preallocated bounded work, monotonic deadlines, no dynamic allocation, blocking RPC, disk wait, Python/LLM, GC, or distributed database |
| Near-real-time | Deadline-sensitive work that may run outside the innermost path and whose late result is rejected or degrades capability safely | Bounded queues and deadlines; nonblocking handoff to hot path; no authority extension on delay/failure |
| Asynchronous | Control, persistence, telemetry, text ingestion, training coordination, and recovery work not needed to finish the current decision | Backpressure, idempotency, explicit retry/dead-letter policy, durable audit where required |
| Offline | Replay analysis, research, model training/evaluation, capacity analysis, build, packaging, and deployment generation | Reproducible artifacts and fixed seeds; no real venue transmission capability |

## Component catalog

### `edge-core/`

**Purpose:** Own the minimal C++20 foundation for safe colocated components:
fixed-width domain primitives, monotonic/event time wrappers, stable identities,
status/reason codes, bounded queues, capacity outcomes, immutable configuration
identity, build metadata, audit envelopes, and nonblocking journal handoff
interfaces.

`edge-core` also owns the neutral authoritative market-state controller and its
immutable publication contract. This prevents ensemble, risk, OMS, or gateways
from defining competing halt/clock/data/kill/event precedence.

**Inputs:** Versioned schema/ordering/serialization ADRs and platform clock/build
information.

**Outputs:** Narrow foundational C++ APIs and, where cross-language access is
required, language-neutral schemas with generated bindings.

**Allowed dependencies:** Standard library and explicitly approved low-level
libraries only. It depends on no domain component.

**Execution class:** Hot-path primitives plus near-real-time journal/service
shells. Disk persistence and telemetry export are asynchronous implementations
behind bounded handoffs.

**Must not own:** Strategy/model behavior, venue protocols, risk policy, global
mutable service locators, network RPC clients, or unbounded convenience
containers in hot APIs.

The implemented queue, snapshot, epoch, and host-local forecast-cache contracts
are specified in [Colocated Event Bus and Shared State](colocated-event-bus.md).
The asynchronous persistence boundary, binary format, overload policy, recovery,
retention, and copy-only repair semantics are specified in the
[Append-only Journal](append-only-journal.md). The durable writer remains
asynchronous; only its bounded publication handoff belongs near a hot path.

### `market-data/`

**Purpose:** Own feed adapter interfaces, packet/message validation, normalized
market/reference/status events, sequence and duplicate detection, freshness and
gap state, and synthetic/reference feeds.

**Inputs:** Authorized provider payloads or visibly synthetic reference
protocols, instrument/reference mappings, and clock-health snapshots.

**Outputs:** Canonically ordered normalized events and explicit
`VALID`/`DEGRADED`/`STALE`/`INVALID` state with reason codes.

**Allowed dependencies:** `edge-core` only for foundational APIs. Provider SDKs
or decoders remain private adapter modules.

**Execution class:** Ingress decode/validation/normalization is hot-path;
snapshot acquisition and gap recovery are near-real-time. The state remains
invalid until recovery completes.

**Licensed boundary:** Production provider/venue decoders, channel/session
semantics, and recovery cannot be completed without authorized specifications.

### `order-book/`

**Purpose:** Own deterministic per-instrument book state, snapshots, invariant
checks, status/halt propagation, sequence application, and invalidation/recovery
transitions.

**Inputs:** Normalized `market-data` events and market-state validity.

**Outputs:** Immutable identified book/market-state views suitable for features,
risk, gateway safety, journaling, and replay.

**Allowed dependencies:** Public `market-data` contracts and `edge-core`.

**Execution class:** Hot-path event application and reads; near-real-time
snapshot rebuild occurs outside the active book and swaps only after validation.

**Licensed boundary:** Generic book algorithms and synthetic books are
implementable; production semantics depend on feed-specific update, priority,
auction, bust/correction, and recovery rules.

### `feature-engine/`

**Purpose:** Own versioned deterministic online feature definitions, bounded
state, feature computation, and immutable feature snapshots. Offline equivalents
must prove parity.

**Inputs:** Normalized market events, validated book/market-state views, and
versioned feature configuration.

**Outputs:** Content-identified feature snapshots through `model-contracts`.

**Allowed dependencies:** `edge-core`, public `market-data` and `order-book`
contracts, and `model-contracts`.

**Execution class:** Online features are hot-path. Bulk backfill and parity
analysis are offline.

**Must not own:** Model inference, order intent, risk decisions, or access to raw
untrusted text.

### `model-contracts/`

**Purpose:** Own versioned feature-snapshot, model-manifest, forecast, validity,
deadline, units, and model-health schemas plus generated C++/Python bindings.
It contains no model implementation.

**Inputs:** Foundational identity/time/unit contracts and accepted compatibility
ADRs.

**Outputs:** Validated immutable messages that bind model/version, feature
snapshot, decision/event identity, creation time, deadline, units, and validity.

**Allowed dependencies:** Only the narrow foundational contract subset of
`edge-core`; generated Python bindings must not import the C++ runtime.

**Execution class:** Contract validation on the decision boundary is hot-path;
schema generation and registry operations are offline/asynchronous.

### `ensemble/`

**Purpose:** Own deterministic versioned policy that validates and combines
on-time forecasts into an uncertainty-aware ensemble forecast or an explicit
abstention reason.

**Inputs:** Validated `model-contracts` forecasts, immutable ensemble state,
configuration identity, event/decision identity, and deadlines.

**Outputs:** Immutable fixed-point `EnsembleForecast`, canonical per-expert
weights, explanation record, or explicit abstention. Intent translation is a
later separate boundary. A forecast never becomes an order by itself.

**Allowed dependencies:** `edge-core` and `model-contracts` only.

**Execution class:** Hot-path. It never waits for a model; missing, incompatible,
or late forecasts follow deterministic fail-closed/degradation policy.

### `risk/`

**Purpose:** Own independent deterministic pre-trade evaluation, immutable risk
snapshots, limits and restricted state, kill/halt aggregation, approval binding,
and stable rejection reasons.

**Inputs:** Order intent, current position/exposure state, validated market/book
snapshot identity, configuration/policy identity, clock state, and independent
kill/halt inputs.

**Outputs:** A short-lived integrity-bound approval for the exact immutable
intent or a deterministic rejection. Health/readiness is separate from an
individual result.

**Allowed dependencies:** `edge-core` and narrow read-only `order-book`
contracts. It does not depend on ensemble/model implementation.

**Execution class:** Per-intent pre-trade checks and kill/halt reads are hot-path;
policy distribution, exposure reconciliation, and surveillance export are
near-real-time/asynchronous.

### `OMS/`

**Purpose:** Own the internal order lifecycle, client order identities,
idempotency, state transitions, pending/open/filled/cancelled/rejected state,
position event derivation, and reconciliation interfaces.

**Inputs:** Risk-approved immutable intents and normalized venue events.

**Outputs:** Gateway commands bound to risk approval; deterministic lifecycle and
position events for risk, journal, and replay.

**Allowed dependencies:** `edge-core` and public `risk` approval contracts. OMS
owns the narrow command/event contract that gateway adapters implement, so OMS
does not import concrete gateway code.

**Execution class:** Normal lifecycle transitions are hot-path; session/account
reconciliation is near-real-time and keeps transmission inhibited until
complete.

### `gateways/`

**Purpose:** Own deterministic execution-policy slicing and smart venue
selection, plus the sole venue transmission boundary, mode state machine, final
safety predicate, session/fencing state, synthetic/reference adapters, and
private venue encoders/decoders. A router decision is only a proposed child and
cannot call a gateway or bypass fresh risk and OMS processing.

**Inputs:** Venue-free execution objectives, validated direct/consolidated market
state and venue observations for routing; then OMS commands, current risk
approval/health, market-data validity, clock state, signed configuration/
authorization evidence, leadership fencing, kill/halt state, and adapter session
state for the independent gateway boundary.

**Outputs:** Risk-required, fixed-point routing decisions and cost attribution;
in simulation or paper, deterministic venue commands/events; for a future
authorized adapter, encoded venue messages and normalized responses.

**Allowed dependencies:** `edge-core`, public `market-data`, `risk`, and `OMS`
contracts. No model, ensemble, intelligence, research, control-plane client, or
distributed-storage import is permitted in the final gate.

**Execution class:** Final validation and encode/decode is hot-path. Session
setup, activation, reconciliation, and durable activation recording are
near-real-time/asynchronous and cannot grant authority while incomplete.

**Licensed boundary:** A real venue adapter and venue-aware recovery are blocked
until authorized protocol, certification, and operational specifications exist.

### `intelligence/`

**Purpose:** Own off-hot-path model serving and untrusted news/filing ingestion,
provenance, prompt-injection isolation, constrained extraction, model artifact
loading, and forecast publication.

**Inputs:** Feature snapshots, approved model artifacts, licensed or synthetic
external text/events, provider metadata, and strict schemas.

**Outputs:** Versioned forecasts or sanitized structured intelligence events
through `model-contracts`; explicit late/rejected/error outcomes.

**Allowed dependencies:** `model-contracts` and narrow `edge-core` identity/time
contracts. It has no OMS, risk-approval, gateway, or control-authority API.

**Execution class:** Deadline-bound model serving may be near-real-time but is
never synchronously awaited by the hot path. Text ingestion and enrichment are
asynchronous.

**Licensed boundary:** Production news/provider connectors, entitlements,
retention, corrections, timestamps, and redistribution behavior require
authorized specifications and agreements.

### `replay/`

**Purpose:** Own journal verification, deterministic faithful replay,
counterfactual-mode separation, divergence reporting, artifact resolution, and
replay orchestration.

**Inputs:** Checksummed journal segments plus exact schemas, build/config/model/
feature/risk artifacts and deterministic seeds.

**Outputs:** Canonical reproduced events/decisions/rejections, integrity reports,
and explicit divergence records.

**Allowed dependencies:** Public contracts and reusable decision libraries from
`edge-core`, `market-data`, `order-book`, `feature-engine`, `model-contracts`,
`ensemble`, `risk`, `OMS`, and the synthetic-only gateway interface.

**Execution class:** Offline or asynchronous. Replay binaries cannot contain or
load real-venue transmission plugins or credentials.

### `research/`

**Purpose:** Own notebooks only when reproducible, dataset definitions,
experiments, training/evaluation, counterfactual analysis, offline feature
parity, model cards, and artifact promotion candidates.

**Inputs:** Licensed/approved datasets through governed abstractions, replay
exports, model contracts, code/dependency locks, and fixed seeds.

**Outputs:** Reproducible reports and content-addressed candidate model/feature
artifacts. Promotion is a separate reviewed process.

**Allowed dependencies:** `replay`, `model-contracts`, and offline-facing
`feature-engine` contracts. Production components never depend on research.

**Execution class:** Offline.

### `control-plane/`

**Purpose:** Own authenticated/authorized operator APIs, signed configuration
distribution and verification support, immutable model-artifact registration
and promotion, activation workflow coordination, authorization leases,
desired-state audit, and fleet/service status views.

**Inputs:** Operator/compliance actions, signed configuration and model
artifacts, exact feature contracts, replay/validation evidence, service
health/readiness, and environment identity.

**Outputs:** Scoped versioned control commands, configuration snapshots, signed
append-only model lifecycle events, and signed deployment pointers. A control
command or model approval is evidence, not direct permission to bypass local
gates.

**Allowed dependencies:** Language-neutral control/health contracts owned by
`edge-core`, `risk`, and `gateways`. Hot components never import a control-plane
client into their decision path.

**Execution class:** Near-real-time safety actions and asynchronous management.
Partitions or delays expire authority closed.

**Implemented configuration slice (2026-09-02):** The standard-library Go
configuration service owns canonical signed immutable snapshots, two-person
critical approval, staged activation, compatible rollback, scoped engage-only
emergency kills, signed hash-chained audit, RBAC/replay checks, and an atomic
RPC-free local edge cache. It recognizes only simulation and paper as valid
runtime modes. Authenticated network transport, production identity/HSM
adapters, multi-host consensus, and an online signed rollback-selection
distributor remain deployment boundaries. See the
[configuration control-plane design](configuration-control-plane.md).

### `observability/`

**Purpose:** Own Prometheus/OpenTelemetry adapters, structured-log schemas,
redaction, collection/export, bounded-cardinality policy, dashboards, alerts,
and evidence retention integration.

**Inputs:** Bounded nonblocking telemetry and service-health snapshots.

**Outputs:** Metrics, logs, traces, alerts, and operational views without secrets
or licensed payloads.

**Allowed dependencies:** Stable telemetry/health contracts from `edge-core`.
Components emit through narrow injected interfaces and do not import exporters.

**Execution class:** Asynchronous. An exporter outage cannot block the hot path
or create trading readiness.

### `deployment/`

**Purpose:** Own reproducible packaging, environment overlays, systemd units for
colocated edge services, containers/Kubernetes for non-colocated services,
network/identity policy, configuration references, rollout, rollback, and
artifact attestation.

**Inputs:** Signed build artifacts, service manifests, configuration schemas,
capacity evidence, secret references, and approved topology ADRs.

**Outputs:** Environment-specific, reviewable deployment artifacts. Simulation,
paper, and any future live environment use separate identities, routes,
credentials, configurations, and build provenance.

**Allowed dependencies:** Published artifacts and manifests from all deployable
components; no component source depends on deployment.

**Execution class:** Offline generation and asynchronous orchestration.

## Execution-path classification

| Component | Hard real-time-like hot path | Near-real-time | Asynchronous | Offline |
| --- | --- | --- | --- | --- |
| `edge-core` | Types, clocks, queues, status, audit enqueue | Service shell | Journal writer | Build/schema generation |
| `market-data` | Decode, validate, normalize, sequence | Gap/snapshot recovery | Feed/session diagnostics | Fixture generation |
| `order-book` | Apply event, validate, publish view | Shadow rebuild/recovery | Diagnostics | Golden-state generation |
| `feature-engine` | Online update and snapshot | Optional bounded off-path feature | Artifact distribution | Backfill/parity |
| `model-contracts` | Forecast/snapshot validation | Registry cache refresh | Registry publication | Code generation |
| `ensemble` | Validate/combine/intent | None in decision flow | Policy distribution | Policy evaluation |
| `risk` | Pre-trade checks and kill/halt read | Exposure reconciliation | Surveillance export | Limit analysis |
| `OMS` | Order state transition | Venue/account reconciliation | History export | State-machine analysis |
| `gateways` | Final gate and encode/decode | Session, fencing, activation | Audit/control handoff | Certification fixtures |
| `intelligence` | Never | Deadline-bound serving | News/filing pipeline | Training belongs in research |
| `replay` | Never transmits | Optional accelerated replay | Journal verification | Faithful/counterfactual replay |
| `research` | Never | Never | Artifact promotion workflow | Experiments/training |
| `control-plane` | Never in decision path | Kill/config/auth delivery | Fleet management/audit | Policy packaging |
| `observability` | Nonblocking emit hook only | Local aggregation | Export/alert | Capacity/report analysis |
| `deployment` | Never | Supervisor health actions | Rollout/rollback | Build/render/attest |

## Contract ownership and cycle prevention

Producer-owned contracts separate data flow from import direction:

- `market-data` owns normalized market events; `order-book` consumes them.
- `order-book` owns immutable book views; feature and risk consumers import the
  view contract, not book internals.
- `model-contracts` owns feature/forecast interchange; feature and intelligence
  publish it, ensemble consumes it.
- `edge-core` owns the neutral order-intent primitives required by ensemble and
  risk so risk does not import ensemble implementation.
- `risk` owns approval/rejection contracts; OMS imports them.
- `OMS` owns normalized command/lifecycle event interfaces; gateways implement
  them, so OMS does not import gateway implementations.
- `edge-core` owns narrow health/telemetry/configuration identities;
  observability and control-plane consume them without becoming production
  hot-path dependencies.

Composition roots may depend on multiple siblings to wire them together, but
composition code contains no business logic and is placed with the deployable
artifact it constructs. Any requested dependency not present in the graph
requires an ADR and a cycle analysis.

## Licensed completion boundary

The generic contracts, deterministic cores, synthetic/reference adapters,
simulation, replay, and research harnesses can be completed without proprietary
protocol material. Production market-data adapters, venue order gateways,
venue-specific OMS recovery, provider-specific news ingestion, certification,
and entitlement/retention behavior cannot. The exact boundary and evidence
requirements are defined in
[Licensed Integration Boundaries](licensed-integration-boundaries.md).
