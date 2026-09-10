# Aegis-MX

Aegis-MX is a safety-critical, event-aware intraday trading platform under
construction. The repository currently contains the engineering foundation,
canonical contracts, temporal-integrity library, deterministic synthetic
market-data generator, bounded feed handler, deterministic order-book and
feature engines, model contracts, infrastructure-only microstructure models,
an off-hot-path time-series forecast service, deterministic risk and OMS
kernels, a signed append-only model registry, a shadow/canary deployment
coordinator, and a repository-owned synthetic/paper gateway. It contains no real
feed adapter, strategy, proprietary order-entry protocol, network transmitter,
broker connection, credential, or live-trading behavior.

The [edge high-availability layer](docs/architecture/high-availability.md)
provides process/session fencing, bounded recovery replication, explicit
leader reconciliation, duplicate-emission suppression, and operator-visible
failover state. It consumes an externally validated witness grant; the
production witness, cross-host transport, and physical gateway fencing remain
integration boundaries.

The [colocated edge deployment bundle](docs/architecture/colocated-edge-deployment.md)
provides six non-live systemd profiles, reviewed CPU/NUMA/NIC/PTP and storage
contracts, local health validation, and deterministic rollback packages. The
composed daemon binaries and site hardware mappings remain prerequisites, so
the bundle fails closed instead of presenting the current libraries as a
runnable production edge.

The [regional Kubernetes deployment](docs/architecture/regional-kubernetes-deployment.md)
isolates nine non-hot-path services across intelligence, model, control,
observability, and research namespaces. Its production admission contract
requires immutable signed image evidence and explicitly excludes edge routing,
OMS, gateways, and deterministic pre-trade risk. Checked-in image digests are
non-routable fail-closed placeholders.

The [chaos framework](docs/architecture/chaos-and-fault-injection.md) executes
all required single faults and deterministic nightly combinations in an
isolated simulation harness. It emits content-hashed JSON evidence and cannot
address an OMS or gateway.

The [smart order router](docs/architecture/smart-order-router.md) implements
fixed-point paper/synthetic venue selection and bounded execution policies. It
emits only a proposed child; every child still requires fresh pre-trade risk,
OMS processing, and the gateway final safety predicate.

The normative rules are in the
[engineering contract](docs/architecture/engineering-contract.md). Real-money
trading is not enabled. The default build omits the live-only adapter boundary,
and enabling that compile boundary still provides no live implementation,
endpoint, transport, credential handling, or runtime authorization. Tests
require no secrets.

## Quick start

On a supported host with the prerequisites below:

```bash
make bootstrap
make fast
```

The exhaustive local gate is one command:

```bash
make full
```

On the UVA module environment used for initial development, the scripts load
the pinned compiler, LLVM, CMake, Ninja, and Go modules automatically when the
system defaults are too old. No shell activation is required.

## Prerequisites

- CMake 3.28 or newer;
- Ninja;
- GCC 11 or newer, or a C++20-capable Clang;
- `clang-format` and `clang-tidy`;
- Python 3.12 with `venv`;
- Go 1.26.4;
- Make, Git, and curl; and
- shellcheck for the complete shell lint gate.

Python tools are installed into `.venv` from the fully hashed
[`python/requirements-dev.lock`](python/requirements-dev.lock). C++ test and
benchmark dependencies are fetched from release archives pinned by version and
SHA-256. Go foundation code uses the standard library only.

## Canonical commands

