# Local Development

| Field | Value |
| --- | --- |
| Status | Canonical contracts and deterministic market-core workflow |
| Foundation test seed | `20260828` |
| Synthetic generator golden seed | `20260829` |
| Python | `3.12.12` |
| Go toolchain | `1.26.4` |
| CMake minimum | `3.28` |

## Safety scope

The implemented market-data behavior is limited to the repository-owned SMX/1
synthetic fixture protocol. It has no network listener, provider endpoint,
licensed exchange protocol, real order-entry transport, credential, or live
gateway. The local synthetic/paper gateway consumes only normalized in-memory
commands and synthetic market events. The default build omits its live-only
adapter boundary and reports no live capability. Enabling the compile boundary
does not add a transmitter or authorize live operation.

The [engineering contract](../architecture/engineering-contract.md) and
[quality gates](quality-gates.md) are normative.

## Native bootstrap

From a fresh checkout:

```bash
make bootstrap
```

This command:

1. loads the pinned UVA environment modules when the system toolchain is too
   old;
2. creates `.venv` with Python 3.12;
3. installs every Python tool from the hash-checked lock;
4. installs the local Python package without dependency resolution;
5. downloads the Go module set declared by `control/go.mod`; and
6. configures CMake preset `dev`, which downloads hash-verified GoogleTest and
   Google Benchmark archives.

It does not install global packages, request credentials, contact a venue, or
enable trading behavior. Re-running it is idempotent.

For canonical-schema development in this checkout, place the required isolated
environment inside `/scratch/djy8hg/env` rather than in the repository:

```bash
export AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts
make bootstrap
make schemas-check
```

`AEGIS_PYTHON_ENV` defaults to `.venv` for portable CI and other clones. It must
not point at a shared or pre-existing environment owned by another project.

Native prerequisites are listed in the [root README](../../README.md). On the
UVA module host, the effective toolchain is loaded by `tools/toolchain.sh`:

```text
gcc/14.2.0
llvm/21.1.8
cmake/3.28.1
ninja/1.13.1
go/1.26.4
```

## Fast validation

Run the pull-request gate with:

```bash
make fast
```

It is equivalent to:

```bash
make docs-check
make schemas-check
make lint
make test
make benchmark
```

`make lint` freshly configures CMake preset `ci`; that preset selects Clang and
enables warnings as errors and clang-tidy only for Aegis-MX targets. Third-party
sources are not weakened or rewritten to satisfy project lint policy.

## Exhaustive validation

Run the complete local gate with:

```bash
make full
```

It adds:

```bash
make test-sanitizers
make test-fuzz
make dependency-scan
make package
```

Sanitizers use separate builds and reports:

```bash
cmake --preset asan
cmake --build --preset asan --parallel
ctest --preset asan

cmake --preset ubsan
cmake --build --preset ubsan --parallel
ctest --preset ubsan

cmake --preset tsan
cmake --build --preset tsan --parallel
ctest --preset tsan

(cd control && go test -race ./...)
```

Combining ASan and TSan is rejected at CMake configuration because their
runtimes are incompatible. A missing sanitizer runtime is a reported limitation,
not a pass.

ASan and TSan also require very large virtual-address shadow mappings. A host
with a restrictive hard `ulimit -v` can compile the instrumented binaries but
cannot execute them. Such a host limitation must remain a failing local gate;
run the separate exhaustive CI jobs on a runner with an unrestricted virtual
address space to obtain sanitizer evidence.

## Individual language commands

### C++

```bash
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
```

Release benchmark smoke:

```bash
cmake --preset release
cmake --build --preset release --parallel
build/release/cpp/benchmarks/aegis_benchmark_smoke \
  --benchmark_min_time=0.01s \
  --benchmark_out=build/reports/benchmarks/smoke.json \
  --benchmark_out_format=json
```

The smoke benchmark measures build-info access, contract validation,
serialization, SHA-256, identifier rendering, bounded synthetic event
generation, packet encoding, timestamp calls, and duration arithmetic. The
feed benchmark additionally reports receive-to-normalized p50/p95/p99/p99.9,
throughput, and fail-closed receiver-overload counters. The synthetic generator
reports events per second; results remain host-specific and are not a trading-
performance acceptance threshold.

Focused journal tests and the read-only/copy-only tools can be run with:

```bash
build/dev/cpp/journal/aegis_journal_tests
build/dev/cpp/journal/journal-inspect JOURNAL
build/dev/cpp/journal/journal-verify JOURNAL
build/dev/cpp/journal/journal-repair-copy SOURCE NEW_EMPTY_TARGET
build/dev/cpp/journal/journal-export SOURCE export.jsonl
build/dev/cpp/journal/journal-replay SOURCE replay.ajrp
```

The exporter contains exact payload bytes and inherits the source journal's
data classification. Recovery behavior and operator prohibitions are in the
[journal recovery runbook](../operations/journal-recovery-runbook.md).

Focused deterministic replay tests and the offline orchestrator can be run
with:

```bash
build/dev/cpp/replay/aegis_replay_tests
build/dev/cpp/replay/aegis-replay --capture CAPTURE --speed maximum
```

Selection, fault syntax, fixed seeds, counterfactual restrictions, and the
benchmark command are documented in
[Deterministic Replay Testing](deterministic-replay-testing.md).

Focused event-level backtester tests, a deterministic report, and the release
benchmark can be run with:

```bash
export AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts
source tools/toolchain.sh
cmake --preset dev
cmake --build --preset dev --target aegis_backtesting_tests aegis_backtest_tool
ctest --test-dir build/dev -R '^aegis_backtesting_tests$' --output-on-failure
build/dev/cpp/backtesting/aegis-backtest \
  --synthetic-events 4096 --seed 20260831 --strategies 2 \
  --report build/reports/backtest.json
cmake --preset release
cmake --build --preset release --target aegis_benchmark_smoke
build/release/cpp/benchmarks/aegis_benchmark_smoke \
  --benchmark_filter=benchmark_event_backtester_synthetic
```

The tool accepts only synthetic generation or verified SMX capture input. It
does not provide bar-close fills, live transmission, or a licensed historical
data decoder. Model assumptions and metric interpretation are documented in
[Event Backtester Testing](event-backtester-testing.md) and the
[event backtester architecture](../architecture/event-backtester.md).

### Synthetic exchange

Direct tool and deterministic-seed commands are documented in
[Synthetic Exchange Testing](synthetic-exchange-testing.md).

Focused feed-handler tests and recovery semantics are documented in
[Feed-handler Testing](feed-handler-testing.md) and the
[Market-data Recovery Runbook](../operations/market-data-recovery-runbook.md).

### Python

```bash
.venv/bin/ruff format --check python tools
.venv/bin/ruff check python tools
.venv/bin/mypy python/intelligence python/model_serving python/tests tools
.venv/bin/pytest
```

Pytest enforces statement and branch coverage of at least 100% for the current
foundation package. Future thresholds cannot be weakened silently.

### Time-series forecast service

The off-hot-path reference roles require no GPU, checkpoint, secret, or live
connection:

```bash
/scratch/djy8hg/env/aegis_mx_contracts/bin/timeseries-context-builder --port 8082
/scratch/djy8hg/env/aegis_mx_contracts/bin/timeseries-forecast-worker --port 8081
/scratch/djy8hg/env/aegis_mx_contracts/bin/timeseries-forecast-api --port 8080
```

Each role exposes `/healthz`, `/readyz`, `/version`, `/configuration`, and
`/metrics`. Forecast and context endpoints accept only bounded canonical context
JSON produced by `ContextBuilder`; malformed or future-leaking input fails
closed. Complete contracts, tests, and limitations are in the
[service architecture](../architecture/timeseries-forecast-service.md).

Build the service-only image from the repository root:

```bash
docker build -f infra/timeseries_forecast/Dockerfile -t aegis-mx-timeseries:0.2.0 .
```

Before applying the Kubernetes manifest, replace its zero/placeholder image
digest with the approved image digest. The manifest does not deploy any
colocated market-data, risk, OMS, execution, or gateway component.

### News and filing intelligence

Run the deterministic adversarial replay suite through the normal Python gate:

```bash
PYTHONPATH=python/intelligence:python/model_serving:python/training \
  /scratch/djy8hg/env/aegis_mx_contracts/bin/pytest \
  python/tests/test_news_pipeline.py
```

The checked-in providers require no credential. `OfficialPublicSourceProvider`
accepts an injected approved HTTPS client and intentionally has no default
network transport. See the
[pipeline architecture](../architecture/news-filings-intelligence.md) and
[failure runbook](../operations/news-intelligence-failure-runbook.md).

Run the earnings specialist fixtures and replay tests with:

```bash
PYTHONPATH=python/intelligence:python/model_serving:python/training \
  /scratch/djy8hg/env/aegis_mx_contracts/bin/pytest \
  python/tests/test_earnings_specialist.py
```

See the [earnings specialist architecture](../architecture/earnings-specialist.md)
and [specialist failure runbook](../operations/earnings-specialist-failure-runbook.md).

Run the macroeconomic release fixtures, strict-abstention checks, and replay
tests with:

```bash
PYTHONPATH=python/intelligence:python/model_serving:python/training \
  /scratch/djy8hg/env/aegis_mx_contracts/bin/pytest \
  python/tests/test_macro_specialist.py
```

See the
[macroeconomic specialist architecture](../architecture/macroeconomic-release-specialist.md)
and
[failure runbook](../operations/macroeconomic-release-failure-runbook.md).

### Options analytics signal service

Run the deterministic options numerical, surface, dealer-pressure, recovery,
replay, and lifecycle suite directly with:

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts \
  /scratch/djy8hg/env/aegis_mx_contracts/bin/pytest \
  python/tests/test_options_analytics.py
