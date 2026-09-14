# Aegis-MX bounded forecasting POC roadmap

| Field | Value |
| --- | --- |
| Status | Prompts 45–57 complete; training inputs admitted for bounded Prompt 58 infrastructure validation |
| Audit date | 2026-09-13 |
| Scope | Current repository plus Prompts 46 through 65 |
| Contract | [Bounded forecasting POC contract](forecasting-poc-contract.md) |
| Data flow | [Mermaid source](forecasting-poc-data-flow.mmd) |
| Quality gates | [Forecasting POC quality gates](../testing/forecasting-poc-quality-gates.md) |

## Outcome and roadmap policy

The objective is a reproducible, storage-bounded pipeline that reads the exact
universe from `ticker.txt`, acquires only explicitly authorized one-minute and
public-event inputs, constructs point-in-time datasets, trains baseline-first
pooled models, publishes twelve-horizon return forecasts or explicit
abstentions, and evaluates them without leakage or unsupported economic claims.

This roadmap is ordered by contracts and evidence. A downstream phase may not
substitute assumptions for an unmet dependency. In particular, remote market-
data access is blocked until source authorization is explicit, a full backfill
is blocked until the pilot passes, training is blocked until the dataset and
leakage gates pass, and final acceptance is blocked until independent review and
remediation are complete. No phase enables order entry or live trading.

## Audit method

The Prompt 45 audit read the governing engineering, point-in-time, time-series,
licensing, and quality-gate documents; reviewed the related ADRs; enumerated the
forecasting, research, training, backtesting, intelligence, model-registry,
schema, security, deployment, test, and benchmark trees; inspected public APIs,
test cases, dependencies, network boundaries, and provider markers; searched
for existing OHLCV, Parquet, downloader, calendar, SEC, GDELT, and ALFRED
implementations; checked the working tree and recent history; parsed
`ticker.txt`; and inspected current filesystem and Python-environment state.

The older [repository audit](repository-audit.md) accurately records the
documentation-only state that existed on 2026-08-28, but it is historical and
must not be used as the current implementation inventory.

## Current implementation inventory

| Boundary | Reusable current capability | Missing for this POC |
| --- | --- | --- |
| Repository/tooling | CMake, Python and Go gates; format, lint, test, sanitizer, fuzz, benchmark, dependency, secret, docs, package, and Slurm-oriented tooling | POC-specific commands, data policies, report schemas, and benchmark jobs |
| Universe/reference | Canonical `InstrumentId`, strict fixed-path `ticker.txt` loader, immutable hashed universe snapshots, explicit resolution outcomes, and deterministic versioned calendar/horizon resolution | Authorized point-in-time resolver/calendar sources and POC coverage report |
| Schemas | FlatBuffers v1.9; `ModelForecast` binds typed integer targets and additive semantic `HorizonSpec`/explicit target time with v1.8 compatibility goldens | Minute-record/report contracts |
| Time-series model serving | Bounded contexts, point-in-time checks, replaceable adapter, deterministic reference adapter, baselines, batching, deadlines, cache, CPU fallback, service endpoints, mTLS/RBAC, and common forecast publication | Data-backed context construction, twelve trading-calendar horizons, trained POC backend, ticker CLI, Parquet output, full evaluation, and actual TimesFM dependency/checkpoint if ever approved |
| Research | Immutable bitemporal types, bounded in-memory `PointInTimeStore`, `as_known_at` queries, chronological manifest types, and deterministic leakage findings | Persistent analytical store, minute bars, partitions/manifests, streaming dataset construction, all requested feature/label families, and 42-session embargo semantics |
| Training | Deterministic synthetic microstructure dataset, fixed-point logistic training, native export, and C++ parity for seven microstructure roles | Historical OHLCV training, pooled multi-horizon regressors/classifiers/quantiles, model cards, POC artifact export, and resource measurements |
| Registry/deployment control | Signed content-addressed Go registry, exact feature-schema compatibility, immutable lifecycle/audit, offline/replay validation, shadow/canary controls, rollback, and disable | POC artifact-manifest integration and environment evidence; production promotion remains out of scope |
| Intelligence | Provider-neutral document interface, mock/filesystem providers, injected official-public boundary, prompt defenses, process sandbox, deterministic news, filings/earnings, macro specialist contracts, and bounded SEC ticker/submissions/company-facts ingestion | GDELT adapter/entity coverage and FRED/ALFRED vintage adapter |
| Backtesting/evaluation | Offline deterministic event-level C++ simulator with synthetic order/price events and execution/cost reports; basic time-series walk-forward MAE/RMSE comparator | Minute-bar forecasting evaluator, per-ticker/horizon/sector/regime reports, required calibration/directional/quantile metrics, and explicit insufficient-data output |
| Security | Zero-trust service identities, TLS 1.3 mTLS, secret-mount abstraction, bounded parsers, document sandbox, secret/dependency scans, signed artifacts/configurations | Source-specific credential bindings, downloader SSRF/path controls, data-license enforcement, restricted source retention, and POC no-trading dependency proof |
| Deployment | Non-hot-path TimesFM Docker/Kubernetes skeleton and isolated regional manifests with fail-closed image placeholders | Data/training batch execution profiles, POC Slurm jobs, storage mounts/quotas, backup policy, and approved immutable images |
| Historical data | No data is committed to Git; the owner-only external root contains an accepted five-session pilot, a verified 501-session Alpaca IEX minute backfill with 9,462,709 canonical records, and a 7,684,385-row leakage-checked feature dataset | Optional event/sector inputs, trained POC models, and evaluation reports |

