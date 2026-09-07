# Aegis-MX Production Risk Register

## Severity policy

| Severity | Meaning |
| --- | --- |
| BLOCKER | Production eligibility cannot be evaluated or the system cannot be safely operated in its claimed role |
| CRITICAL | A plausible defect can bypass a primary safety, audit, or authority invariant |
| HIGH | A major safety, security, recovery, or qualification control is absent or unproven |
| MEDIUM | Material governance, evidence, maintainability, or defense-in-depth weakness |

## Fresh register

| ID | Severity | Risk/current control | Required treatment | Status |
| --- | --- | --- | --- | --- |
| PRD-B001 | BLOCKER | Prior audit covered a dirty tree | Immutable implementation and evidence commits; clean final tree | **Closed** |
| PRD-B002 | BLOCKER | Non-live edge shells/verifier now build and package; site remains a placeholder | Complete composition and target-host lifecycle/rollback qualification | **Partial / Open** |
| PRD-B003 | BLOCKER | Synthetic/reference protocols only | Licensed intake, implementation, conformance and certification | **Open — external** |
| PRD-B004 | BLOCKER | Signed verifier-only local grants; no witness or physical fence | Independent witness, key custody, physical stale-owner fence, multi-host partition proof | **Open — external** |
| PRD-B005 | BLOCKER | Decision explanation is mandatory and durable before PAPER send; other stage journals remain fragmented | Whole-pipeline canonical journal, power-loss recovery, fresh-process exact replay and external reconciliation | **Partial / Open** |
| PRD-B006 | BLOCKER | Non-live manifests disclose zero digests and invalid registry | Real signed images/services/distributor/IAM/KMS/storage; cluster outage and restore proof | **Open — external/platform** |
| PRD-C001 | CRITICAL | Live interface rejects unsafe issuance and requires a move-only, verifier-created exact-frame capability consumed at its non-virtual send boundary | Preserve socket-boundary enforcement through licensed implementation/certification | **Closed** |
| PRD-C002 | CRITICAL | Mandatory persistent explanation acceptance now precedes send | Preserve ordering and extend canonical journal coverage to every mandatory stage | **Closed** |
| PRD-C003 | CRITICAL | Coordinator now accepts only Ed25519-verified grant capability | Preserve verifier boundary when integrating production witness | **Closed** |
| PRD-H001 | HIGH | UBSan and Go race pass; host refuses ASan/TSan shadow mappings | Run complete suites on compatible clean CI host and retain reports | Open |
| PRD-H002 | HIGH | Smoke is deterministic and reports percentiles, but has 32 samples and simulated inputs | 10,000+ samples/case, target CPU/NUMA/kernel/NIC/NVMe/PTP, soak/fault and signed baselines | Open |
| PRD-H003 | HIGH | pip-audit has no known issue; govulncheck has no called issue; CSI references reviewed; secret scan exits zero | Keep zero-exit dependency/secret gate and triage new advisories | **Closed** |
| PRD-H004 | HIGH | Generic gateway consumes `synthetic::SyntheticEvent` | Introduce provider-neutral market-state contract before licensed adapter work | Open |
| PRD-H005 | HIGH | PIT leakage tests cover a bounded allocating Python reference only | Authoritative store/provider semantics, completeness, retention, correction and parity evidence | Open |
| PRD-H006 | HIGH | Content limits, sanitization, subprocess limits and prompt isolation pass | Production no-network namespace/seccomp equivalent and escape tests | Open |
| PRD-H007 | HIGH | Bounded metrics/log/trace references exist | Production exporter, durable store, signed alerts, outage/backpressure and operator drill evidence | Open |
| PRD-H008 | HIGH | Component replay and deterministic PAPER reruns pass | Fresh complete system reconstruction from authoritative persistent journal and external reconciliation | Open |
| PRD-M001 | MEDIUM | Fresh audit is current; older roadmap/audit snapshots remain historical prose | Regenerate repository inventory and machine-check roadmap status | Open |
| PRD-M002 | MEDIUM | Go packages pass; command packages lack direct measured coverage | Risk-based thresholds and CLI/service corruption, shutdown and recovery tests | Open |
| PRD-M003 | MEDIUM | Generated CMake graph is acyclic; boundary direction is not enforced | Add CI allow-list for dependency direction and provider imports | Open |
| PRD-M004 | MEDIUM | CTest JUnit is emitted below preset build directories | Align absolute report path/upload and test report discovery | Open |

## Preserved controls

- Live compilation and activation remain off; checked-in profiles are
  SIMULATION/PAPER or production-disabled.
- Risk and gateway checks remain local, deterministic, integer based, bounded,
  and fail closed.
- Invalid market state, halt, clock, feed/book, configuration, fencing, journal,
  or kill state prevents new orders.
- No model sends an order directly, and late/invalid/unprovenanced model output is
  rejected before ensemble/risk use.
- Mandatory decision evidence now precedes gateway emission; telemetry remains
  non-blocking and separate.
- Untrusted intelligence input is bounded, sanitized, schema validated, and does
  not alter system instructions.

Update a row only when immutable evidence meets its stated treatment. Difficulty
or external ownership does not lower severity.