| Command | Purpose |
| --- | --- |
| `make bootstrap` | Create `.venv` from the hashed lock, install the local Python package, download Go modules, and configure the C++ development preset |
| `make format` | Apply clang-format, Ruff formatting/fixes, and gofmt |
| `make format-check` | Check C++, Python, and Go formatting without modifying files |
| `make lint` | Run format checks, Ruff, strict mypy, go vet, clang-tidy, compiler warnings, and shellcheck when available |
| `make test` | Build and run CTest/GoogleTest, pytest with 100% coverage threshold, and Go tests |
| `make test-sanitizers` | Run separate ASan, UBSan, TSan C++ builds and the Go race detector |
| `make test-fuzz` | Run deterministic Clang libFuzzer + UBSan deserialization smoke tests; ASan remains a separate sanitizer gate |
| `make benchmark` | Run release-mode C++ and Python smoke workloads, including bounded data-repository usage/hash/admission measurements, and retain JSON output |
| `make benchmark-platform` | Run the pinned, warmed 14-stage/9-scenario suite with qualification sample counts |
| `make benchmark-platform-smoke` | Run the complete matrix with bounded non-qualifying sample counts |
| `make benchmark-regression` | Compare a platform report against an explicitly supplied approved baseline |
| `make paper-integration` | Run all 16 deterministic full-system PAPER scenarios and write machine/human acceptance reports |
| `make paper-soak-smoke` | Validate the bounded long-duration PAPER soak workflow |
| `make paper-soak` | Run a configurable local accelerated/realtime PAPER soak and retain raw evidence |
| `make dependency-scan` | Run pip-audit, pinned govulncheck, and the repository secret baseline check |
| `make chaos-fast` | Run all 23 bounded single-fault scenarios and emit a deterministic JSON report |
| `make chaos-nightly` | Run the repeated fault soak plus simultaneous fault combinations |
| `make edge-validate` | Validate all non-live edge profiles, systemd contracts, and host assets |
| `make edge-package` | Build and verify deterministic, non-activating rollback packages for all profiles |
| `make regional-validate` | Validate regional Kubernetes namespaces, workloads, security, scaling, disruption, GPU, backup, and release-integrity contracts |
| `make package` | Produce CPack, Python, control-plane, deterministic edge rollback, and CycloneDX SBOM artifacts |
| `make docs-check` | Validate UTF-8, whitespace, fences, local links, and Mermaid structure without network access |
| `make schemas-check` | Reproduce generated bindings and verify golden schema bytes |
| `make schemas-generate` | Regenerate canonical bindings and golden files after reviewed IDL changes |
| `make fast` | Run the pull-request validation set, including all single-fault chaos scenarios |
| `make full` | Run fast checks, the chaos soak/combinations, sanitizers, scans, packaging, and SBOM generation |

All commands run from the repository root. Detailed outputs and direct commands
are documented in
[Local Development](docs/testing/local-development.md).

Full-system PAPER acceptance, report paths, and the optional `parallel`-partition
Slurm launcher are documented in
[Full-system PAPER trading validation](docs/testing/full-system-paper-trading.md).

Long-duration accelerated/realtime PAPER soak commands, fixed stability
thresholds, raw evidence, and the two-node `parallel` Slurm launcher are
documented in [Long-duration PAPER soak testing](docs/testing/paper-soak-testing.md).

Canonical event contracts are documented in [schemas/README.md](schemas/README.md)
and [event contracts](docs/architecture/event-contracts.md). This phase requires
its isolated Python environment inside `/scratch/djy8hg/env`:

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make bootstrap
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make schemas-check
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make test
```

The provider-neutral options analytics service is documented in
[Options Analytics](docs/architecture/options-analytics.md). It is an advisory,
off-hot-path service with no order-entry capability; production options feeds
and reference data remain licensed integration boundaries.

## Synthetic exchange tools

SMX/1 is a repository-owned fixture protocol, not an exchange emulation or a
live gateway. Generate and verify a deterministic multi-venue stream with:

```bash
build/dev/cpp/market_data/synth-exchange-generate \
  --output-prefix build/fixtures/example \
  --seed 20260829 \
  --events 100000 \
  --scenario normal
build/dev/cpp/market_data/synth-exchange-verify \
  --capture build/fixtures/example.smxcap \
  --book build/fixtures/example.book.txt
build/dev/cpp/market_data/synth-exchange-inspect \
  --capture build/fixtures/example.smxcap \
  --max 10
