# Provider-neutral ingestion testing

| Field | Value |
| --- | --- |
| Environment | `/scratch/djy8hg/env/aegis_mx_contracts` |
| Deterministic seed | `20260831` |
| Network in tests | Denied; fixtures and mock provider only |
| External credentials | None |

## Focused validation

```bash
export AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts
$AEGIS_PYTHON_ENV/bin/python -m ruff format --check \
  python/research/aegis_mx_research python/tests tools/benchmark_ingestion.py
$AEGIS_PYTHON_ENV/bin/python -m ruff check \
  python/research/aegis_mx_research python/tests tools/benchmark_ingestion.py
$AEGIS_PYTHON_ENV/bin/python -m mypy --strict python tools
$AEGIS_PYTHON_ENV/bin/python -m pytest -q \
  python/tests/test_ingestion.py \
  python/tests/test_ingestion_cli.py \
  python/tests/test_data_repository.py \
  python/tests/test_data_repository_cli.py
```

The repository pytest configuration collects branch coverage across all Python
packages and requires 100%. For a focused module-only diagnostic, clear the
global addopts and name the coverage target explicitly; this is not a
replacement for the complete gate:

```bash
$AEGIS_PYTHON_ENV/bin/python -m pytest -q -o addopts='' \
  python/tests/test_ingestion.py \
  --cov=aegis_mx_research.ingestion --cov-branch \
  --cov-report=term-missing --cov-fail-under=100
```

## Test inventory

The tests cover no-I/O planning, explicit execution, provider and entitlement
identity, expiry, unavailable health, request bounds, size overflow, exact
universe filtering, duplicate and conflicting objects, quota denial, retained
admission, timeouts, rate limits, bounded deterministic retry, partial
responses, bad offsets and completion flags, empty responses, source-version
changes, oversized objects, malformed payloads, count and hash mismatches,
quarantine, idempotence, correction lineage, full restart, verified-range
resume, stale and corrupt checkpoints, cooperative shutdown, concurrent
batches, publication faults, filesystem path/symlink attacks, credential
availability, complete redaction, and deterministic replay equivalence.

One test replaces `socket.socket` with a failing function while exercising the
HTTP/S3-like mock, proving the integration fixture is in-process. A source scan
must also show no networking client import in the ingestion or CLI modules.

## Benchmark smoke

```bash
$AEGIS_PYTHON_ENV/bin/python tools/benchmark_ingestion.py \
  --iterations 25 --object-bytes 65536 \
  --output build/reports/benchmarks/ingestion.json
```

The report retains raw monotonic samples and p50/p95/p99/maximum summaries for
dry-run planning, mock fetch/hash/checkpoint/publication, and checkpoint
encoding. It labels the HTTP/S3 boundary as a socket-free mock and records that
actual network access was false. Results are host-specific infrastructure
measurements, not market-data-provider or forecast-performance claims.

`make benchmark` includes this smoke with
`AEGIS_INGESTION_BENCHMARK_ITERATIONS` and
`AEGIS_INGESTION_BENCHMARK_OBJECT_BYTES` overrides.