The declared Python runtime now pins FlatBuffers and PyArrow for the accepted
canonical and feature Parquet path. No gradient-boosting, ONNX, or
deep-learning package is yet part of the lock. Adding any such dependency
requires exact pinning, license/security review, SBOM regeneration, and
installation only in `/scratch/djy8hg/env/aegis_mx_contracts`.

## Audit observations and gaps

| ID | State | Observation | Required closure |
| --- | --- | --- | --- |
| FPOC-001 | Closed by Prompt 46 | Existing contexts still use elapsed `horizon_ns`, but the canonical v1.9 contract now distinguishes elapsed, trading-minute, and trading-session semantics and binds an explicit target | Integrate the v1.9 resolver into context/dataset construction in later prompts; do not regress to elapsed-only semantics |
| FPOC-002 | Closed by Prompt 47 | The bounded external-root layout, fail-closed quota admission, immutable manifests, verification, cleanup planner, audits, CLI, schemas, and benchmarks are implemented | Future writers must retain the admission lease and Prompt 48 source authorization remains mandatory before any download |
| FPOC-003 | Closed for the Prompt 50 Alpaca scope | A private, expiring, owner-only academic policy authorizes Basic IEX one-minute bars; the checked-in policy remains deny-by-default and no other source is enabled | Reassess on expiry or any account, terms, purpose, universe, or distribution change; authorize every future source independently |
| FPOC-004 | Closed for minute and SEC data by Prompts 49–53 | Provider-neutral bounded ingestion and the authorized Alpaca IEX minute adapter produced a verified two-year dataset; SEC report v1.1 binds the same closed date window, retrieves only intersecting historical submission shards, and provides current associations, submissions, company facts, immutable manifests, and complete-universe coverage with one unresolved symbol | Build only separately authorized GDELT and FRED/ALFRED adapters in Prompts 54–55; do not infer historical ticker truth, publication time, or amendment parents from absent SEC fields |
| FPOC-005 | Closed for the bounded POC | The published corpus has 4,000 verified Zstandard Parquet partitions and 9,462,709 canonical records. Records remain degraded for now-known/current-cohort mappings and incomplete historical halt coverage under ADR 0060. | Retain the degraded-evidence restrictions; never interpret the research quantum as a venue tick or claim consolidated-market coverage. |
| FPOC-006 | Closed and admitted | The real Prompt 57 build published 7,684,385 feature rows, 36,520 session summaries, all 948 coverage dispositions, and leakage `PASS`. Every resolved symbol/horizon pair exceeds 100 labels; the model-ready intersection retains more than 1.75 million TRAIN rows and at least 71 contributors per horizon; `KRKNF` abstains. | Prompt 58 must consume the immutable training-readiness feature mask, split counts, and exact dataset identity. Optional event/sector inputs require a new dataset build. |
| FPOC-007 | Ready to begin Prompt 58 | Existing training is synthetic microstructure infrastructure and unrelated to minute return forecasting; the authenticated minute dataset is now admitted only for infrastructure validation. | Train baseline-first pooled POC models and export signed artifacts without economic-value claims. |
| FPOC-008 | Blocking Prompts 59–60 | Existing service accepts already built contexts; evaluation covers four targets with MAE/RMSE only | Add ticker/universe inference, explicit abstention, implied-price derivation, and complete walk-forward evaluation |
| FPOC-009 | Blocking Prompts 61–65 | No POC resource suite, acceptance schema/report, independent POC review, or handoff exists | Qualify resources, validate end to end, audit, remediate, and publish final evidence |

## Capacity baseline and planning envelope

The authoritative 2026-09-11 `/opt/rci/bin/hdquota -s` observation established
a 10 TiB scratch soft quota, 608,990,093,312 bytes used, and
10,386,126,184,448 bytes available. The POC root remains independently bounded
to an 800 GB target and hard limit. Every mutation still requires current
quota evidence; a historical observation cannot silently authorize a later
operation.

