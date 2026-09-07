# Aegis-MX Production-Readiness Audit — Fresh Remediation Review

| Field | Value |
| --- | --- |
| Audit date | 2026-09-07 |
| Decision | **NO-GO — NOT PRODUCTION READY** |
| Audited implementation | `bab2be626da56acfb2c217ce84def51520758b56` |
| Environment | Shared compute host; synthetic/PAPER only |
| Deterministic test seed | `20260907` |
| Governing contract | [Engineering contract](../architecture/engineering-contract.md) |
| Remediation detail | [Blocker remediation record](blocker-remediation-2026-09-07.md) |
| Blocking detail | [Blockers](blockers.md) |
| Complete register | [Risk register](risk-register.md) |
| Evidence map | [Evidence index](evidence-index.md) |

## Executive verdict

This is a fresh hostile audit after the Prompt 41 remediation. The three
repository-owned CRITICAL defects from the 2026-09-06 audit are closed with typed
security boundaries and regression tests. No CRITICAL finding remains open.

Five BLOCKERs remain: target-site edge qualification, licensed integrations,
independent witness and physical gateway fencing, whole-pipeline durable recovery,
and operable production regional/control infrastructure. They depend on authorized
specifications or production systems that are not present in this repository.
Production promotion therefore remains prohibited. The build remains non-live by
default and contains no real-money transmitter.

## Audit method

The review repeated source-to-sink tracing for risk, OMS, routing, gateway,
decision evidence, leadership authority, configuration, clock/data/market safety,
and recovery. It inspected the complete working tree, generated and checked the
CMake dependency graph, searched for live defaults and bypasses, and reran the
unit, integration, replay, chaos, security, sanitizer, package,
reproducibility, documentation, and benchmark gates listed in the
[evidence index](evidence-index.md).

## Finding summary

| ID | Severity | Finding | Fresh status |
| --- | --- | --- | --- |
| PRD-B001 | BLOCKER | Candidate was not an immutable revision | **Closed** by the remediation and evidence commits |
| PRD-B002 | BLOCKER | Colocated edge was non-runnable | **Partial / Open**: binaries and verifier now build; target-site composition and qualification remain absent |
| PRD-B003 | BLOCKER | Licensed market/provider integrations absent | **Open — external** |
| PRD-B004 | BLOCKER | Production witness and physical session fencing absent | **Open — external** |
| PRD-B005 | BLOCKER | Durable whole-pipeline evidence/recovery incomplete | **Partial / Open**: mandatory explanations are durable before send; complete fresh-process recovery is absent |
| PRD-B006 | BLOCKER | Regional/control production plane non-runnable | **Open — external/platform** |
| PRD-C001 | CRITICAL | Frame-only future live-transmission boundary | **Closed** |
| PRD-C002 | CRITICAL | Lossy explanation publication occurred after send | **Closed** |
| PRD-C003 | CRITICAL | Caller-asserted HA grants | **Closed** |
| PRD-H001 | HIGH | ASan/TSan runtime evidence unavailable on this host | Open |
| PRD-H002 | HIGH | Target-host performance and soak unqualified | Open |
| PRD-H003 | HIGH | Dependency/secret gate failed | **Closed** |
| PRD-H004–H008 | HIGH | Provider-neutral gateway, PIT, sandbox, telemetry, full recovery gaps | Open |
| PRD-M001–M004 | MEDIUM | Governance, coverage, dependency-policy, and report-path gaps | Open |

## Closed CRITICAL findings

### PRD-C001 — Verified live-transmission capability

The live adapter can no longer accept a frame alone. Its public non-virtual send
boundary requires a move-only `VerifiedLiveTransmissionCapability`, whose
constructor is private to the final verifier. The HMAC-SHA-256 capability binds
the exact frame SHA-256, command and risk hashes, safety state, full configuration
digest, operator authorization, activation record and journal sequence, signed
fencing evidence, session, epoch, token, key identity, issuance, and expiry. The
issuer rejects unsafe final clock, feed, book, market, risk, kill-switch,
configuration, operator, activation, session, epoch, or fencing state.
Verification is bounded, allocation-free, constant-time for authentication, and
consumes the authenticated ID once. The adapter then rechecks the exact frame and
irreversibly consumes the verified wrapper before the protected implementation
hook can run.

Regression tests reject altered frames and fields, wrong trust, expired
capabilities, replay, absent authority evidence, unsafe issuance, attempted
copy/reuse, frame rebinding, and capacity exhaustion. No live adapter
implementation or enabled live build was introduced.

### PRD-C002 — Mandatory durable evidence before emission