```

`synth-exchange-stream` emits concatenated fixed-size SMX packets to stdout. It
has no network destination or venue connectivity. Complete configuration,
scenario, format, and pacing details are in the
[generator guide](docs/architecture/synthetic-exchange.md) and
[SMX/1 specification](docs/architecture/synthetic-mock-protocol.md).

The bounded [feed-handler framework](docs/architecture/feed-handler.md) decodes
SMX/1, reconciles synthetic A/B feeds, detects every continuity failure, and uses
synthetic retransmission or validated snapshots. It contains no real-feed socket
or proprietary protocol implementation.

The single-writer [order-book engine](docs/architecture/order-book-engine.md)
supports bounded order-by-order and price-level books, recovery snapshots,
multi-venue consolidation, and explicit fail-closed validity. It does not infer
licensed venue priority, auction, corporate-action, or crossed-transition rules.

## Synthetic microstructure model validation

The initial seven-model suite uses simulator-derived integer CSV and a stable
SHA-256-signed native artifact. It is infrastructure validation and makes no
claim of predictive or economic value. Use the required external environment:

```bash
build/dev/cpp/models/synth-microstructure-dataset \
  --output build/microstructure.csv --seed 20260829 10000
PYTHONPATH=python/training:python/intelligence \
  /scratch/djy8hg/env/aegis_mx_contracts/bin/python -m aegis_mx_training \
  --dataset build/microstructure.csv \
  --output-directory build/native-models \
  --seed 20260829 \
  --expires-wall-clock-utc-ns 1900000000000000000
build/dev/cpp/models/native-model-verify \
  build/native-models/queue_depletion.amdl build/microstructure.csv
```

The expiry value is a deterministic fixture. Artifact format, feature order,
expiry, OOD, and parity rules are in the
[microstructure suite guide](docs/architecture/microstructure-model-suite.md).

## Off-path time-series forecasting

The installable `timeseries-forecast-api`, `timeseries-forecast-worker`, and
`timeseries-context-builder` commands implement a bounded reference service for
return, realized-volatility, volume, spread, market-factor, and sector-factor
forecasts. The default adapter is deterministic infrastructure validation, not
Google TimesFM and not evidence of economic value. An approved TimesFM runtime
or checkpoint must be injected separately and cannot enter the colocated hot
path.

Architecture, endpoints, provenance, walk-forward baselines, fallback rules,
container manifests, and limitations are documented in the
[time-series service guide](docs/architecture/timeseries-forecast-service.md).

## News and filings intelligence

The `aegis_mx_intelligence` package contains bounded mock, filesystem-replay,
and injected official-public provider boundaries plus deterministic fast and
tool-less deep intelligence stages. Source text is always untrusted, active
content and instruction-like segments are isolated, entity resolution is
registry-backed, and fast alerts are never overwritten by deep refinements.
The pipeline publishes v1.4 `EventIntelligenceRecord` contracts only and has no
OMS, risk-approval, execution, or gateway connection. See the
[news and filings architecture](docs/architecture/news-filings-intelligence.md).

The deterministic [earnings-event specialist](docs/architecture/earnings-specialist.md)
normalizes GAAP/non-GAAP earnings facts, point-in-time estimates, guidance,
segments, option-implied moves, historical reactions, transcripts, and feature
snapshots. It retains evidence, rejects future-estimate leakage, supports linked
corrections and byte-stable replay, and emits only an advisory common forecast.

The deterministic
[macroeconomic release specialist](docs/architecture/macroeconomic-release-specialist.md)
freezes consensus before scheduled CPI, PPI, employment, GDP, retail-sales,
FOMC, Federal Reserve statement, Treasury-auction, and PMI events. It separates
scheduled, publication, receipt, exchange, and monotonic times; retains prior
values and revisions separately; integrates bounded cross-asset responses; and
emits no forecast bytes for partial, conflicting, untrusted, or incomplete data.

## Point-in-time research data

The offline `aegis_mx_research` package stores immutable event, publication,
receipt, processing, revision, and business-validity times for corporate
actions, symbol mappings, delistings, index membership, estimate and macro
vintages, corrected news, and filing amendments. Its dataset gate rejects
look-ahead, survivorship, label overlap, and randomized time-series splits with
stable reason codes. It has no order, gateway, or live-data connection.

The normative semantics are in the
[point-in-time data contract](docs/architecture/point-in-time-data-contract.md),
with focused commands in
[Point-in-time Data Testing](docs/testing/point-in-time-data-testing.md).

The bounded forecasting POC adds the local-only `aegis-data` command for an
external data root. It creates the fixed storage layout, reports logical and
allocated usage, fail-closes storage estimates without authoritative quota
evidence, verifies immutable manifests and objects, and produces cleanup plans
without deleting anything:

```bash
/scratch/djy8hg/env/aegis_mx_contracts/bin/aegis-data \
  --data-root /scratch/djy8hg/aegis_mx_poc_data usage