The following preliminary steady-state envelopes preserve the original 80 GB
POC baseline. The remaining 720 GB is deliberately unallocated headroom, not
permission for a source adapter to expand its scope:

| Storage family | Planning ceiling |
| --- | ---: |
| Raw one-minute source objects | 12 GB |
| Canonical one-minute partitions | 18 GB |
| Derived features, labels, and evaluation datasets | 30 GB |
| SEC filing metadata, facts, and bounded evidence | 8 GB |
| GDELT metadata and derived records | 5 GB |
| FRED/ALFRED vintages | 1 GB |
| Reference data, manifests, models, reports, and indexes | 6 GB |
| Defined baseline | 80 GB |
| Unallocated policy headroom | 720 GB |
| Total target and hard ceiling | 800 GB |

Temporary work is separately bounded to 20 GB and counts toward the 800 GB hard
data-root peak. Each operation recalculates current and projected peak usage;
actual source estimates may require a smaller date range or representation.
Automatic deletion is forbidden.

## Dependency graph and execution order

The critical path is:

```text
45 contract
  -> 46 universe/horizons
  -> 47 bounded repository
  -> 48 source approval
  -> 49 neutral ingestion
  -> 50 market-data pilot
  -> 51 minute backfill
  -> 52 reference/calendar/actions
  -> 56 normalization/persistent PIT
  -> 57 features/labels
  -> 58 training
  -> 59 inference
  -> 60 evaluation
  -> 61 qualification
  -> 62 acceptance
  -> 63 independent audit
  -> 64 blocker remediation when required
  -> 65 handoff
```

After Prompts 47 through 49 and source-specific approvals, Prompts 52, 53, 54,
and 55 may be developed independently. Their accepted manifests converge at
Prompt 56 or 57. Parallel work cannot bypass the named acceptance gates.

## Phase backlog

### Prompt 46 — Universe and exchange-calendar horizon contracts

- Status: implemented and verified on 2026-09-09; downstream integration remains
  governed by the dependencies below.

- Inputs: `ticker.txt`, canonical identifiers, `ModelForecast`, schema evolution
  policy, and the POC contract.
- Outputs: immutable universe snapshot and digest, resolution outcomes,
  `HorizonSpec`, explicit target-time calculation, compatible schema additions,
  golden files, and universe/horizon architecture documents.
- Dependencies: Prompt 45 only. It blocks every operation that filters symbols,
  calculates labels, builds contexts, or reports coverage.
- Acceptance: source order and canonical hash are deterministic; all current
  symbols remain represented; malformed/duplicate input fails; weekends,
  holidays, early closes, and configured halts produce documented target times;
  older/newer readers interoperate according to the migration.
- Expected tests: parser boundaries, final line without newline, duplicates,
  malformed symbols, stable IDs, calendar scenarios, timestamp overflow,
  schema compatibility, goldens, and fuzz smoke.
- Expected benchmarks: bounded universe parse/hash and horizon-resolution
  throughput; schema encode/decode size and latency.
- Storage/licensing gate: negligible persistent size; authoritative production
  calendars and historical mappings remain source-dependent.

### Prompt 47 — Bounded POC data repository and quota enforcement

- Status: Implemented and locally verified; no remote data was accessed and the
  recommended persistent root was not populated by this phase.

- Inputs: POC contract, universe/horizon contracts, administrative allocation,
  filesystem facts, and a configured external data root.
- Outputs: storage layout, immutable manifests, atomic publication, usage and
  projected-peak calculator, quota admission, cleanup planner, audit records,
  and `aegis-data usage|estimate|verify|cleanup-plan`.
- Dependencies: Prompt 46 for universe identity. It must complete before any
  network download or durable source ingestion.
- Acceptance: 80/100/20/50 GB controls use decimal bytes; partial and temporary
  files count; unknown quota or projection rejects; symlink/path escape and
  concurrent admission fail closed; no automatic deletion occurs.
- Expected tests: quota/reserve exhaustion, overflow, temporary accounting,
  concurrent writers, path traversal, symlinks, corrupt/hash-mismatched
  manifests, interrupted atomic publication, and filesystem faults.
- Expected benchmarks: recursive usage scan, manifest verify, SHA-256 throughput,
  admission latency, and peak memory with representative file counts.
- Storage/licensing gate: test roots only; no remote data and no Git-tracked data.

### Prompt 48 — Source assessment and authorization gate

- Inputs: POC and licensed-boundary contracts, universe, current primary source
  terms/specifications, and the user's exact account/use classification.
- Outputs: cited provider comparison, explicit approval record, default-disabled
  source policy example, and authorization runbook.
- Dependencies: Prompt 45; Prompt 46 supplies exact universe evidence. It may be
  researched alongside Prompt 47 but must finish before provider code.
