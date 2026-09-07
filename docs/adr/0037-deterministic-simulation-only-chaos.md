# ADR-0037: Deterministic, simulation-only chaos orchestration

| Field | Value |
| --- | --- |
| Status | Accepted |
| Date | 2026-09-04 |
| Owners | Edge reliability, market core, model platform, control plane |

## Context

Aegis-MX has fault hooks in packet replay and feed recovery, clock-quality
state, bounded queues, asynchronous model services, intelligence, paper
gateways, portfolio reconciliation, the append-only journal, configuration
validation, and leader fencing. A cross-component chaos test must verify their
common fail-closed contract without creating a second trading runtime, relying
on wall-clock sleeps, or exhausting an engineer's workstation or CI runner.

Expected outcomes and detector behavior must not share a mutable source.
Otherwise changing an expectation could silently make an incorrect detector
appear to pass. Reports must also be reproducible and useful as audit evidence.

## Decision

Use a Python 3.12 orchestration tool outside the hot path. The checked-in
[scenario catalog](../../infra/chaos/scenario-catalog-v1.json) is the normative
expectation source. Independent fault behaviors in
[the scenario runner](../../tools/chaos_runner.py) mutate named signals in a
bounded synthetic edge harness and derive observed detection, component-local
state transition, automated response, recovery evidence, and audit events.

The runner is permanently in SIMULATION mode and has no OMS or gateway
transmission interface. CPU starvation and memory pressure are injected as
scheduler and allocation-pool quotas; they do not consume unbounded host CPU or
memory. Detection deadlines use deterministic logical nanoseconds, with
content-derived jitter, rather than scheduler timing.

The fast profile exercises all individual faults once. The nightly profile
repeats all individual faults and co-injects defined combinations into one
synthetic signal plane. Each report conforms to the
[v1 JSON Schema](../../schemas/chaos-result-report-v1.schema.json), contains the
catalog hash and seed, and is itself content hashed.

## Consequences

- Expectations can be reviewed without reading detector code, while drift
  fails an executable assertion.
- CI results are repeatable across differently loaded hosts.
- The framework validates orchestration and safety contracts; it does not prove
  physical NIC, kernel, GPU, disk, witness, or venue behavior.
- Component-native tests and staging drills remain necessary. Production
  adapters may implement the same scenario boundary only after isolation,
  access-control, and rollback review.
- No chaos injector may be shipped into or enabled in a live-capable process.

## Rejected alternatives

Wall-clock sleeps and deliberate host resource exhaustion were rejected because
they are nondeterministic and can damage unrelated workloads. A production
network or gateway fault controller was rejected because this repository has no
licensed connectivity and because an accidental live target would violate the
engineering contract. Copying expected outcomes directly into reports was
rejected because it would not test detection.
