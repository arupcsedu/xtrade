# Aegis-MX Production-Readiness Evidence Index

## Evidence rules

The audited implementation is commit
`bab2be626da56acfb2c217ce84def51520758b56`. The subsequent audit commit
contains only these review records. Generated files under `build/` and `dist/`
are local evidence: their hashes identify this run, but they are not a substitute
for a signed immutable artifact store or production qualification.

All Python-backed repository commands used the required environment at
`/scratch/djy8hg/env/aegis_mx_contracts`. Deterministic behavioral tests used seed
`20260907`; the benchmark tool's manifest records its own fixed seed `20260906`.

## Remediation source evidence

| Evidence | Source | Relevant fact |
| --- | --- | --- |
| E-001 | [ADR 0042](../adr/0042-verified-authority-capabilities-and-mandatory-decision-audit.md) | Records the authenticated capability and pre-send audit decisions |
| E-002 | [Authentication API](../../cpp/common/include/aegis/common/authentication.hpp) | Bounded SHA-256/HMAC-SHA-256 and Ed25519 primitives |
| E-003 | [Live capability](../../cpp/execution/include/aegis/execution/live_capability.hpp) | Exact-frame evidence, final-state issuer, private move-only verified type, expiry and replay cache |
| E-004 | [Gateway interface](../../cpp/execution/include/aegis/execution/interfaces.hpp) | Non-virtual future-live send wrapper rechecks the frame and consumes verified authority before its protected implementation hook; no frame-only overload exists |
| E-005 | [Live capability tests](../../cpp/execution/tests/live_capability_test.cpp) | Exact binding, unsafe-state issuance, move/copy semantics, send-boundary consumption, replay, tamper, wrong trust, expiry, absent evidence and capacity rejection |
| E-006 | [HA coordinator contract](../../cpp/high_availability/include/aegis/high_availability/coordinator.hpp) | Coordinator accepts verifier-created signed grant type only |
| E-007 | [HA adversarial tests](../../cpp/high_availability/tests/coordinator_test.cpp) | Self-rehashed forgery, tamper, wrong root, lease and replay/rollback rejection |
| E-008 | [Decision explanation contract](../../cpp/observability/include/aegis/observability/decision_explanation.hpp) | Canonical full-field codec and mandatory bounded journal publisher |
| E-009 | [Decision explanation tests](../../cpp/observability/tests/decision_explanation_test.cpp) | Codec corruption/truncation and journal saturation fail-closed regression |
| E-010 | [PAPER acceptance path](../../cpp/integration/src/paper_acceptance.cpp) | Mandatory durable publication occurs before gateway submission; segment is reopened and decoded |
| E-011 | [Edge service targets](../../cpp/edge_services/CMakeLists.txt) | Five non-live process entry points are built and installed |
| E-012 | [Config verifier](../../control/cmd/edge-config-verify/main.go) | Strict bounded Ed25519 signed-config preflight with trust, validity, digest, profile, environment and mode checks |
| E-013 | [Edge deployment validator](../../tools/edge_deployment.py) | Checks repository executable contracts and reports missing binaries |

## Finding-to-test evidence

| Finding | Regression or integration proof | Result |
| --- | --- | --- |
| PRD-C001 | `LiveTransmissionCapabilityTest.*`; RFC crypto vectors in `AuthenticationTest.*` | PASS within 18/18 C++ suites and UBSan, including unsafe issuance and one-shot adapter consumption |
| PRD-C002 | `DecisionExplanationTest.MandatoryJournalOverflowFailsClosed`; PAPER integration durable-open scan and pre-send ordering contract | PASS; 16/16 PAPER scenarios |
| PRD-C003 | `HaCoordinatorTest.RejectsSelfAssertedGrantWithRecomputedPublicHash` plus signature/trust/lease/sequence tests | PASS within C++ and UBSan suites |
| PRD-B002 partial | Python edge deployment tests, missing-config CTest, package/rollback verification | PASS; no missing repository executables |
| PRD-B005 partial | Canonical codec, mandatory queue saturation, persistent reopen/checksum/decode, journal corruption/truncation tests | PASS; broader recovery blocker remains open |