/scratch/djy8hg/env/aegis_mx_contracts/bin/aegis-data \
  --data-root /scratch/djy8hg/aegis_mx_poc_data verify
```

The [storage architecture](docs/architecture/poc-data-repository.md) and
[operations runbook](docs/operations/poc-data-storage.md) document quota input,
admission estimates, recovery, and the exact non-network/non-deletion boundary.

## Durable journal tools

The C++ journal boundary provides bounded nonblocking publication, segmented
checksummed local persistence, crash recovery, sparse seeking, copy-only repair,
retention controls, and a transmission-free verified replay stream. After
`make bootstrap` and a development build:

```bash
build/dev/cpp/journal/journal-inspect PATH
build/dev/cpp/journal/journal-verify PATH
build/dev/cpp/journal/journal-repair-copy SOURCE NEW_EMPTY_TARGET
build/dev/cpp/journal/journal-export SOURCE export.jsonl
build/dev/cpp/journal/journal-replay SOURCE replay.ajrp
```

The binary format, durability policies, failure semantics, and current replay
limitations are documented in the
[append-only journal architecture](docs/architecture/append-only-journal.md).
`journal-export` emits exact audit payloads and must be handled under the same
access policy as the source journal.

## Deterministic replay

The offline `aegis-replay` tool consumes a verified journal or the documented
synthetic capture format, supports deterministic time/instrument selection and
fault injection, and writes a content-bound manifest plus per-component output
hashes. It has no gateway dependency and cannot transmit orders.

```bash
build/dev/cpp/replay/aegis-replay \
  --capture events.smxcap --speed maximum --seed 20260831 \
  --manifest replay-manifest.json --summary replay-summary.json
```

Exact and counterfactual semantics, fault syntax, focused test commands, and
known integration boundaries are in
[Deterministic Replay Testing](docs/testing/deterministic-replay-testing.md) and
the [replay architecture](docs/architecture/deterministic-replay.md).

## Event-level backtesting

The offline `aegis-backtest` tool evaluates deterministic synthetic streams or
verified SMX captures with price-time queueing, acknowledgement latency,
partial fills, fees, slippage, impact assumptions, halts, and auctions. It has
no bar-fill input and no gateway dependency. Reports combine forecast,
execution, portfolio, event-period, and zero-versus-realistic-cost metrics and
explicitly prohibit raw-accuracy-only claims.

```bash
build/dev/cpp/backtesting/aegis-backtest \
  --synthetic-events 100000 --seed 20260831 --strategies 2 \
  --latency-min-ns 5000 --latency-max-ns 25000 \
  --report build/reports/backtest.json
