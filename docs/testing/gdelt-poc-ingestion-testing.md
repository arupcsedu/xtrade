# GDELT POC ingestion testing

Deterministic seed: `20260911`.

The unit and socket-free integration suite is
`python/tests/test_gdelt.py`. It covers bounded query planning, exact entity
resolution, fake ticker rejection, alias ambiguity, provider/content dedup,
same-URL contradiction lineage, observation/receipt/processing order,
unsupported languages, missing and hostile URLs, oversized metadata, prompt
injection, schema compatibility, immutable storage, quota admission, client
provenance, and deterministic replay IDs.

Run:

```bash
export AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts
PYTHONPATH=python/intelligence:python/research:python/training:python/model_serving:. \
  $AEGIS_PYTHON_ENV/bin/pytest python/tests/test_gdelt.py
$AEGIS_PYTHON_ENV/bin/ruff format --check \
  python/research/aegis_mx_research/gdelt.py python/tests/test_gdelt.py
$AEGIS_PYTHON_ENV/bin/ruff check \
  python/research/aegis_mx_research/gdelt.py python/tests/test_gdelt.py
$AEGIS_PYTHON_ENV/bin/mypy \
  python/intelligence python/model_serving python/research python/tests tools
```

Unit tests use an injected scripted query client and fixed-host mock transport;
they perform no remote request. The production transport is exercised only for
URL enforcement and injected response/error behavior. A real BigQuery run is
an operational acceptance test and requires the external prerequisites in the
[runbook](../operations/gdelt-poc-ingestion.md).

The benchmark `tools/benchmark_gdelt.py` measures parse/entity/classifier/dedup
rows per second and peak traced Python memory. It is infrastructure performance
evidence, not a signal-quality or profitability result.

The local smoke run on 2026-09-11 used seed `20260911` and 1,000 synthetic
metadata rows. It accepted all rows in 852,569,904 ns (1,172 rows/s) with
1,336,166 peak traced Python bytes. The raw result is
`build/benchmarks/gdelt-prompt-54.json`; this Python/tracemalloc result is not a
latency target and includes instrumentation overhead.