Decision explanations now have a canonical, full-field, range-checked
little-endian representation. The mandatory publisher submits them to the bounded
`AsyncJournal` at mandatory priority. The composed PAPER path returns fail-closed
unless that publication is accepted, and performs this check before
`send_order`. It then reopens the segment, verifies framing/checksums, decodes each
record, and includes the journal SHA-256 in the acceptance audit hash.

Regression tests cover serialization corruption and truncation, queue saturation,
stopped journal, persistent failure/inhibit behavior, ordering, and durable scan.
This closes the original order-without-explanation defect, but not PRD-B005's
broader requirement to persist and reconstruct every pipeline stage.

### PRD-C003 — Cryptographically verified fencing grants

Fencing grants now carry Ed25519 signature, witness key identity, and trust-root
identity. Only `FencingGrantVerifier` can create `VerifiedFencingGrant`, and the
coordinator accepts only that type. Canonical signing covers all authority,
lease, epoch, sequence, quorum, and previous-owner-fenced fields. Negative tests
reject self-asserted rehashed grants, field tampering, wrong keys/trust roots,
expiry, and replay/rollback.

This closes the unsafe in-process authority boundary. PRD-B004 remains because
there is still no independent production witness, consensus deployment, key
custody, multi-host proof, or physical exchange-session fence.

## Remaining BLOCKERs

- **PRD-B002:** five non-live edge process entry points and a strict signed-config
  verifier now build and package. The checked-in production-disabled profile
  intentionally retains `OPERATOR_REQUIRED`; no target NIC/PTP/NUMA/NVMe host,
  complete daemon composition, signed site overlay, or operational qualification
  was available.
- **PRD-B003:** no licensed venue, reference-data, broker, drop-copy, order-entry,
  or news-provider specification and no certification environment were supplied.
- **PRD-B004:** repository cryptography cannot substitute for an independent
  witness, physical old-owner fencing, or certified session takeover.
- **PRD-B005:** mandatory decision evidence is durable before PAPER send, but all
  required records are not yet composed through one authoritative journal and a
  killed fresh process is not reconstructed solely from verified segments plus
  drop-copy/open-order reconciliation.
- **PRD-B006:** Kubernetes overlays deliberately retain zero image digests and
  `registry.invalid`; production IAM, CA, KMS/HSM, CSI, admission, data stores,
  distributor, images, and cluster drills are absent.

## Domain assessment

| Domain | Assessment | Principal evidence or gap |
| --- | --- | --- |
| Correctness/determinism | Strong reference evidence | 18 C++ suites, 459 Python tests, Go tests, replay/PAPER and fixed seeds pass |
| Risk, halt, data and clock safety | Strong synthetic/PAPER evidence | Fail-closed restrictions and 16 scenarios pass; licensed semantics absent |
| Audit/explainability | Critical path repaired | Mandatory durable explanation precedes send; whole-pipeline recovery remains open |
| HA | Safer reference boundary | Signed verifier-only grants; external witness and physical fence absent |
| Security | Partial | Dependency/secret gate and adversarial tests pass; production identities and kernel sandbox absent |
| Performance | Smoke only | 126 cases with 32 samples; report explicitly records `qualified=false` |
| Deployment | Non-live reference | Edge executables exist and assets validate; site and regional production systems absent |
| Sanitizers | Partial | UBSan and Go race pass; ASan/TSan cannot map shadow memory on this host |

## Fresh validation outcome

- PASS: formatter, Ruff, strict mypy, Go vet, C++ `-Werror`/clang-tidy build,
  18/18 C++ suites, 459/459 Python tests with 100% measured coverage, Go tests,
  UBSan, Go race, 60,000 fuzz executions, schemas/golden files, docs/Mermaid,
  security policy, dependency/secret scan, packaging, and reproducibility.
- PASS within declared scope: 16/16 PAPER scenarios, 23/23 fast chaos scenarios,
  28,000 nightly chaos attempts, non-live edge/regional validation, and benchmark
  smoke.
- NOT RUNNABLE on this host: ASan and TSan executables build, but Linux refuses
  their required 15.4 TiB/32 TiB shadow mappings. This remains PRD-H001.
- UNQUALIFIED: benchmark smoke uses 32 samples/case and simulated inputs. It is
  neither target-hardware nor real-NIC evidence and leaves PRD-H002 open.

## Decision

The remediation materially strengthens safety and closes every reproducible
CRITICAL code defect from the prior audit. It does not close every BLOCKER because
doing so would require fabricating licensed protocols or claiming absent
production infrastructure. The authoritative decision is **NO-GO** until the five
open blockers and all designated release gates have immutable production evidence
and another independent hostile audit passes.