```

The simulation model and limitations are in the
[event backtester architecture](docs/architecture/event-backtester.md); focused
commands and deterministic test coverage are in
[Event Backtester Testing](docs/testing/event-backtester-testing.md).

## Signed model registry

The off-hot-path Go model registry stores content-addressed artifacts, signed
immutable provenance manifests, hash-chained approval/deployment events, and
signed environment pointers. Exact feature-schema compatibility and every
lifecycle transition fail closed. Full-production model promotion is never
automatic: it requires an explicit command, external authorization reference,
and a promoter different from the recorded approver. This lifecycle does not
authorize live trading.

Build and inspect the CLI without any service credential:

```bash
source tools/toolchain.sh
(cd control && go test ./model_registry)
(cd control && go build -trimpath -o ../build/model-registry ./cmd/model-registry)
build/model-registry version
```

Mutation commands require externally supplied Ed25519 key paths and audited
actor/reason fields. Exact register, validate, approve, shadow, canary,
limited-risk, production, rollback, disable, and lineage commands are in
[Model Registry Testing](docs/testing/model-registry-testing.md). Architecture
and recovery behavior are in the
[model registry design](docs/architecture/model-registry.md) and
[rollback runbook](docs/operations/model-registry-rollback-runbook.md).

## Signed configuration control plane

The Go `config-service` manages one immutable snapshot for strategy and venue
policy, integer risk limits, exact model eligibility, ensemble caps, sessions,
event calendars, kill switches, simulation/paper mode, and operator roles.
Snapshots are canonicalized, SHA-256 identified, Ed25519 signed, staged, and
activated only after approval by a principal distinct from the author. Accepted
actions form an fsynced signed hash chain. Emergency commands only engage
hierarchical inhibits; they cannot clear them.

The edge consumes an atomically published verified local snapshot and performs
no control RPC or disk operation while evaluating it. Missing, expired, stale,
clock-regressed, unauthorized, or killed state blocks new orders. An allow is
still subject to the deterministic C++ market-state, risk, OMS, and gateway
controls. This build accepts only `SIMULATION` and `PAPER`.

```bash
source tools/toolchain.sh
(cd control && go test ./config_service)
(cd control && go build -trimpath -o ../build/config-service ./cmd/config-service)
build/config-service version
```

See [Configuration Control Testing](docs/testing/configuration-control-testing.md),
the [architecture](docs/architecture/configuration-control-plane.md), and the
[operations runbook](docs/operations/configuration-control-runbook.md).

## Operational readiness and operator drill

The final
[operational-readiness package](docs/operations/operational-readiness-package.md)
indexes architecture, ownership, startup/shutdown and daily PAPER checklists,
incident runbooks, licensed/regulatory reviews, and the deliberately prohibited
production-activation gate. Run the PAPER-only hierarchical kill simulation and
generate live-disabled evidence with:

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts \
  make operator-simulation
```

The drill exercises symbol, strategy, venue, and firm kills, rejects
unauthorized clearing, performs an authorized recovery evaluation, and exports
a hash-chained audit. It has no OMS, gateway, network, credential, or activation
API. A pass proves that the inspected state remains non-live; it reports
`production_ready=false` and `activation_status=PROHIBITED`.

## Shadow and canary model deployment

The off-hot-path Go deployment coordinator compares production and candidate
forecasts only when both bind the identical feature snapshot. It records
non-executable hypothetical decisions, evaluates all ten rollback dimensions in
every signed required regime with deterministic paired confidence bounds, and
never promotes automatically. Canary activation requires explicit second-person
approval. Canary eligibility is limited by signed symbol, strategy, capital,
order-rate, and fresh risk-snapshot controls and still cannot bypass normal risk,
OMS, or gateway gates.

Any configured confidence breach, invalid canary evidence, or audit failure
initiates compatible signed rollback; a rollback failure globally disables the
candidate. The full design is in
[Model Deployment Control](docs/architecture/model-deployment-control.md), with
focused commands in
[Model Deployment Testing](docs/testing/model-deployment-testing.md), the
[operational runbook](docs/operations/model-deployment-rollback-runbook.md), and
the checked-in
[Grafana dashboard](infra/observability/grafana/dashboards/model-deployment.json).

## Edge observability and decision explanations

The C++ observability boundary converts existing feed, model, ensemble, risk,
gateway, portfolio, cost, clock, journal, and supplied infrastructure snapshots
into a bounded Prometheus vocabulary. Critical producers only copy fixed-size
records into preallocated queues; they never render text, allocate, perform
network/disk I/O, or wait for exporter capacity. Structured logs use fixed event
codes and correlation IDs. OpenTelemetry encoding is explicitly off-path.

