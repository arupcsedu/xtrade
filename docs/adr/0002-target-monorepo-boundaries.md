# ADR-0002: Target Monorepo Boundaries

| Field | Value |
| --- | --- |
| Status | Accepted |
| Date | 2026-08-28 |
| Deciders | Aegis-MX principal engineering baseline |

## Context

The repository audit found no source code, packages, build system, schemas,
services, tests, or infrastructure. A dependency-aware boundary model is needed
before code is introduced so that the execution path cannot accidentally depend
on model runtimes, control RPC, storage, observability exporters, research code,
or provider SDKs.

The engineering contract requires a C++20+ bounded hot path, model-to-forecast
isolation, deterministic pre-trade risk, a sole mode-gated venue boundary,
replayability, and off-hot-path Python/LLM/distributed services. ADR-0001 already
fixes the primary safety and live-activation boundaries. This ADR assigns
component ownership and source-dependency direction.

## Decision

Adopt a modular monorepo with these top-level domain component boundaries:

1. `edge-core`
2. `market-data`
3. `order-book`
4. `feature-engine`
5. `model-contracts`
6. `ensemble`
7. `risk`
8. `OMS`
9. `gateways`
10. `intelligence`
11. `replay`
12. `research`
13. `control-plane`
14. `observability`
15. `deployment`

These are build, ownership, API, and test boundaries. They are not automatically
separate processes. Hot-path components may be statically linked into a
colocated edge process, with process splits justified later by latency, fault,
security, scaling, or operational evidence.

The source dependency direction is the acyclic graph in
[`../architecture/dependency-graph.mmd`](../architecture/dependency-graph.mmd).
`A --> B` means `A` may import `B`'s public contract. Runtime data flow is wired
through producer-owned immutable contracts and dependency injection and does not
grant a reverse import.

In particular:

- `edge-core` is the minimal foundation and imports no domain component.
- `market-data`, `order-book`, `feature-engine`, `ensemble`, `risk`, `OMS`, and
  `gateways` contain the hard real-time-like decision path and use bounded C++
  interfaces.
- `model-contracts` owns model interchange but no model implementation.
- `intelligence` publishes contracts only and has no order, risk-approval,
  gateway, or control-authority API.
- `gateways` is the only component allowed to contain venue transmission code.
- `replay` may reuse production decision libraries, but no production library
  imports replay and replay cannot load real transmission plugins.
- `research`, `control-plane`, `observability`, and `deployment` never become
  dependencies of hot-path decision libraries.
- Observability exporters and storage run behind bounded nonblocking handoffs.
- Provider SDKs and licensed details are private to authorized adapters and do
  not leak into normalized internal contracts.
- A catch-all `common` or `utils` domain is prohibited; shared APIs require an
  explicit owner.

The full responsibilities, allowed dependencies, execution classifications, and
contract ownership are defined in
[`../architecture/component-boundaries.md`](../architecture/component-boundaries.md).

## Consequences

### Positive

- Source dependency cycles can be detected mechanically against a small allowed
  graph.
- Hot-path code remains insulated from blocking/control/research dependencies.
- Models cannot acquire a transitive path to OMS or gateway APIs.
- Risk and final gateway enforcement remain independent and directly testable.
- Production and replay use the same decision libraries without production code
  depending on replay drivers.
- Licensed provider details remain contained and replaceable behind normalized
  interfaces.

### Costs and constraints

- Contract ownership must be designed before implementation and may require
  narrow generated bindings across languages.
- Composition roots must wire sibling components without accumulating business
  logic.
- Some runtime flows are intentionally opposite to source ownership, requiring
  injected sinks or producer-owned event interfaces.
- The component boundaries add build and test targets even when several
  components share a process.
- Moving an API between owners becomes a versioned migration rather than an
  informal shared-header edit.

## Alternatives considered

### One hot-path component containing all trading behavior

Rejected. It would obscure model, risk, OMS, gateway, and market-state authority,
make bypasses difficult to detect, and couple replay/testing to internal state.

### One deployable microservice per named component

Rejected as a default. Network RPCs between the per-event components violate the
hot-path boundary and add failure/latency modes without evidence that process
separation is needed.

### Shared `common` or `utils` package

Rejected. Catch-all sharing tends to create hidden bidirectional dependencies
and unstable ownership. Truly foundational behavior belongs in the deliberately
small `edge-core`; domain contracts stay with their producer.

### Control-plane-owned shared schemas

Rejected. It would make the execution path depend conceptually or mechanically
on an off-path service. Cross-language control schemas consume narrow
component-owned contracts instead.

### Gateway-owned OMS lifecycle

Rejected. Internal order state must remain provider-neutral and testable with a
synthetic gateway. OMS owns normalized command/event interfaces; concrete
gateways implement them privately.

## Enforcement

Phase 1 shall add build targets and dependency checks matching the graph. Public
and private APIs must be explicit. CI shall reject undeclared links/imports and
cycles. Python environments must not be linked or embedded in hot-path targets.
Replay targets link only synthetic gateway interfaces. Production gateway
plugins, if ever authorized, require separate artifacts and must not be present
in simulation/research builds.

Any new top-level component, reversed edge, direct sibling-internal import, or
process-boundary change requires an ADR with hot-path, fault, replay,
compatibility, and migration analysis.