- Acceptance: Massive/Polygon, Alpaca IEX, SEC, GDELT, FRED/ALFRED, and Nasdaq
  Trader are assessed separately for coverage, timestamps, adjustments, rate,
  credentials, storage, training, derived use, retention, attribution, and user
  classification; ambiguity remains disabled.
- Expected tests: documentation/schema validation for default-disabled policy,
  invalid approval, scope mismatch, expiry, and secret-pattern rejection.
- Expected benchmarks: N/A; this is a compliance/documentation decision.
- Storage/licensing gate: no download. Explicit market-data storage and training
  permission is mandatory; public sources retain separate terms.

### Prompt 49 — Provider-neutral bounded ingestion

- Status: Implemented and locally verified with synthetic, filesystem, and
  socket-free deterministic mock providers. No real remote adapter or download
  was added; the checked-in source policy remains disabled.

- Inputs: accepted source policy, storage admission API, universe manifest, and
  external credential interface.
- Outputs: provider-neutral fetch contracts, dry-run planner, bounded executor,
  checkpoints, quarantine, mock/filesystem/synthetic providers, and fetch CLIs.
- Dependencies: Prompts 46 through 48.
- Acceptance: dry-run is default; `--execute` is required for remote mutation;
  retries, concurrency, body/object size, paths, dates, and storage are bounded;
  restart is idempotent; credentials are redacted; unit tests have no network.
- Expected tests: deterministic mock timeouts, throttles, partial/changing/
  duplicate/oversized objects, quota refusal, shutdown/resume, hash mismatch,
  SSRF/redirect, credential redaction, and replay.
- Expected benchmarks: plan size, mock download throughput, hashing, checkpoint
  frequency, bounded concurrency/RSS, and restart overhead.
- Storage/licensing gate: mock data only until a specific source is enabled by a
  matching approval.

### Prompt 50 — Authorized minute-data adapter and pilot

Status on 2026-09-11: implemented and accepted for the conditionally authorized
Alpaca Basic IEX scope. The five-session pilot covered all 79 requested symbols,
retained `KRKNF` as unsupported OTC, and passed manifest/hash/sample checks.

- Inputs: exact approved source specification and entitlement, universe
  manifest, storage/ingestion framework, calendars, and external credentials.
- Outputs: one provider adapter plus five-trading-day raw/canonical pilot
  manifests and machine/human coverage reports.
- Dependencies: Prompts 46 through 49. Missing or ambiguous authorization is a
  required stop, not a phase failure to work around.
- Acceptance: one-minute OHLCV only, explicit execution, all source symbols
  reported, provider timestamps/adjustments verified, gaps unfilled, restart
  idempotent, hashes valid, peak disk admitted, and no secret emitted.
- Expected tests: licensed-contract mapping against mock/sanitized fixtures,
  timestamp and scale boundaries, pagination/rate/retry/resume, adjustment and
  duplicate behavior, unsupported symbols, and secret scans.
- Expected benchmarks: pilot source/canonical rows per second, bytes per second,
  API rate utilization, peak RSS/temp/disk, and checksum cost.
- Storage/licensing gate: pilot projection must fit; exact provider rights and
  credentials are external blockers.

### Prompt 51 — Bounded two-year minute backfill

Status on 2026-09-11: complete. The 501-session backfill produced 9,462,709
canonical records in 36,520 partitions. Two exact-coverage verification passes
were byte-identical and the full repository scan found no manifest or object
errors. The external data root uses 7,782,178,266 logical bytes.

- Inputs: accepted pilot, unchanged source approval, latest complete market
  date, exact range, universe digest, capacity projection, and credentials.
- Outputs: immutable two-year source manifests, checkpoints, coverage/gap/
  corruption reports, deterministic sample validation, and storage evidence.
- Dependencies: Prompt 50 plus successful Prompt 47 admission.
- Acceptance: all symbols attempted exactly within scope; unsupported/missing
  outcomes explicit; no active epoch overlap; retries bounded; every object and
  partition hashes; restart is idempotent; reserve and hard limits hold.
- Expected tests: preflight refusal, epoch fencing, interruption/restart,
  changed objects, date/session coverage, duplicate/corrupt partitions, and
  deterministic seeded sampling.
- Expected benchmarks: actual download/canonicalization throughput, total bytes,
  object counts, peak RSS/temp/disk, and verify/resume duration.
- Storage/licensing gate: do not start when projected peak, quota, reserve,
  approval, or credential state is unknown.

### Prompt 52 — Point-in-time reference, calendars, and corporate actions

Status on 2026-09-11: implemented and verified using already retained Alpaca
current-asset and retrospective-calendar evidence only. The owner-only report
covers all 79 symbols, but correctly remains `PARTIAL_REFERENCE_COVERAGE` and
unsafe for historical training because authoritative historical symbology,
actions, delistings, and halts are not authorized or available.

- Inputs: universe/horizon contracts, approved reference sources, existing
  bitemporal types/store, and source manifests.