Every decision explanation binds market/event state, model identities and
outputs, feature provenance, eligibility/weights, uncertainty/cost/abstention,
risk outcome, and route/venue selection in one stable-hashed record. Exact IDs
remain in audit records rather than high-cardinality metric labels.

```bash
export AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts
source tools/toolchain.sh
cmake --preset dev
cmake --build --preset dev --target aegis_observability_tests
ctest --test-dir build/dev -R '^aegis_observability_tests$' --output-on-failure
```

See the [architecture](docs/architecture/observability-and-explainability.md),
[focused test commands](docs/testing/observability-testing.md),
[alert runbook](docs/operations/observability-alert-runbook.md), and
[edge dashboard](infra/observability/grafana/dashboards/aegis-edge-overview.json).

## Security hardening

Network services now fail closed unless TLS 1.3 mutual authentication is
configured from a read-only secret-manager mount. Certificate URI SANs provide
bounded Aegis-MX service identities; route permissions, per-identity integer
rate limits, request-body/path bounds, and a concurrent-handler ceiling apply
before forecasting work. Mounted certificate generations rotate without
putting network or certificate work into the colocated C++ hot path.

News documents default to a fresh resource-limited parser process with no
ambient environment or Python socket access. Model artifacts and configurations
retain their existing signed, content-addressed, hash-chained controls. Release
images are built with SBOM/provenance and signed by the scoped CI workload
identity against the immutable OCI digest.

```bash
export AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts
make security-test
make dependency-scan
make reproducibility-check
```

See the [threat model](docs/security/threat-model.md),
[trust boundaries](docs/security/trust-boundaries.md),
[incident response](docs/security/incident-response.md), and
[ADR 0035](docs/adr/0035-zero-trust-service-boundaries-and-sandboxed-content.md).

## Containerized development

The development image uses a digest-pinned Ubuntu 24.04 base and a checksummed
Go 1.26.4 archive. Python and project dependencies remain lock-controlled.

```bash
docker compose -f infra/dev/compose.yaml build
docker compose -f infra/dev/compose.yaml run --rm dev make bootstrap
docker compose -f infra/dev/compose.yaml run --rm dev make fast
```

For the exhaustive gate:

```bash
docker compose -f infra/dev/compose.yaml run --rm dev bash -lc 'make bootstrap && make full'
```

The same image is usable through
[the development-container definition](.devcontainer/devcontainer.json).

## Build metadata and outputs

Build metadata contains only project version, source revision, compiler identity,
build type, and the default-off `live_trading_capable` compile-boundary value. It deliberately
excludes wall-clock time, hostname, user, workspace path, credentials, and
environment secrets.

Generated outputs are ignored by Git:

- `build/reports/` — JUnit, coverage, Go JSON, and benchmark reports;
- `dist/cpp/` — CPack foundation archive;
- `dist/python/` — Python wheel and source distribution;
- `dist/control/` — control-plane Go package archive plus `config-service` and
  `model-registry` CLIs;
- `dist/edge/` — deterministic, non-activating rollback bundles for the six
  checked-in non-live deployment profiles; and
- `dist/aegis-mx.cdx.json` — deterministic CycloneDX SBOM.

## CI

- [Fast Validation](.github/workflows/fast.yml) runs formatting, linting,
  language tests, benchmark smoke, dependency review, caching, and report
  retention on pull requests and `main` updates.
- [Exhaustive Validation](.github/workflows/exhaustive.yml) runs dependency and
  secret scans, packaging/SBOM, separate ASan/UBSan/TSan matrix jobs, and the Go
  race detector on `main`, schedule, and manual dispatch.
- [Signed Container Release](.github/workflows/container-release.yml) builds a
  provenance/SBOM-attested time-series image, signs its immutable digest with
  keyless CI identity, verifies that identity, and retains the evidence.

GitHub actions are pinned to immutable commit SHAs. Workflows use only the
scoped built-in GitHub token and do not require repository secrets to test.