```

The full benchmark task writes
`build/reports/benchmarks/options-analytics.json`. See the
[options testing guide](options-analytics-testing.md),
[architecture](../architecture/options-analytics.md), and
[failure runbook](../operations/options-analytics-failure-runbook.md).

### Point-in-time research data

Run the immutable-vintage queries and adversarial leakage suite with:

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts \
  /scratch/djy8hg/env/aegis_mx_contracts/bin/pytest \
  python/tests/test_point_in_time.py
/scratch/djy8hg/env/aegis_mx_contracts/bin/python \
  tools/benchmark_point_in_time.py --iterations 100 --records 256 \
  --output build/reports/benchmarks/point-in-time.json
```

See the [point-in-time testing guide](point-in-time-data-testing.md) and
[data contract](../architecture/point-in-time-data-contract.md).

### Control plane

```bash
(cd control && gofmt -l .)
(cd control && go vet ./...)
(cd control && go test ./...)
(cd control && go test -race ./...)
(cd control && go test -run '^$' \
  -bench 'Benchmark(VerifySignedManifest|FeatureSchemaCompatibility|InspectLineage|DeploymentObservation)$' \
  -benchmem -count=1 ./model_registry)
(cd control && go test -run '^$' -bench '^BenchmarkEdgeEvaluate$' \
  -benchmem -count=1 ./config_service)
```

The Go control plane includes non-networked foundation metadata, the local
signed model registry, and a signed immutable configuration API/CLI with
two-person approval, staged activation, rollback, emergency inhibits, and an
RPC-free local edge cache. It has no trading or gateway transport capability;
production authentication, HSM/KMS custody, and network distribution remain
deployment adapters. See
[model registry testing](model-registry-testing.md), the
[registry architecture](../architecture/model-registry.md), and the
[disable/rollback runbook](../operations/model-registry-rollback-runbook.md).
The same package includes signed shadow/canary evaluation and fail-closed
rollback; see [model deployment testing](model-deployment-testing.md), the
[deployment-control architecture](../architecture/model-deployment-control.md),
and the [deployment rollback runbook](../operations/model-deployment-rollback-runbook.md).
Configuration-focused commands and limitations are in
[configuration control testing](configuration-control-testing.md), the
[control-plane architecture](../architecture/configuration-control-plane.md),
and the [configuration runbook](../operations/configuration-control-runbook.md).

## Dependency and secret validation

```bash
make dependency-scan
```

This runs:

- `pip-audit --require-hashes` against the Python lock;
- `govulncheck@v1.7.0` against `control`; and
- `detect-secrets-hook` against visible repository files using the reviewed
  `.secrets.baseline`.

No credentials are needed. Synthetic sentinel values must be clearly marked in
future security tests and must never resemble a usable credential.

To intentionally update Python dependencies, edit
`python/requirements-dev.in`, then use an isolated temporary environment,
disable inherited per-user package indexes, and review the complete diff:

```bash
source tools/toolchain.sh
lock_env=$(mktemp -d)
python3.12 -m venv "$lock_env/venv"
"$lock_env/venv/bin/python" -m pip install pip-tools==7.6.1
"$lock_env/venv/bin/pip-compile" \
  --allow-unsafe \
  --generate-hashes \
  --index-url=https://pypi.org/simple \
  --resolver=backtracking \
  --output-file=python/requirements-dev.lock \
  --strip-extras \
  python/requirements-dev.in
rm -rf "$lock_env"
```

Dependency versions, archive hashes, licenses, and review expectations are in
[Third-Party Dependencies](../compliance/third-party-dependencies.md).

## Packaging and SBOM

```bash
make package
```

The command builds release-mode C++, creates CPack and Go package archives,
creates the Python wheel/sdist without an isolated dependency download, and
emits a CycloneDX 1.5 SBOM. The SBOM omits wall-clock time and uses a
deterministic UUID derived from project name/version.

## Container workflow

The host-independent path is:

```bash
docker compose -f infra/dev/compose.yaml build
docker compose -f infra/dev/compose.yaml run --rm dev bash -lc 'make bootstrap && make fast'
```

For full validation:

```bash
docker compose -f infra/dev/compose.yaml run --rm dev bash -lc 'make bootstrap && make full'
```

The base image is pinned by digest and the Go archive by SHA-256. Apt package
names are fixed, but Ubuntu repository snapshots are not yet pinned; exact
compiler/tool versions are therefore captured in build metadata and CI logs.
Bit-for-bit container reproduction remains a known Phase 1 limitation.

## Reports

| Report | Location |
| --- | --- |
| C++ JUnit | `build/reports/cpp/*.xml` |
| Python JUnit | `build/reports/python/junit.xml` |
| Python coverage | `build/reports/python/coverage.xml` |
| Go test events | `build/reports/control/*.json` |
| Go coverage | `build/reports/control/coverage.out` |
| Benchmark JSON | `build/reports/benchmarks/*.json` |
| Packages and SBOM | `dist/` |

CI uploads reports even when a gate fails and retains exhaustive artifacts for
30 days.