- Outputs: stable resolution history, sessions/holidays/early closes/halts,
  rational corporate actions, and a complete-universe resolution report.
- Dependencies: Prompts 46 through 49 and source-specific approval. It may run in
  parallel with bounded source ingestion.
- Acceptance: mapping is deterministic at as-of time; current mappings are not
  represented as historical completeness; conflicting or missing data is
  unresolved; no automatic substitution or future-action leakage occurs.
- Expected tests: ticker reuse/change, splits/reverse splits, delisting, IPO,
  ADR, OTC/unsupported venue, holidays, early close, missing calendar,
  future-known action, conflicts, fixed-point/rational overflow, and replay.
- Expected benchmarks: batch universe resolution, calendar target calculation,
  point-in-time lookup, and adjustment throughput/RSS.
- Storage/licensing gate: authoritative historical symbology, actions, halts,
  and venue calendars require approved sources and may remain incomplete.

### Prompt 53 — SEC EDGAR ingestion

- Inputs: resolved issuer mappings, approved SEC policy, bounded ingestion,
  storage admission, and document sandbox.
- Outputs: bounded submissions/form/XBRL manifests, amendment lineage, issuer and
  filing coverage reports, and sanitized evidence records.
- Dependencies: Prompts 47 through 49 and relevant Prompt 52 resolution.
- Acceptance: official endpoint and identifying User-Agent policy, bounded SEC
  request rate, 8 GB cap, exact acceptance/publication and local times, active
  content stripped, prompt text inert, amendments immutable, and no order path.
- Expected tests: mock throttling, malformed/oversized/decompression/nesting
  cases, duplicate submissions, amendments, timestamp disorder, prompt
  injection, shutdown/resume, and hash verification.
- Expected benchmarks: metadata/fact parse throughput, sandbox latency, peak RSS,
  request rate, and compressed/final size.
- Storage/licensing gate: no complete EDGAR mirror; source policy and SEC access
  requirements must remain current.

### Prompt 54 — Bounded GDELT metadata and entity resolution

Status: implemented and locally verified; the real two-year retrieval remains
externally gated by a Google Cloud project, BigQuery access/billing, and a
short-lived OAuth token. No GDELT or publisher data was downloaded during
implementation.

- Inputs: resolved issuer aliases, approved GDELT policy, bounded ingestion,
  point-in-time contract, and untrusted-text controls.
- Outputs: two-year bounded metadata, entity/rejection mappings, deterministic
  advisory fast events, coverage/quality/storage reports, and attribution.
- Dependencies: Prompts 47 through 49 and Prompt 52 identity. It can run in
  parallel with Prompts 53 and 55.
- Acceptance: issuer-relevant queries only, 5 GB cap, no global mirror or assumed
  publisher full-text rights, ambiguous matches explicit, timestamps honest,
  dedup deterministic, content inert, and no trading dependency.
- Expected tests: fake tickers, ambiguous names, repeated URLs, contradictions,
  prompt injection, timestamp disorder, oversized/unsupported-language records,
  missing URLs, attribution, and replay hashes.
- Expected benchmarks: query/parse/dedup/entity rows per second, classifier
  latency, peak RSS, rejection rate, and bytes retained.
- Storage/licensing gate: GDELT terms and underlying publisher restrictions are
  distinct; original article rights are not inferred.

### Prompt 55 — Bounded FRED/ALFRED vintages

Status: implemented and locally verified with deterministic mock transports;
no remote series was downloaded. Real execution remains gated by a registered
FRED API key, an unexpired owner-only approval, and current authoritative quota
evidence.

- Inputs: approved series-by-series policy, macro contracts, bounded ingestion,
  point-in-time store, and external API key.
- Outputs: selected immutable vintages, source/unit/transformation catalog,
  as-known queries, and data-quality/coverage manifests.
- Dependencies: Prompts 47 through 49. It can run in parallel with Prompts 53
  and 54.
- Acceptance: less than 1 GB; each series owner and rights reviewed; initial and
  revised values remain distinct; release/revision/local times preserved;
  restricted series rejected; secrets redacted; replay deterministic.
- Expected tests: initial/revision/conflicting vintage, missing release, future
  leakage, throttling, malformed response, unit/transformation mismatch, and
  deterministic query/replay.
- Expected benchmarks: vintage parse/store/query throughput, peak RSS, API rate,
  hash verification, and final bytes.
- Storage/licensing gate: an API key does not authorize every FRED-hosted series;
  PMI is included only with explicit underlying-source permission.

### Prompt 56 — Canonical minute normalization and persistent PIT storage

- Inputs: accepted minute/reference/event manifests, schemas, calendars,
  corporate-action versions, and in-memory store semantics.
