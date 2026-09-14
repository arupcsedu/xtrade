# ADR 0058: SQLite spool and Parquet minute partitions

- Status: Accepted for the bounded forecasting POC
- Date: 2026-09-11
- Scope: Offline research data only
- Live-trading capability: None

## Context

Prompt 56 requires a streaming conversion from immutable provider objects into
deterministic, point-in-time minute partitions. The implementation must bound
memory, make duplicate handling independent of source order, recover after a
process crash, preserve exact integer price units, and remain query-compatible
with the existing in-memory point-in-time reference store.

The existing source corpus is already immutable and content-addressed, but its
legacy canonical output is newline JSON partitioned per ticker. That layout
creates small files and is not the accepted Prompt 56 dataset.

## Decision

Use two separate disk structures:

1. SQLite in `DELETE` journal mode with `synchronous=FULL` is the bounded,
   restartable normalization spool and the persistent implementation of the
   generic point-in-time store. Spool schema 1.2 compresses each versioned
   canonical document independently and uses 8 KiB pages; decoding is bounded
   to 64 KiB and rejects corrupt, truncated, oversized, or trailing streams.
   Explicit record-count and page-count limits fail closed. Logical records
   carry SHA-256 digests independent of compression and SQLite page layout.
2. Apache Parquet 2.6 written through pinned PyArrow 25.0.1 is the immutable
   analytical partition format. Every column uses Zstandard compression.
   Fixed Arrow fields, row order, row-group size, codec level, dictionary
   columns, and page version make output reproducible under the pinned build.

Partitions use `date=<date>/instrument_bucket=<bucket>`; a SHA-256 of the
stable `InstrumentId` selects one of eight buckets by default. A ticker never
appears in the physical path. An explicit quality report is published before
the Parquet object, and the generic immutable partition manifest is published
last. Only that last manifest means the partition is accepted.

The normalizer requires a bitemporal symbol mapping, instrument revision,
calendar revision, tick-size revision, and halt query. Missing or conflicting
reference data rejects a bar. Raw bars remain raw across corporate actions;
the known action IDs are provenance, not an instruction to rewrite history.

## Consequences

- Peak Python memory is bounded by one admitted source object and one Parquet
  batch, not by the full corpus.
- Identical duplicates are idempotent. The lexicographically smallest complete
  canonical representation is retained, so ingestion order does not change
  output. Conflicting economics for one instrument/minute abort the source
  transaction.
- Missing minutes and observed zero-volume bars are distinct. The system never
  forward-fills and never creates a zero-volume record.
- A quality report or Parquet object left by a crash is an unaccepted orphan;
  it is never mistaken for a dataset partition without the final manifest.
- SQLite is an offline indexing decision, not a hot-path dependency.
- A promotion obtains one admission/fencing lease for its complete projected
  output and reuses it across partitions. This avoids thousands of repeated
  full data-root scans while retaining single-writer fencing and the same
  fail-closed peak-usage check.
- The compressed spool remains temporary and must not exceed the POC's 20 GB
  workspace ceiling. A spool from an incompatible schema is fail-closed and
  must be preserved as failed-run evidence or removed through an approved
  cleanup plan before rebuilding.
- Reproducing byte-identical Parquet requires the pinned PyArrow version.

## Rejected alternatives

- Per-ticker JSONL was rejected because it creates many small files and weakens
  typed column/unit enforcement.
- An in-memory global sort was rejected because its memory grows with the
  complete dataset and cannot recover transactionally.
- A second vendor daily dataset was rejected; later session summaries must be
  derived from accepted minute partitions.
- DuckDB as the sole source of truth was rejected for this phase because the
  repository already has a narrow append/query contract and does not need a
  second database engine.

## Evidence

- [Canonical minute architecture](../architecture/canonical-minute-storage.md)
- [Recovery runbook](../operations/canonical-minute-recovery.md)
- [PyArrow Parquet writer API](https://arrow.apache.org/docs/python/generated/pyarrow.parquet.ParquetWriter.html)
- [PyArrow Parquet format guide](https://arrow.apache.org/docs/python/parquet.html)
