# Aegis-MX forecasting POC quality gates

| Field | Value |
| --- | --- |
| Status | Normative for Prompts 45 through 65 |
| Effective date | 2026-09-09 |
| Parent policy | [Aegis-MX quality gates](quality-gates.md) |
| POC contract | [Bounded forecasting POC contract](../architecture/forecasting-poc-contract.md) |
| Roadmap | [Bounded forecasting POC roadmap](../architecture/forecasting-poc-roadmap.md) |
| Default POC seed | `20260831` |

## Rule

These gates add data rights, storage, temporal integrity, research
reproducibility, and economic-claim controls to the repository-wide quality
policy. They do not replace any stricter parent gate. A command passes only when
it exits successfully and its output is reviewed and recorded. Missing tools,
credentials, source approval, quota evidence, data, GPU access, or cluster
capacity are explicit blockers or limitations, never implicit passes.

No failing test may be removed, skipped, or weakened to complete a phase. No
storage, leakage, minimum-sample, signature, calibration, deadline, or resource
threshold may be silently relaxed. No phase may download data, train, publish a
forecast, or progress an artifact beyond its authorized scope merely because a
downstream demonstration expects it.

## Required environment and isolation

All Python implementation, tests, training, evaluation, and benchmarks use:

```text
/scratch/djy8hg/env/aegis_mx_contracts
```

New dependencies are installed only into that environment, pinned exactly in
the appropriate input and lock files, audited, and added to the SBOM. The
repository `.venv` is not a substitute. Unit tests and ordinary integration
tests must deny external network access and use synthetic, filesystem, or mock
providers. Tests never require secrets. Authorized remote pilots, backfills, and
public-source jobs run as separately identified operational evidence, never as
ordinary CI.

The canonical invocation prefix is:

```bash
export AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts
```

Repository-owned wrappers remain the authority for available generic gates:

```bash
make format-check
make lint
make test
make schemas-check
make docs-check
make dependency-scan
make security-test
make benchmark
```

A phase that adds a POC command must add it to the appropriate wrapper or
document an exact reproducible invocation until the wrapper exists. Commands
that mutate a remote source or durable dataset are never part of `make test`.

## Evidence envelope

Every phase report records:

- exact command and arguments with secret values omitted rather than redacted
  into ambiguous placeholders;
- start and end UTC timestamps and exit status;
- test count, failures, skips, deterministic seed, and fixture identities;
- source-policy version, entitlement state, and universe SHA-256 where relevant;
- source, partition, dataset, feature-schema, model, configuration, code, report,
  and environment hashes where produced;
- current data-root bytes, projected peak bytes, temporary bytes, authoritative
  quota ceiling, and post-operation filesystem reserve;
- CPU, RAM, GPU, storage, filesystem, kernel, and Slurm metadata for benchmarks;
- raw result/log artifact locations and their hashes;
- limitations, unsupported symbols, missing periods, abstentions, and the next
  dependency; and
- explicit `live_trading_capable=false` and economic-claim disposition where a
  service, model, forecast, or report is involved.

Licensed payloads, credentials, signed URLs, private keys, document text, and
restricted provider fields must not enter evidence artifacts.

## Phase gate matrix