- Outputs: streaming canonical minute partitions in Zstandard Parquet, atomic
  quality/manifests, persistent bitemporal store, and reference-store parity.
- Dependencies: Prompts 51 and 52; accepted outputs from Prompts 53 through 55
  are integrated only where available and approved.
- Acceptance: bounded memory; date plus deterministic instrument-bucket
  partitioning; integer OHLCV; no tiny per-ticker layout; no fill/fabrication;
  bad/conflicting data quarantined; idempotent duplicate handling; exact
  point-in-time query parity; deterministic partition hashes.
- Expected tests: OHLC invariants, scale/overflow, negative volume, disorder,
  identical/conflicting duplicates, missing minutes, crash/atomic recovery,
  corrupt Parquet/manifests, old/new store parity, and deterministic rebuild.
- Expected benchmarks: raw-to-canonical rows per second, compression ratio,
  point-in-time queries, memory high-water, partition count, temporary and final
  bytes.
- Storage/licensing gate: projected raw plus canonical plus temporary peak must
  remain below hard/root/reserve limits; source retention terms govern raw data.
- Implementation status (2026-09-13): complete for the bounded POC. The
  verified real promotion contains 4,000 partitions and 9,462,709 records. The
  deliberately degraded now-known reference semantics are governed by ADR
  0060 and cannot support survivorship-bias-free or venue-tick claims. See
  [canonical minute storage](canonical-minute-storage.md).

### Prompt 57 — Leakage-safe features and labels

- Inputs: canonical minute/PIT store, universe, calendars, horizon specs,
  reference/actions, and accepted event/macro records.
- Outputs: derived session summaries, feature/label partitions for all twelve
  horizons, immutable dataset manifest, leakage report, and per-symbol/horizon
  coverage.
- Dependencies: Prompts 46, 52, and 56.
- Acceptance: no separate daily input; all label target timestamps use calendar
  semantics; no feature/label or future-revision leakage; training-only
  normalization; chronological splits and 42-session embargo; seed `20260831`;
  bounded streaming build and explicit missing-label reasons.
- Expected tests: hand-calculated features/labels, every horizon, closures and
  actions, intentionally leaking macro/news/reference samples, overlap,
  normalization leakage, recent listings, unsupported instruments, overflow,
  deterministic rebuild, and complete coverage accounting.
- Expected benchmarks: feature rows/second, labels/second, session aggregation,
  peak RSS/temp/final size, and dataset verification.
- Storage/licensing gate: derived dataset plus projected training outputs must
  fit the remaining target; derived/model rights must be approved.
- Implementation status (2026-09-13): complete for the bounded POC. The real
  build contains 7,684,385 feature rows, 36,520 session summaries, 306 output
  objects, leakage `PASS`, and 79,065,673 valid labels across the twelve
  horizons. All 78 resolved symbols clear the per-horizon minimum; unsupported
  `KRKNF` remains an explicit abstention. See
  [feature datasets](leakage-safe-feature-datasets.md), the
  [coverage report](../testing/forecasting-poc-label-coverage.md), and the
  [training-readiness contract](training-readiness.md).

### Prompt 58 — Pooled multi-horizon training

- Inputs: accepted dataset and feature schemas, dependency policy, model
  registry, CPU/GPU facts, and the dedicated Python environment.
- Outputs: baseline and learned-model comparisons, exported runtime artifacts,
  parity evidence, signed manifests, model cards, and offline/replay registry
  states.
- Dependencies: Prompt 57 plus a published
  `READY_FOR_INFRASTRUCTURE_VALIDATION` training-readiness report. No training
  begins on a rejected, dirty-source, or unverified dataset.
- Acceptance: all horizons covered; pooled instrument/horizon features; fixed
  ordering and training-only normalization; deterministic seeds; calibration,
  confidence, OOD, expiry, provenance, and signature present; parity within a
  declared tolerance; no stage beyond `REPLAY_VALIDATED`.
- Expected tests: deterministic training, malformed/missing features, split and
  leakage revalidation, export/load parity, artifact tampering, schema mismatch,
  OOD/calibration, expiry, and registry lifecycle limits.
- Expected benchmarks: CPU training wall time/RSS, optional single-GPU time and
  VRAM, inference latency/throughput, model size/load, and export parity cost.
- Storage/licensing gate: dependencies install only into the named environment;
  training rights must cover every dataset; model/report bytes are admitted.

### Prompt 59 — Twelve-horizon forecast CLI and service

- Inputs: universe/horizons, latest complete canonical data, signed compatible
  artifacts, feature builder, configuration, and existing service shell.
- Outputs: `aegis-forecast`, single/universe batch output in JSON and Parquet,
  explicit abstentions, implied prices, operational endpoints, and benchmarks.
