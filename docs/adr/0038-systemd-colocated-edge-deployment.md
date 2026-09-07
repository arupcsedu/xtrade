# ADR 0038: Systemd and immutable profiles for the colocated edge

- Status: Accepted
- Date: 2026-09-04

## Context

The colocated edge needs host-level CPU, NUMA, NIC, PTP, memory, and storage control. Kubernetes scheduling and virtualized networking cannot express or preserve all of those properties with sufficient clarity for the innermost path. The repository currently provides edge libraries and offline tools, but does not yet provide the five composed daemon binaries named by the deployment contract.

## Decision

Deploy the edge as versioned release directories selected by an atomic `current` symlink and supervised by systemd. Store resource intent in immutable, secret-free JSON profiles. Render CPU affinity, NUMA binding, memory-lock, and file-descriptor limits into systemd drop-ins. Keep host tuning as reviewed example files rather than applying it from build or test automation.

Every profile is restricted to `SIMULATION` or `PAPER`, sets live transmission and automatic activation to false, and is validated before rendering or packaging. Staging-class profiles require signed runtime configuration, PTP, disjoint CPUs, and an intentionally unresolved NIC identity. The checked-in `production-disabled` profile is simulation-only. No production-enabled profile is defined.

The service units are deployment contracts until their binaries exist. A missing binary, configuration, health record, PTP state, or host mapping prevents readiness. Local bounded status files are used for systemd health checks; OpenTelemetry remains outside the strict hot path.

Rollback artifacts are deterministic tar archives containing base assets, the selected profile, rendered drop-ins, instructions, and checksums. They never install, switch releases, start services, or activate trading.

## Consequences

- Edge deployment does not depend on Kubernetes.
- Hardware-specific values require measured operator approval.
- Configuration can be audited and rollback packages compared byte-for-byte.
- Starting `aegis-edge.target` cannot succeed until composition daemons are implemented and installed.
- PTP and unresolved production NIC prerequisites fail closed.

## Alternatives rejected

- Kubernetes for the hot path: insufficiently direct ownership of IRQs, queues, and CPUs.
- One mutable host script: difficult to review, reproduce, and roll back.
- Auto-detecting and applying topology: a mistaken guess could degrade determinism or bind the wrong interface.
- Shipping a live profile: conflicts with the engineering safety contract.
