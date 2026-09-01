# ADR-0003: Monorepo Build and CI Foundation

| Field | Value |
| --- | --- |
| Status | Accepted |
| Date | 2026-08-28 |
| Deciders | Aegis-MX principal engineering baseline |

## Context

The repository audit found no version control, source layout, build system,
language environment, dependency lock, tests, CI, developer commands, container,
security scan, artifact provenance, or SBOM. ADR-0002 defines logical component
boundaries but not their physical language layout or toolchains.

Phase 1 requires a minimal complete build/test vertical slice without adding
market-data, model, risk, OMS, execution, gateway, or live-trading behavior.

## Decision

### Physical layout

Use language-oriented top-level roots: `cpp/`, `python/`, and `control/`, plus
`schemas/`, `infra/`, `docs/`, and `tools/`. Logical boundaries from ADR-0002
remain authoritative. The physical mapping is documented in
[`../architecture/component-boundaries.md`](../architecture/component-boundaries.md).

Empty component directories are retained with `.gitkeep`; they are not
implementations and cannot satisfy a later phase.

### C++

- Require CMake 3.28+, Ninja, and C++20 without compiler extensions.
- Use target-scoped warnings, sanitizer flags, and clang-tidy; never apply
  project warning policy to fetched third-party targets.
- Treat warnings as errors in CI presets and use Clang for the clang-tidy lint
  build so the analyzer and compilation driver share one flag vocabulary.
- Use distinct ASan, UBSan, and TSan presets and jobs.
- Use GoogleTest 1.17.0 and Google Benchmark 1.9.5 release archives pinned by
  SHA-256 through CMake FetchContent.
- Generate deterministic build metadata containing source revision, project
  version, compiler identity, build type, and `live_trading_capable=false`.
  Exclude wall-clock time, host, user, absolute paths, and environment secrets.

### Python

- Support Python 3.12.12 in a repository-local `.venv`.
- Use Ruff for formatting/linting, mypy strict mode, pytest, pytest-cov with a
  100% foundation coverage threshold, pip-audit, and detect-secrets.
- Maintain exact direct requirements and a fully transitive hash-checked pip
  lock. Package with Hatchling and `python -m build --no-isolation` so packaging
  cannot resolve undeclared dependencies.
- Ignore ambient user pip configuration during repository tasks; use one
  explicitly selected package index or an operator-supplied controlled mirror.

### Control plane

- Select Go for the initial control-plane foundation, pinned to toolchain
  1.26.4.
- Use gofmt, go vet, standard-library tests, and the Go race detector.
- Introduce no network service or external Go module in this phase.

### Developer and CI workflow

- Use GNU Make as the discoverable task interface and one strict Bash dispatcher
  as the implementation, providing `bootstrap`, `format`, `lint`, `test`,
  `test-sanitizers`, `benchmark`, `package`, and `docs-check`, plus `fast` and
  `full` aggregate gates.
- Auto-load exact UVA modules only when local system tools do not meet minimums;
  never modify the caller's shell or install global packages.
- Provide a digest-pinned Ubuntu 24.04 development container and checksummed Go
  archive. Python/project dependencies remain lock-controlled.
- Use GitHub Actions with separate fast and exhaustive workflows on
  `ubuntu-24.04`, immutable action commit pins, least-privilege permissions,
  build caches, retained reports/artifacts, dependency/secret scans, and SBOM.
- Use the deterministic seed `20260828` across scripts and CI.

There is no live-trading CMake option, Go build tag, Python feature flag,
credential, venue endpoint, or order-transmission code. All three language smoke
tests assert the non-live foundation state.

## Consequences

### Positive

- A clean checkout has one bootstrap command and consistent local/CI gates.
- Compiler warnings, format, static analysis, strict types, sanitizers, races,
  coverage, benchmarks, dependencies, secrets, and packaging are independently
  reviewable.
- Fetched C++ and Python dependencies are version/integrity pinned.
- Build identity is reproducible and safe to expose without leaking host data.
- The language-root layout can grow without making each logical component a
  separate process.

### Costs and limitations

- Bootstrap requires network access for first-time C++ archives and Python
  wheels; subsequent CMake builds use populated FetchContent state.
- CI actions are commit-pinned and require deliberate dependency updates.
- Ubuntu apt repository snapshots are not pinned, so the base image and project
  dependencies are reproducible but native package resolution is not yet
  bit-for-bit reproducible. Tool versions are recorded in logs/build metadata.
- TSan support depends on the host kernel/runtime; an unsupported environment is
  a reported limitation, not a waived gate.
- The current SBOM covers selected build/runtime environments but is not a signed
  release attestation.

## Alternatives considered

### One build system for all languages

Rejected. Wrapping Python and Go internals in CMake would obscure their native
locks and quality tools. Make orchestrates stable language-native commands.

### Unpinned Git branches or package ranges

Rejected. Mutable dependencies prevent reproducible audit evidence and create
unreviewed supply-chain change.

### Container-only development

Rejected. Colocated/HPC development needs native toolchains and sanitizer access.
The container is a reproducible option, not the only interface.

### Implement empty service stubs in every directory

Rejected. Stub-only packages would imply progress and violate the engineering
contract. Only one tested foundation slice per language is introduced.

## Validation implications

Phase 1 completes only after `make format-check`, `make lint`, `make test`,
`make test-sanitizers`, `make benchmark`, `make dependency-scan`, `make package`,
and `make docs-check` have evidence or an explicitly reported environment
limitation. CI workflow syntax, clean bootstrap, report paths, SBOM content, lock
hash enforcement, and absence of live capability are also acceptance tests.