- Dependencies: Prompts 46, 56, and 58.
- Acceptance: every requested source ticker/horizon has one deterministic result;
  valid returns/quantiles use integer PPM; prices use checked integer arithmetic;
  all source/model/dataset/feature/config/universe hashes bind; stale, late,
  malformed, incompatible, or overflow results reject; no trading import/call.
- Expected tests: 79-current-symbol fixture without hardcoding production count,
  all horizons, IPO/unsupported/missing history, stale data, artifact mismatch,
  deadline, CPU fallback, overflow, output ordering/hashes, service health and
  bounded shutdown, and no-network inference.
- Expected benchmarks: single ticker and complete-universe CPU/single-GPU
  latency, forecasts/second, batch sizes, model load, peak RSS/VRAM, and output
  serialization.
- Storage/licensing gate: inference performs no hidden fetch and stores only
  admitted outputs; source data is read under its approved rights.

### Prompt 60 — Leakage-safe performance evaluation

- Inputs: accepted forecast outputs, labels, chronological split manifest,
  baselines, artifact hashes, and environment metadata.
- Outputs: `aegis-forecast-evaluate`, per-ticker/horizon/pooled/sector/regime
  machine and human reports with insufficiency outcomes.
- Dependencies: Prompts 57 through 59.
- Acceptance: 42-session embargo; declared minimum samples; complete forecast,
  calibration, quantile, OOD, abstention, coverage, and baseline-relative
  metrics; minute-bar execution limitations explicit; reports bind all required
  hashes and reproduce.
- Expected tests: metric fixtures, leakage, embargo, insufficient samples,
  malformed predictions, quantile/calibration bounds, missing coverage,
  corporate-action exclusions, deterministic report schema/hash, and economic-
  claim guardrails.
- Expected benchmarks: evaluation rows/second, report generation time/RSS, and
  output size by ticker/horizon.
- Storage/licensing gate: evaluation outputs remain within reports allocation;
  derived publication follows provider restrictions.

### Prompt 61 — Resource and performance qualification

- Inputs: accepted full POC artifacts, performance policy, data-root admission,
  cluster hardware facts, and Slurm partitions.
- Outputs: reproducible smoke/full benchmark jobs, raw JSON, hardware metadata,
  regression policy, job IDs, and resource conclusions.
- Dependencies: Prompts 50 through 60 as applicable to each scenario.
- Acceptance: five-day, one-month, full two-year, single/universe, CPU/GPU,
  missing-heavy, and news-burst scenarios report every required metric; limits
  hold; no unbounded memory/queues; fallback needs no GPU; inference has no
  network fetch; login/parallel/GPU results are distinct.
- Expected tests: benchmark harness schema, deterministic smoke workloads,
  threshold comparison, hardware metadata, interrupted job, raw-result hash,
  and no-threshold-weakening guards.
- Expected benchmarks: download, normalization, features, labels, CPU/GPU
  training, model load, inference, reports, RSS/VRAM, temp/final bytes, and
  complete-universe capacity.
- Storage/licensing gate: benchmark source data must be approved or synthetic;
  every job runs the same 80/100/20/50 GB admission checks.

### Prompt 62 — Full data-to-forecast validation

- Inputs: every accepted upstream contract, source manifest, dataset, artifact,
  report schema, and resource result.
- Outputs: acceptance JSON schema/report, human system report, command summary,
  evidence index, and end-to-end hashes for the complete source universe.
- Dependencies: Prompts 46 through 61.
- Acceptance: all sixteen stated acceptance properties in Prompt 62 pass,
  including complete ticker/horizon disposition, no fabrication/daily source,
  PIT and deterministic hashes, signed provenance, storage, secret/prompt
  containment, no trading connection, live disabled, and honest claims.
- Expected tests: full synthetic/mock pipeline in CI, authorized-data run in the
  approved environment, tampering, leakage, storage exhaustion, source failure,
  prompt injection, dependency-graph/no-network checks, repeat build/inference,
  and report schema.
- Expected benchmarks: aggregate phase durations, peak resources, final usage,
  complete-universe inference, and report generation using Prompt 61 methods.
- Storage/licensing gate: absence of authorized source evidence produces a
  machine-readable blocked acceptance result, never synthetic substitution.

### Prompt 63 — Independent hostile POC review

- Inputs: completed implementation and Prompt 62 evidence, immutable copies of
  safe local/synthetic/mock artifacts, and governing contracts.
- Outputs: independent audit, blocker list, risk register, evidence index, and
  failing local regression tests for successful findings.
- Dependencies: Prompt 62. The reviewer must not add product features or attack
  external systems.
- Acceptance: every listed omission, leakage, corruption, quota, secret,
  licensing, prompt, artifact, determinism, and trading-coupling avenue is
  challenged; findings have evidence, severity, root cause, and smallest
  recommendation; no premature safety/economic claim.
- Expected tests: adversarial cases identified by the audit using only bounded
  local resources; failing tests remain preserved until remediation.
