# Colocated edge deployment architecture

## Scope and safety state

The edge is a bare-metal, systemd-oriented deployment. Kubernetes is reserved for non-colocated services. All checked-in profiles are non-live, contain no credentials or provider endpoints, and set both `live_transmission_enabled` and `automatic_activation` to false. `production-disabled` is an installation/performance-validation profile, not authorization to trade.

The units in `infra/edge/systemd` define lifecycle and security contracts for five
installed non-live process entry points. Their common service shell publishes
build/configuration identity, structured lifecycle logs, health/readiness,
systemd readiness/watchdog signals, and graceful shutdown state. These entry
points supply operable process boundaries; licensed feed/gateway composition and
target-host qualification remain separate blockers.
The units also require `aegis-edge-config-verify` to bind the rendered manifest
and profile and validate a trusted Ed25519 configuration signature, expiry,
environment, mode, and canonical runtime content before every service start; the
filename suffix is never treated as proof of a valid signature.

## Service order

| Start | Service | Responsibility | Stops |
|---:|---|---|---:|
| 0 | inhibit initializer | create the default local trading inhibit | 60 |
| 10 | clock guard | PTP/hardware timestamp readiness | 50 |
| 20 | journal | durable append and recovery boundary | 40 |
| 30 | edge core | feed, book, features, models, ensemble, and risk composition | 30 |
| 40 | observability | asynchronous export and local health aggregation | 20 |
| 50 | paper gateway | synthetic/paper order transport only | 10 |

Systemd `Requires`, `After`, and `Before` relationships encode startup and
reverse shutdown. The inhibit exists before any edge daemon starts, and the
gateway writes it again before termination. Clearing it is outside this bundle
and requires the existing approved control/kill-switch workflow; readiness
alone never clears it. Any critical-unit failure writes
`/run/aegis-mx/trading.inhibit`. The health timer validates bounded local files
and never makes a network request. Graceful stop deadlines allow the gateway to
stop first, the core to drain, and the journal to flush before the clock guard
exits.

## Immutable layout

```text
/opt/aegis-mx/releases/<artifact-hash>/  read-only release
/opt/aegis-mx/current -> releases/...   operator-switched symlink
/etc/aegis-mx/config/                  signed runtime configuration
/etc/aegis-mx/environment/             rendered non-secret environment
/etc/aegis-mx/deployment/              active profile, manifest, and reviewed host facts
/var/lib/aegis-mx/journal/             dedicated local NVMe journal
/var/lib/aegis-mx/                     snapshots and recovery state
/var/log/aegis-mx/                     asynchronous structured logs
/run/aegis-mx/health/                  bounded local health records
/run/aegis-mx/trading.inhibit           fail-closed inhibit record
```

The journal device should be dedicated, mounted by stable UUID, and excluded from unrelated workloads. Segments remain immutable after rotation; retention removes only segments made eligible by the journal retention process, never the active segment. Audit retention policy takes precedence over the profile minimum.

## CPU, NUMA, and memory

Production-like profiles assume at least 16 logical CPUs on NUMA node 0. CPUs 0-1 are housekeeping/IRQ CPUs; clock, journal, edge core, gateway, and observability use disjoint CPUs 2-13. This is a reference allocation, not universal hardware truth. Hyperthread siblings and NUMA-local memory must be checked from actual topology before approval.

Generated drop-ins set `CPUAffinity`, `NUMAPolicy=bind`, `NUMAMask`, `LimitMEMLOCK`, and `LimitNOFILE`. Huge pages are configured at host boot and validated against captured host facts. Transparent huge pages are disabled in the example boot arguments. The application remains responsible for prefaulting memory and calling `mlockall`; a limit alone does not lock memory.

## NIC and PTP

An approved NIC needs hardware receive timestamps, sufficient independent RX queues, stable PHC association, RSS/flow steering appropriate to the licensed feed, and a supported driver/firmware combination. Market-data A/B, gateway, and drop-copy queues are distinct in the reference plan. IRQ affinity must target housekeeping or explicitly assigned receive CPUs and must not drift under `irqbalance`.

The deployment does not invent exchange multicast groups, protocols, credentials, VLANs, or broker endpoints. Those remain licensed integration data. PTP readiness requires `ptp4l`, `phc2sys`, a fresh `clock-quality.json`, hardware timestamp availability, `HEALTHY` state, and offset within the selected profile. An unsafe or stale clock blocks readiness.
The core and paper gateway units permit only Unix and IP socket families; raw
packet capabilities are not granted. Any future licensed receiver needing a
different kernel interface requires a separate reviewed unit and threat-model
change.

## Health record contract

Each service atomically publishes a small JSON file with schema version, service identity, build version, health/readiness flags, configuration SHA-256, mode, and process-monotonic observation time. Clock quality adds state, hardware timestamp availability, and signed PTP offset. Missing, malformed, stale, wrong-mode, or non-ready records fail closed. Aggregate readiness requires every active service to report the same configuration hash. The schemas are `edge-health-v1.schema.json` and `edge-deployment-profile-v1.schema.json`.

## Profile intent

| Profile | Mode | PTP | Dedicated CPUs | Signed runtime config | Intended use |
|---|---|---:|---:|---:|---|
| development | simulation | no | no | no | workstation iteration |
| ci | simulation | no | no | no | hermetic validation |
| replay | simulation | no | no | no | deterministic replay/performance |
| paper | paper | yes | yes | yes | connected paper certification |
| staging | paper | yes | yes | yes | production-like non-live validation |
| production-disabled | simulation | yes | yes | yes | installed host burn-in with transmission disabled |

Staging and production-disabled keep `OPERATOR_REQUIRED` as the NIC. The validator rejects that placeholder until a separately reviewed site overlay is supplied; no such overlay is committed.
The resolved profile is installed as
`/etc/aegis-mx/deployment/active-profile.json`; its SHA-256 is propagated into
the rendered manifest. Captured and reviewed host facts are installed beside it
as `host-facts.json`, and every service revalidates both files before start.

## Deployment boundary still missing

The repository now builds and installs the process entry points and packages the
configuration verifier. Completion of a production deployment still depends on
a reviewed site overlay, production trust roots and signed configuration,
clock-source evidence, composed data/order services, and cold-start, watchdog,
disk-failure, shutdown and rollback qualification on target hardware. Licensed
native feed and order-entry adapters remain unavailable until licensed
specifications and certification environments exist. None of those gaps is
bypassed by these artifacts.
