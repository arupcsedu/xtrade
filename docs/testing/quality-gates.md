# Aegis-MX Quality Gates

| Field | Value |
| --- | --- |
| Status | Normative |
| Effective date | 2026-08-28 |
| Applies to | Every change and implementation phase |

## Objective

Quality gates provide reproducible evidence that a change satisfies the
[engineering contract](../architecture/engineering-contract.md). Passing a gate
means the required command completed successfully and its output was reviewed;
it does not mean a command was merely invoked. Gates are cumulative and fail
closed. A missing tool, test environment, fixture, or result is a reported
blocker or limitation, not a pass.

No failing test may be deleted, skipped, quarantined, or weakened solely to make
a gate pass. No threshold may be silently relaxed. An intentional test or
threshold change requires the associated requirement change, rationale, review,
and before/after evidence.

## Per-phase workflow

### 1. Inspect

Before editing, inspect all repository-level instructions and all relevant
documentation, accepted ADRs, schemas, tests, source, build files, deployment
artifacts, and recent local changes. Record conflicts and unresolved
assumptions. Preserve user changes and backward compatibility.

### 2. Plan

Write or update a short implementation plan containing:

- the user-visible and safety-relevant outcome;
- in-scope and out-of-scope behavior;
- affected contracts and compatibility implications;
- tests to add or update;
- applicable formatter, linter, test, sanitizer, and benchmark gates; and
- rollback or migration needs.

A nontrivial design decision requires an ADR before or with implementation.

### 3. Specify and test

Define acceptance and failure behavior before or alongside production code.
Tests must include the smallest complete success path and relevant fail-closed
paths. Tests shall assert machine-readable outcomes, not rely only on logs.

### 4. Implement the vertical slice

Implement the least scope that provides complete behavior. Stubs, TODOs, and
interfaces without their required core behavior do not satisfy this gate.
Simulation and paper paths precede any live-capable behavior.

### 5. Verify

Run the applicable commands from a clean, reproducible build environment. The
project shall eventually provide checked-in wrapper scripts or documented
presets so local and CI commands match. Until build tooling exists, a phase
report must state each unavailable gate explicitly.

### 6. Report

Record files changed, ADRs or architectural decisions, exact commands and output
summaries, deterministic seeds, benchmark environment/results, known
limitations, and the next dependency. Do not claim completion without this
evidence.

## Required gate matrix

| Change type | Format/lint | Unit | Integration | Sanitizers | Benchmarks | Replay/determinism | Security/safety |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Documentation only | Markdown/style/link checks when configured | N/A | N/A | N/A | N/A | N/A | Contract consistency and secret scan |
| C++ non-hot-path | Required | Required | As affected | ASan, UBSan; TSan where concurrent | If performance-sensitive | If decision-affecting | Required for trust-boundary changes |
| C++ hot path | Required | Required | Required | ASan, UBSan, TSan in suitable builds | Required | Required | Required |
| Python research/tooling | Formatter, linter, strict types | pytest | As affected | Dependency/security scan | If performance-sensitive | Fixed seeds and reproducible artifacts | Required for untrusted input |
| Model/feature change | Formatter, linter, strict types | Required | Contract and replay tests | As runtime permits | Inference latency/capacity | Dataset/model/config hashes and fixed seeds | Leakage, robustness, deadline, malformed-output tests |
| Control plane | Required | Required | Authn/authz/config/failure integration | Language-appropriate race/sanitizer tools | Capacity if changed | Idempotency and configuration-hash tests | Required |
| Gateway/risk/live safety | Required | Required | Required, including fault injection | All applicable | Required | Full decision and activation replay | Independent negative safety suite required |
| Schema/storage migration | Required | Required | Upgrade, downgrade/rollback, mixed-version | Parser/fuzzer where applicable | Throughput/capacity if relevant | Old and new artifact replay | Corruption, truncation, access-control tests |

`N/A` must be justified by change type. “Not configured” is different from
`N/A` and must be reported as a limitation.

## Core test requirements

### Determinism and replay

- Tests use explicit, recorded seeds. The default project seed, once chosen,
  shall be a checked-in constant; randomized CI runs record the generated seed.
- Time is injected. Tests do not depend on wall-clock sleeps for correctness.
- Canonical replay covers source ordering, feature identity, model versions and
  deadlines, ensemble state, configuration hash, risk snapshot, rejection
  reasons, and resulting action.
- Duplicate, missing, reordered, malformed, and late inputs have deterministic
  outcomes.
- Replay compares canonical serialized results or explicitly normalized records,
  never unstable log text.
- Research and counterfactual replay are clearly marked and cannot invoke venue
  transmission.

### Numeric and schema correctness

- Execution tests use integer price ticks and integer quantities, including
  boundary, overflow, underflow, sign, scale, and invalid-sentinel cases.
- Floating-point values are rejected at execution contract boundaries.
- Parsers test truncation, corruption, unknown versions, unsupported enum values,
  oversized fields, checksums, and compatibility behavior.
- Property-based tests and fuzzers are expected for parsers, state machines,
  book builders, serialization, and numeric conversion where appropriate.

### Safety behavior

Negative tests independently remove every live-transmission precondition and
prove that transmission is impossible. At minimum, test:

- live support absent at compile time;
- unsigned, invalidly signed, expired, replayed, or wrong-scope configuration;
- missing, expired, revoked, or wrong-scope operator authorization;
- unhealthy, unavailable, stale, or disagreeing risk state;
- unsynchronized, uncertain, or regressed clocks;
- stale data, sequence gaps, malformed data, invalid/crossed books, and trading
  halts;
- split brain, leadership ambiguity, partition, restart, and dependency loss;
- local and remote kill switches;
- absent or non-durable activation audit record;
- malformed, mismatched, or late forecast; and
- queue saturation, capacity exhaustion, and shutdown races.

Tests also prove that models cannot address a gateway and that all order intents
pass the same deterministic risk boundary. Safety tests must observe the final
transmission boundary, not only an upstream decision.

### Untrusted text

News and filing tests treat payloads as adversarial. The suite includes prompt
injection, nested instructions, encoded instructions, malicious links, oversized
documents, malformed encodings, schema-breaking output, data-exfiltration
requests, and attempts to invoke tools or change configuration. Passing behavior
is constrained structured data or explicit rejection; external text never gains
control authority.

### Concurrency and fault containment

- Concurrent components test races, memory ordering, queue wraparound, producer
  and consumer failure, cancellation, shutdown, overload, and restart.
- Bounded structures prove capacity behavior. Tests must not accept unbounded
  allocation or retry loops.
- Fault-injection tests cover partial writes, corrupt journal tails, disk-full
  conditions, exporter outage, process death, network partition, and dependency
  recovery.
- Recovery requires fresh safety validation and cannot restore stale live
  authority.

## C++ gates

C++ uses C++20 or newer. The repository shall standardize CMake presets for at
least development, release, ASan/UBSan, and TSan configurations. Once present,
the canonical gates are:

- configured formatter check (normally `clang-format`);
- `clang-tidy` with warnings treated according to checked-in policy;
- compile with the project warning set and no unreviewed warnings;
- unit and integration tests through CTest;
- AddressSanitizer and UndefinedBehaviorSanitizer test runs;
- ThreadSanitizer for concurrent code in a separate compatible build;
- fuzz tests for exposed binary/text parsers where applicable; and
- benchmark runs for hot-path or performance-sensitive changes.

Sanitizer builds shall use supported combinations rather than claiming coverage
from an incompatible all-in-one build. Release-only behavior and architecture-
specific optimizations need tests in a representative release build.

## Python gates

Python tooling shall be pinned and invoked through repository-owned commands.
Required gates are:

- configured formatter check;
- configured linter;
- strict static type checking;
- `pytest` unit and affected integration tests;
- schema validation and malformed-input tests;
- dependency vulnerability/license checks according to compliance policy; and
- reproducibility evidence for training or evaluation artifacts.

Training/evaluation records include code revision, environment/dependency lock,
dataset identity and provenance, split definition, preprocessing and feature
versions, all random seeds, model artifact hash, configuration hash, and metrics.

## Performance gates

Benchmarks are tests, not marketing measurements. Hot-path changes require a
benchmark with:

- a checked-in workload and fixed seed;
- release build flags and compiler identity;
- CPU model, microcode, topology, affinity, frequency/turbo policy, NUMA policy,
  memory, kernel, and relevant clock configuration;
- warm-up policy and sample count;
- throughput plus p50, p95, p99, and maximum or p99.9 latency as appropriate;
- allocation count, queue occupancy, rejection/drop counts, and correctness
  checks; and
- baseline comparison with raw output retained as a CI artifact.

Latency thresholds and regression tolerances must be established by an ADR and
capacity evidence before they are enforced. They may not be silently weakened.
A statistically or operationally meaningful regression blocks the change unless
explicitly accepted with rationale. A fast incorrect result is a failure.

Hot-path benchmarks must demonstrate that the measured path performs no dynamic
allocation, blocking disk I/O, network RPC, Python or LLM call, distributed
database operation, or garbage-collected work. Benchmark instrumentation shall
be accounted for and shall not obscure queue or overload behavior.

## Review and evidence requirements

Every phase report shall use this minimum structure:

```text
Scope:
Files changed:
ADRs / architectural decisions:
Compatibility or migration:
Commands and output summaries:
Deterministic seeds:
Benchmarks and environment:
Known limitations:
Next dependency:
```

For each command, record the exact invocation, exit status, number of tests run,
failures/skips, and a concise result. Preserve full logs as CI artifacts when CI
exists. Redact secrets without removing diagnostically necessary reason codes.

## Gate exceptions

There is no emergency exception that enables live transmission or bypasses a
safety invariant. A temporarily unavailable non-safety gate may be documented as
a limitation only when the delivered scope cannot exercise the affected
behavior, the omission has an owner and follow-up condition, and completion is
not overstated. Code that depends on an unavailable required safety gate remains
incomplete and non-live-capable.

## Documentation-only baseline

For the initial documentation-only phase, no build system, formatter, linter,
test suite, sanitizer target, benchmark target, or Git metadata exists. The
applicable checks are file inventory, required-section/content review, internal
link validation, whitespace/encoding inspection, and secret-pattern review.
This statement records current capability; it does not waive gates for future
code.