The [remediation record](blocker-remediation-2026-09-07.md) documents the
pre-fix failures and root causes. Git history preserves the prior failing design
at `45058a03e078ed26f3c26005bf1fdb40967c7ffe`, the initial remediation at
`30d4e17e72565bde34ec1b045730f6e9b83b5261`, and the fresh-audit live-boundary
hardening at `bab2be626da56acfb2c217ce84def51520758b56`.

## Exact-commit validation

| ID | Command | Result | Artifact SHA-256 |
| --- | --- | --- | --- |
| E-100 | `tools/run.sh lint` on Slurm `parallel`, followed by final-delta lint | PASS: full-tree 70-file formatting, Ruff, strict mypy over 65 files, Go vet, and all 268 clang-tidy/`-Werror` targets at the initial remediation; final commit repeated formatter/Ruff/mypy/Go vet and the affected execution clang-tidy/`-Werror` target. ShellCheck unavailable locally and remains CI-enforced | Console evidence |
| E-101 | `AEGIS_TEST_SEED=20260907 tools/run.sh test` | PASS: 18/18 C++; 459/459 Python; Go packages; Python 7,585 statements/1,870 branches at 100% | C++ `0558d85b6135865be93e322d8eca114193c8b3b3bc269d72e2a107b1e043915f`; Python JUnit `74dd1a9208332112f6da4910562b313aa246d446662e8b1eb8b028a2ceea6f9f`; coverage `3c800467e81138f99c18cc07c33304b0eb498a0c4bc79ad611a6869c3aa774f8`; Go coverage `785b5199591dee576a7c1f0fdfebb6d580d35937e8df1a81193f1d95076fa78e` |
| E-102 | `go test -race ./...` | PASS: config and registry; command packages disclose no direct tests | Console evidence |
| E-103 | UBSan configure/build/CTest | PASS: 220 build targets; 18/18 suites, repeated for the final affected target | `6aa08a2398046c2d06d80fcdade3283a4151b53c71ae23653647f5b885c6b2ca` |
| E-104 | ASan configure/build/CTest | ENVIRONMENT FAILURE: full build completed at the initial remediation and the exact final affected target rebuilt; test code cannot start because the host refuses a 15.4 TiB shadow mapping | final affected-target failed JUnit `1aac610391546e394f2527f18e0b89e127dda531c85d0bec224691b3f77d8914` |
| E-105 | TSan configure/build/CTest | ENVIRONMENT FAILURE: full build completed at the initial remediation and the exact final affected target rebuilt; test code cannot start because `setrlimit` cannot establish the required address space | final affected-target failed JUnit `9b40bf9a8ea3ace617517c18260d40572d3d3a2e7abb379e782de2790985afa5` |
| E-106 | `AEGIS_FUZZ_RUNS=10000 AEGIS_TEST_SEED=20260907 tools/run.sh test-fuzz` | PASS: six parsers/state machines, 60,000 executions | Console evidence |
| E-107 | `tools/run.sh schemas-check` | PASS: FlatBuffers v1 through v1.8, 195 generated files and six golden files | Console evidence |
| E-108 | `tools/run.sh security-test` | PASS: 22 pinned actions, three hardened deployments, 61 SBOM components and 20 adversarial tests | SBOM `0f7f4a2a1c20fe11da0e9f3095a6f6a537f66b158aac2e21902051fe7b992a9a`; regional report `54b5b068135c768186822dd6ca9ab300e369f9dccb5769768761e953c17718f3` |
| E-109 | `tools/run.sh dependency-scan` | PASS: no known Python vulnerability, no called Go vulnerability, reviewed CSI references, no secret finding | Console evidence |
| E-110 | `tools/run.sh edge-validate` | PASS: 18 assets, six profiles, five services, no missing repository executable, live false and automatic activation false | Asset digest `591b7551bb66b86e863d76cdff62c651c72f77498db70af11243c8685e371739` |
| E-111 | `tools/run.sh regional-validate` | PASS as fail-closed reference: nine deployments, five namespaces, no edge workload, live false, ten placeholder images, `production_apply_ready=false` | Asset digest `374c5b96ce925c59ede88a2b5bcde4076feebabb74ddb44812d4f59635e3d657` |
| E-112 | `AEGIS_TEST_SEED=20260907 tools/run.sh chaos-fast` | PASS: 23/23 deterministic single-fault attempts | `92896b53b7284a171f1933842480d5900e60b26b772ae3edfe04722a1af631b3` |
| E-113 | `AEGIS_TEST_SEED=20260907 tools/run.sh chaos-nightly` | PASS: 23,000 single-fault and 5,000 combined-fault attempts; zero failures | `b03bcf512c3f607fbab0758fe09e0f20f02c5354aa6924a370cd26a7d1b5d7bf` |
| E-114 | `AEGIS_TEST_SEED=20260907 tools/run.sh paper-integration` | PASS: 16/16; PAPER; live compiled false; durable explanation and audit checks true | JSON `109de8b6966e4b059cf1104a99ea43086e459e5fe3bf0092c277f323bf49611b`; report `2685e96b2a09b2b8b389e6909398d1e0a7eb64cf014d4dc9e0e67945ebef8406` |
| E-115 | `tools/run.sh benchmark-platform-smoke` | PASS as smoke: 126 cases, no unavailable cases; 32 samples and eight warmups; explicitly `qualified=false` | report `bd7520d0a5612699b018e15b2cc03e7365ce3bc0ed48b68682f65bf51262037d`; samples `146e74961cc4ae5ced789e0225f2801d12afebcf83d4274d54dd7cf6b028e45f` |
| E-116 | `tools/run.sh package` | PASS: CPack, wheel/sdist, three Go executables, SBOM and six verified rollback packages | SBOM `0f7f4a2a...92a9`; package hashes below |
| E-117 | `tools/run.sh reproducibility-check` | PASS: all 14 artifacts matched across two builds | Console evidence |
| E-118 | `tools/run.sh docs-check` | PASS: 153 documents, 254 relative links and 20 Mermaid sources | Console evidence |
| E-119 | CMake Graphviz plus `tsort` | PASS: 69 nodes, 254 edges, acyclic | Mutable `build/reports/audit/cmake-targets.dot` |
| E-120 | `git diff --check` | PASS before implementation commit and before evidence commit | Console evidence |