- Expected benchmarks: N/A unless a finding concerns a resource/performance
  threshold, in which case preserve the reproducing workload and raw samples.
- Storage/licensing gate: copied/synthetic artifacts only; no provider attack or
  unapproved remote access.

### Prompt 64 — Blocker and critical remediation

- Inputs: complete Prompt 63 report and preserved failing evidence.
- Outputs: smallest robust fixes, deterministic regression tests, updated risk
  register/evidence, and a fresh independent audit.
- Dependencies: Prompt 63 and only applies when `BLOCKER` or `CRITICAL` findings
  exist.
- Acceptance: each issue is reproduced before the fix; root cause and evidence
  link are recorded; all relevant gates pass; fresh audit leaves no blocker or
  critical issue; storage/leakage/signature/source controls are not weakened.
- Expected tests: issue-specific schema, unit, integration, security, leakage,
  replay, storage, inference, and fault tests plus unaffected regression suites.
- Expected benchmarks: rerun affected baselines and regression gates; record
  before/after raw evidence without relaxing thresholds.
- Storage/licensing gate: remediation cannot grant rights, fabricate data, hide
  unsupported symbols, or expand resource limits without a new decision.

### Prompt 65 — Final bounded POC handoff

- Inputs: accepted Prompt 62 evidence, fresh Prompt 63 audit, Prompt 64 closure
  where needed, and all immutable manifests/reports.
- Outputs: operator runbook, model card, data card, research results, final
  architecture, and a complete final evidence index.
- Dependencies: Prompts 62 through 64 with no open blocker/critical finding.
- Acceptance: all 25 required handoff subjects are linked and reproducible; fast
  validation and all relevant POC tests pass; commands, hashes, storage,
  benchmarks, limitations, unresolved noncritical risks, scale requirements,
  and storage-investment triggers are explicit; live remains disabled.
- Expected tests: docs/links/Mermaid, manifest/report/card schema, reproducibility
  walkthrough, clean-environment commands, no-trading dependency, secret scan,
  and final live-disabled evidence.
- Expected benchmarks: report final accepted Prompt 61 results; no new marketing
  benchmark is substituted during handoff.
- Storage/licensing gate: backup/cleanup is a plan, not automatic deletion;
  scaling and new sources require new capacity and authorization decisions.

## Component ownership and execution class

| Work | Logical owner | Execution class |
| --- | --- | --- |
| Universe, horizon, minute, manifest, and report schemas | `model-contracts` / `schemas` | Offline generation; near-real-time validation where consumed |
| Storage admission, manifests, source ingestion, normalization | `research` with data-governance review | Offline/asynchronous |
| SEC, GDELT, and ALFRED adapters | `intelligence` | Asynchronous/offline |
| Calendar/reference/action resolution | `research` and reference-data boundary | Offline/asynchronous |
| Feature/label datasets, training, and evaluation | `research` / `training` | Offline |
| Batch forecasting and context service | `model-serving` | Near-real-time outside the hot path |
| Artifact signing and lifecycle | `control-plane` / model registry | Asynchronous |
| Slurm, reports, packaging, and evidence | `tools`, `deployment`, and testing support | Offline |

No work in this roadmap belongs to the hard real-time-like execution path.
Forecast publication may feed the existing asynchronous forecast-cache boundary
in later separately authorized work, but this POC does not integrate a strategy,
risk engine, OMS, router, or gateway.

## Explicit external blockers

The following cannot be completed from repository contents alone:

- A real one-minute provider adapter, pilot, and backfill require the provider's
  current authorized specification, user/account classification, credentials,
  and affirmative storage, training, derived-data, retention, and attribution
  rights.
- Authoritative historical mappings, corporate actions, venue calendars, halt
  intervals, and adjustment semantics require approved reference sources.
- SEC, GDELT, and FRED/ALFRED access must comply with each source's current
  request, identification, attribution, and underlying-data terms. Public access
  does not erase those obligations.
- Exact historical local receive latency cannot be reconstructed when it was not
  observed. The limitation must remain explicit.
- A real TimesFM package or checkpoint is not present. Adding one requires a
  pinned dependency/checkpoint, rights review, model card, SBOM, signature,
  runtime parity, and resource qualification. It is not necessary to prove the
  baseline-first POC.
- Scratch quota evidence must be refreshed with `/opt/rci/bin/hdquota -s`
  before mutation. Shared filesystem capacity alone remains insufficient.
- Production identities, secret-manager values, immutable image digests, and
  organization approval roles are external deployment inputs. None is needed
  for safe local mock testing, and none authorizes live trading.

## Next dependency

Prompt 46 is the next implementation phase. It must make the universe and
trading-calendar horizon semantics explicit before any downloader, historical
label builder, or user-facing forecast command is implemented.