| Prompt | Change class | Mandatory gates before completion |
| --- | --- | --- |
| 45 | Documentation and ADR | Repository/relevant-code audit; required-section review; UTF-8/whitespace; links; Mermaid structural/render check where tooling exists; contract consistency; secret scan; changed-file review |
| 46 | Schema, universe, calendar | Format/lint; schema generation/check; C++/Python unit tests; golden and forward/backward compatibility; invalid enum/bounds; deserialization fuzz smoke; horizon/parse benchmarks |
| 47 | Python storage/security | Format/lint/strict types; unit and filesystem integration; path/symlink/concurrency/fault tests; storage-policy schema; dependency/secret scan; usage/hash/admission benchmarks |
| 48 | Compliance documentation/config | Current primary-source citations; approval-policy schema; default-disabled negative test; secret scan; links/docs; explicit unresolved-rights report; no network mutation |
| 49 | Python ingestion/security | Format/lint/strict types; unit/mock integration; deterministic replay; timeout/rate/retry/resume/shutdown; SSRF/path/size/secret tests; bounded throughput/RSS benchmarks; no real network in tests |
| 50 | Provider adapter and pilot | All Prompt 49 gates; exact approved-spec conformance; authorization preflight; five-day operational pilot; coverage/timestamp/adjustment/hash/restart/storage reports; secret scan |
| 51 | Controlled data operation | Pilot accepted; date/universe/authorization/quota/epoch preflight; resumable execution; complete verify/coverage/corruption scans; seeded sample validation; raw operational evidence |
| 52 | Reference/calendar/PIT | Format/lint/strict types; schema/compatibility as affected; temporal, mapping, action, conflict and overflow unit/property tests; replay determinism; lookup/adjustment benchmarks; complete-universe report |
| 53 | SEC and untrusted documents | Format/lint/types; mock-server integration; request-policy, parser, sandbox, prompt-injection, decompression/size/time, amendment, timestamp, retry/shutdown tests; parse/sandbox/storage benchmarks |
| 54 | GDELT and entity resolution | Format/lint/types; mock integration; entity ambiguity/fake ticker/dedup/contradiction/time/prompt/size/language tests; replay; attribution check; entity/classifier/storage benchmarks |
| 55 | FRED/ALFRED vintages | Format/lint/types; mock integration; series-rights, unit, release/revision/conflict/time/leakage/throttle tests; deterministic PIT replay; query/storage benchmarks; credential scan |
| 56 | Schema/storage migration | Format/lint/types; schema/golden/compatibility if affected; streaming normalization and persistent-store parity; corruption/crash/atomic-recovery tests; deterministic rebuild; memory/compression/query benchmarks |
| 57 | Dataset/features/labels | Format/lint/types; hand fixtures; property/reference comparisons; all-horizon calendar cases; deliberate leakage and embargo negatives; deterministic rebuild; coverage schema; rows/RSS/storage benchmarks |
| 58 | Model/training/artifact | Dependency lock/audit/SBOM; format/lint/types; deterministic train/retrain; dataset/leakage revalidation; runtime parity; calibration/OOD/expiry; signature/tamper/schema tests; CPU and optional GPU resource benchmarks |
| 59 | CLI/model service | Format/lint/types; single/universe/all-horizon unit/integration; abstention/stale/deadline/fallback/overflow/artifact/output tests; service contract/security; no-network/no-trading dependency; CPU/GPU inference benchmarks |
| 60 | Evaluation/reporting | Format/lint/types; metric goldens; walk-forward/embargo/leakage/insufficiency/calibration/quantile/report-schema tests; repeat hash; report throughput/RSS; unsupported-claim checks |
| 61 | Slurm performance | Harness unit/schema tests; benchmark smoke; approved parallel/GPU jobs; raw samples and hardware metadata; threshold/regression check; storage/RSS/VRAM/queue/network invariants |
| 62 | End-to-end acceptance | Full mock/synthetic CI composition plus separately authorized data run; acceptance schema; all upstream tests; tamper/leakage/storage/secret/prompt/no-trading negatives; deterministic repeat; resource evidence |
| 63 | Independent review | Safe local adversarial suite; preserved failing tests/evidence; complete severity/root-cause/remediation records; no external attack; no implementation before report completion |
| 64 | Remediation | Reproduce each blocker/critical first; smallest fix; all affected and regression gates; before/after benchmark evidence; fresh Prompt 63 audit; no weakened control |
| 65 | Handoff | Complete fast validation plus all relevant POC suites; docs/links/Mermaid; reproducibility walkthrough; cards/reports/manifests; secret/no-trading/live-disabled checks; final evidence index |

## Contract and schema gates

Universe and horizon contracts must make invalid states representable only as
explicit rejection or unresolved results. Tests include an input whose final
line has no newline, Unicode/control characters, duplicate symbols, mixed case,
overlength symbols, empty universe, order-preserving display, sorted canonical
hashing, and stable `InstrumentId` resolution.