## Final non-live operational readiness evidence

The Prompt 43 drill evidence binds source revision
`b04c44cb520f70c82120aef8da7ed80a8eba7864`. Its pass status is limited to
repository-owned synthetic/PAPER operation and explicitly preserves the current
`STOP / NO-GO` production decision.

| ID | Evidence | Result | Artifact SHA-256 |
| --- | --- | --- | --- |
| E-121 | [Operator simulation report](../testing/evidence/operator-readiness-20260908/operator-simulation.json) | PASS: PAPER, live compilation false, no production activation attempt, 4/4 kill scopes | `7cafbc4692796a4d0ada26a408257e14291550141f1bfbfcae8ad27e40446007` |
| E-122 | [Operator audit extract](../testing/evidence/operator-readiness-20260908/operator-audit.ndjson) | PASS: 21 SHA-256-linked records and nine ordered risk decisions | `a167c57693edc5cbd815d814977d79920a573ed371d561ffe6cfacfed4eb2d30` |
| E-123 | [Live-mode-disabled evidence](../testing/evidence/operator-readiness-20260908/live-mode-disabled.json) | PASS for all eight non-live controls; `production_ready=false`, activation `PROHIBITED`, five blockers retained | `4834c7bcdee27f75be46865c499e7e935040e60e76b1a0e9deb4a7507172ae89` |
| E-124 | [Prompt 43 validation summary](../testing/evidence/operator-readiness-20260908/validation-summary.md) | Test, sanitizer, documentation, security and applicability results with limitations | See linked record |

