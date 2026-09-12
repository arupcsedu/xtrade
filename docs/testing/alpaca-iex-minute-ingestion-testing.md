# Alpaca IEX minute-ingestion testing

Normal tests never reach Alpaca. An injectable deterministic transport covers
clock, calendar, asset, bar, missing-asset, pagination, and malformed-response
behavior. The integration suite exercises the real repository with temporary
owner-only paths. It injects interruption immediately after final-page
publication and proves resume performs no duplicate network fetch.
It also injects termination during `fsync` and proves the final path is absent,
the owned incomplete temporary is removed, and the stable interruption code is
reported.

Required local gates:

```bash
export PYTHONPATH=python/research:.
/scratch/djy8hg/env/aegis_mx_contracts/bin/python -m ruff check \
  python/research/aegis_mx_research/alpaca_historical.py \
  python/tests/test_alpaca_historical.py
/scratch/djy8hg/env/aegis_mx_contracts/bin/python -m mypy --strict \
  python/research/aegis_mx_research/alpaca_historical.py
/scratch/djy8hg/env/aegis_mx_contracts/bin/python -m pytest -q \
  python/tests/test_alpaca_historical.py --no-cov
bash -n tools/slurm/alpaca-iex-minute-backfill.sbatch
```

Remote tests are explicit operator runs and must retain the immutable report,
provider request/retry counts, exact market dates, object and partition counts,
bytes, coverage gaps, quota evidence, sample seed, and verification hashes.
They are not part of ordinary CI and never submit orders.

## 2026-09-11 validation evidence

- The focused adapter suite passed 89 tests in 90.10 seconds and measured 100%
  statement and branch coverage over 1,033 statements and 290 branches.
- The combined adapter, storage-repository, source-policy, and provider-neutral
  ingestion suite passed 261 tests in 89.24 seconds. All HTTP behavior in that
  suite used injected socket-free transports.
- Ruff formatting and linting passed, strict mypy passed for the adapter and
  tests, the schema check passed through v1.9 with 204 generated files and six
  goldens, and the documentation check validated 203 documents, 401 links, and
  25 Mermaid sources.
- Security validation generated a 61-component SBOM and passed 20 policy tests;
  the checked-in source policy remained deny-by-default while the owner-only
  external policy enabled exactly one source.
- With seeds retained in their reports, the provider-neutral benchmark measured
  median mock fetch/hash/checkpoint/publication latency of 20,135,589 ns and the
  repository benchmark measured median SHA-256 latency of 2,251,733 ns per
  1,048,576-byte object. These login-node measurements are infrastructure smoke
  results, not provider-network or production performance claims.

`shellcheck` was unavailable in the interactive environment; `bash -n` passed
for the Slurm script, and CI remains responsible for the mandatory ShellCheck
gate. A repository-wide warnings-as-errors C++ rebuild was intentionally
stopped after unrelated targets began recompiling; the Alpaca acquisition path
is Python-only and its targeted checks above completed.

The remote backfill Slurm job `19595328` completed with exit `0:0` in 1:38:04.
It produced 9,462,709 canonical records in 36,520 partitions across 501
sessions. The exact-set verifier proves that those partitions plus 3,059
explicit missing pairs equal all 79 × 501 requested pairs. Two seed-`20260911`
verification reports are byte-identical with file SHA-256
`9aa9745b476e664e5a811b0ed69ea9a080afc655805509442369afb6c782f496`
and deterministic reinspection SHA-256
`b6b0191c400221d765e972dd1782f774f177dfaaf4a9d2ba60ab0f76f456e973`.
Full repository verification checked 38,783 manifests and objects without an
error. Final logical usage is 7,782,178,266 bytes; one preserved 71,308-byte
temporary is documented recovery evidence, not an unaccounted partial.
For the final attempt, the report records 1,399 provider requests, zero retries,
about 14.28 requests/minute, 1,609 canonical records/second, and 1.13 MB/second
of canonical publication. Scheduler `MaxRSS` was 7.38 GiB within the 16,000 MB
allocation. These end-to-end figures include network latency and synchronous
filesystem durability; they are not a hot-path benchmark.