Horizon tests cover regular sessions, overnight closure, weekend, holiday,
early close, configured halt behavior, daylight-saving transitions, missing or
conflicting calendar, and checked timestamp overflow. Every expected label and
forecast target timestamp is asserted directly. A fixed elapsed nanosecond value
must not be accepted as sufficient evidence for session horizons.

Any FlatBuffers evolution follows
[schema evolution policy](../../schemas/schema-evolution-policy.md): accepted
ADR, reader-first deployment, regenerated bindings, goldens, older/newer reader
tests, unknown enum/bounds tests, parser fuzzing, benchmarks, and documented
rollback. Existing fields and enum values are never repurposed.

## Source authorization gate

The source policy begins with every source disabled. Enabling one requires a
reviewed immutable record for the exact provider, dataset, version, environment,
user classification, credentials mechanism, date/universe scope, timestamp and
adjustment semantics, rate limits, persistent-storage rights, research and model
training rights, derived-data rights, retention/deletion, attribution, and
expiry/review date.

A source request is rejected before network access when approval is missing,
ambiguous, expired, mismatched, unsigned where signing is required, or narrower
than the requested dataset/use. Tests prove that free credentials, a valid HTTPS
endpoint, or a prior provider approval cannot substitute for exact authorization.
SEC, GDELT, FRED/ALFRED series, Nasdaq Trader, and market data are assessed and
enabled independently.

The only current exception to the checked-in deny-all state is an external,
owner-only, expiring policy for Alpaca Basic IEX `1Min` data. Its pilot must be
accepted before backfill, and every real run records the approval and policy
hashes, exact dates, universe hash, quota evidence, request/retry counts,
coverage outcomes, and deterministic verification hash. This exception does
not authorize another source or any order operation.

## Storage admission gate

All byte policy uses decimal GB. Reports may add binary GiB only with explicit
labels. Before each object, partition, derived dataset, training artifact, or
report publication, admission verifies:

```text
current data-root bytes
+ partial object bytes
+ temporary bytes
+ requested output bytes
+ bounded retry or rewrite overhead
= projected peak bytes
```

The operation is rejected if the data root would exceed 100 GB, temporary work
would exceed 20 GB, post-operation filesystem reserve would fall below 50 GB,
or the authoritative 10 TiB scratch soft quota and current quota usage are
unknown. The 80 GB target triggers an operator-visible stop-and-review before
additional scope. Shared filesystem free space alone is insufficient. Symlinks,
hard links where accounting is ambiguous, sparse files, concurrent admissions,
integer overflow, and interrupted publication must not bypass accounting.

Tests use disposable bounded directories and never delete user data. Production
cleanup output is a plan only. Accepted objects and manifests are immutable;
repair writes a verified replacement with lineage.

## Ingestion and data-quality gates

Dry-run planning is the default. Remote mutation requires an explicit
`--execute`, a matching approval, an admitted storage lease/epoch, an external
credential reference, and bounded date/universe/dataset filters. Redirect hosts,
response size, decompression, pagination, concurrency, rate, retry count, total
time, and checkpoint size are bounded. Credentials and signed URLs are redacted
before structured logging, exceptions, and reports.

Every accepted object binds request identity, provider/dataset/specification,
observation/publication/receipt/processing semantics, bytes, record count,
SHA-256, entitlement version, and correction lineage. Malformed, unverifiable,
unexpected-version, or scope-mismatched objects enter quarantine or reject;
they do not become canonical data. Resume verifies byte ranges when the source
supports stable range identity or restarts the complete object.

Coverage reports list every source symbol, requested session, source object,
duplicate, gap, unsupported result, quarantine reason, and correction. Missing
history is never replaced by synthetic, copied, forward-filled, or zero-volume
bars.

## Point-in-time, feature, and label gates

Canonical minute data uses integer ticks and quantities with explicit scale,
session, timestamp domains, source object/record identity, schema version,
quality state, and content hash. Impossible OHLC relationships, nonpositive or
unknown price scale, negative volume, conflicting duplicate bars, timestamp
disorder, and unapproved adjustment state reject. Identical duplicates are
idempotent and counted.