## Package identities

| Artifact | SHA-256 |
| --- | --- |
| `dist/cpp/aegis-mx-foundation-0.1.0-Linux-x86_64.tar.gz` | `ed74ab56ba56039c6f719642d551978a72a955c1c4e07986fbac2b89be9572bb` |
| Python wheel | `3399ff3e25c9bdf3e5f00bd74724223858dfd142ccdba6b1c7dd7f3ca3390a61` |
| Python sdist | `eb2c1c2ec99e7bd148011d2e0c613f408f5c3b9540604369b87e11788ef53c67` |
| `aegis-edge-config-verify` | `bc7efc8af50e26992cddcfe5f6093c0e32a9c73576bb363ff2e62d89dbfa11b7` |
| `config-service` | `4f18a534467404f89f9e4511a7800d8207d25c46343b9786e48f1c004f871238` |
| `model-registry` | `553d6eedaa8c5f87ba28ab6d014ca2e9cfdefead9a4cef1dfd0c940446563ffd` |
| CycloneDX SBOM | `0f7f4a2a1c20fe11da0e9f3095a6f6a537f66b158aac2e21902051fe7b992a9a` |

Rollback-package hashes are reported by the package command as development
`6f72e3ee088ff98daf6b9bde0e25ef0a962178e24b28844b2329701bd7818158`, CI
`0d42fda19100eb40a0a48329aabe8a0fb32760b47057a869db97fd71175c0700`, replay
`af869d6a57af4f78d7117c2f4dc72d9bb0f600fdca7c16d012ac1554702fb703`, PAPER
`43480c7613dea2df1c85ce8653c9841ad2df7839d97d5eaf030cc2282b2780d1`, staging
`daa2e3de3f3a66da50cdbd01a0a0a249427b63e1914c59ddd0024741bb15f502`, and
production-disabled
`e3cba2c8e5eb8f1aeed42ea42eb8d0d436813e9a46881a277472adc4ce7cf79b`;
every package contained 36 verified files.

## Benchmark smoke observations

These are diagnostic synthetic measurements on an Intel Xeon Gold 6248, pinned
to CPU 0. They are not production thresholds or real-NIC evidence.

| Ordinary-load stage | p50 ns | p99 ns | p99.9 ns | max ns |
| --- | ---: | ---: | ---: | ---: |
| Packet to normalized | 848 | 875 | 875 | 2,149 |
| Normalized to book | 185 | 188 | 188 | 218 |
| Book to features | 234 | 242 | 242 | 296 |
| Features to micro forecast | 88 | 97 | 97 | 130 |
| Forecasts to ensemble | 2,947 | 2,973 | 2,973 | 2,992 |
| Ensemble to risk | 1,124 | 1,204 | 1,204 | 1,215 |
| Risk to gateway enqueue | 2,134 | 2,319 | 2,319 | 14,156 |
| Tick to intent | 11,269 | 11,588 | 11,588 | 11,743 |
| Tick to PAPER send | 201,801 | 280,894 | 280,894 | 290,647 |

With only 32 samples, p99 and p99.9 are not statistically meaningful; the tool
correctly rejects qualification.

## Evidence limitations

- No licensed specification, endpoint, credential, certification session,
  drop-copy feed, real NIC capture, production CA/KMS/HSM, independent witness,
  physical gateway fence, production cluster, or target edge host was available.
- ASan and TSan did not execute test code because of the host address-space
  policy. Their JUnit failures are retained and PRD-H001 remains open.
- Chaos injects deterministic component faults, not kernel/NIC/disk/network
  failures into independently deployed production services.
- Full fresh-process reconstruction from the authoritative journal plus external
  reconciliation remains unproven.
- Passing tests prove only their declared synthetic/PAPER contracts. They confer
  no economic claim, legal authorization, or live-trading approval.