Persistent point-in-time queries must match the existing in-memory reference
store on canonical fixtures. Query order, inclusive/strict cutoff behavior,
validity intervals, revisions, amendments, symbol mapping, membership, and
corporate actions are compared. Parser/storage corruption and partial
publication fail closed.

Dataset tests deliberately introduce future macro revisions, future or
ineffective constituents/actions/mappings, post-cutoff news corrections and
estimates, survivorship bias, feature/label overlap, cross-split label overlap,
randomized/nonchronological splits, normalization fitted outside training, and
missing provenance. Any violation rejects the dataset. A 42-session purge and
embargo is calculated from the versioned calendar, not an approximate duration.

## Model, inference, and evaluation gates

Training starts with zero-return, last-value, seasonal-naive, and moving-average
baselines. Learned models use fixed ordered features, training-only
normalization, deterministic seeds, bounded missing-value behavior, and exact
dataset/universe/calendar/feature/configuration identities. Exported runtime
predictions must match Python inside a declared, reviewed tolerance on normal,
boundary, missing, and OOD fixtures. Artifact manifests and signatures are
verified before registry transition.

Each single or universe inference record is either a valid forecast or explicit
abstention. Every result includes ticker, stable instrument, horizon spec,
target timestamp, as-of/source price, return and quantile PPM, implied integer
prices when valid, quality/freshness/OOD/calibration/confidence, reason codes,
and all provenance hashes. NaN, Inf, stale inputs, expired model, late result,
schema mismatch, absent history, unknown calendar, and integer overflow cannot
produce a valid forecast.

Evaluation uses chronological walk-forward partitions and the same 42-session
embargo. Minimum samples are declared before results are inspected.
Ticker/horizon combinations below the threshold produce
`INSUFFICIENT_EVALUATION_DATA`. Reports include return and implied-price MAE and
RMSE, direction, log loss, Brier score, calibration, p10/p50/p90 coverage,
interval width, OOD, abstention, baseline comparison, and source-data coverage.
Minute bars do not establish queue position, passive fills, effective spread,
or tick-level execution behavior.

## Performance and Slurm gates

Benchmarks follow the parent quality policy and preserve raw samples. POC runs
add data-root and temporary-disk high-water marks, final bytes, rows/second,
model bytes/load time, CPU/GPU utilization, RAM/VRAM, forecasts/second, and
complete-universe latency. Threads/processes, warm-up, samples, seeds, compiler
or Python/dependency lock, CPU affinity, NUMA, storage mount, and input hashes
are recorded.

Login-node smoke, `parallel` partition, and GPU-partition measurements are never
combined. A GPU is optional and only one is used for the compact temporal model
or inference qualification unless a later ADR justifies more. CPU fallback must
remain deterministic. Resource regression thresholds are evidence-based,
versioned, and cannot be weakened without review and before/after evidence.

## Security and no-trading gates

Dependency and secret scans run after every source, provider, dependency,
artifact, or deployment change. Downloader tests cover hostile paths, URLs,
redirects, metadata, filenames, archives, decompression, response sizes,
timeouts, and credential strings. SEC/news content uses the existing untrusted-
document sandbox and schema-constrained outputs. Source text has no tools,
configuration authority, secret access, or trading interface.

Static dependency and runtime negative tests prove that POC packages and
services cannot import or call risk, OMS, router, gateway, exchange credentials,
or live activation. Network access during inference is treated as a failure.
Services expose the common operational contract and report
`live_trading_capable=false`. No POC evidence may be interpreted as an order,
trading recommendation, profitability claim, or production approval.

## Prompt 45 evidence

Prompt 45 is documentation-only. Its applicable completion checks are:

- required files and sections exist;
- all repository-relative Markdown links resolve;
- Mermaid sources pass the repository structural validator and, when an
  installed renderer is available without a network install, a render check;
- files are UTF-8 with balanced fences and no trailing whitespace or tabs;
- the POC contract is consistent with the engineering, point-in-time,
  licensing, schema evolution, and time-series service contracts;
- a secret-pattern scan finds no introduced credential; and
- only the five requested documentation artifacts are changed by this prompt.

Unit, integration, sanitizer, and performance tests are not applicable because
Prompt 45 changes no executable code, schema, build, runtime, or data artifact.
