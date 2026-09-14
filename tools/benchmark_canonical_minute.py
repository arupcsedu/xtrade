"""Bounded memory/storage benchmark for Prompt 56 offline primitives."""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import resource
import tempfile
import time
from datetime import UTC, datetime
from decimal import Decimal
from pathlib import Path
from typing import TYPE_CHECKING, Final

from aegis_mx_research.canonical_minute import (
    NANOSECONDS_PER_MINUTE,
    CanonicalMinuteNormalizer,
    CanonicalMinuteSpool,
    RawMinuteBar,
    RawPriceEncoding,
    ResolvedMinuteReference,
    SpoolIngestResult,
    write_partition_parquet,
)
from aegis_mx_research.data_repository import (
    ManifestLineage,
    ManifestTimeRange,
    SourceManifest,
)
from aegis_mx_research.store import PersistentPointInTimeStore
from aegis_mx_research.types import (
    DataSource,
    PointInTimeRecord,
    RecordKind,
    SourceKind,
    SymbolMapping,
    ValidityInterval,
)

if TYPE_CHECKING:
    from collections.abc import Iterator

SEED: Final = 20_260_831
EVENT_NS: Final = 1_789_133_400_000_000_000
SESSION_MINUTES: Final = 390
SOURCE_SHA256: Final = hashlib.sha256(b"prompt-56-benchmark-source").hexdigest()
UNIVERSE_SHA256: Final = hashlib.sha256(b"prompt-56-benchmark-universe").hexdigest()


class _Resolver:
    def resolve(
        self, symbol: str, *, event_time_ns: int, known_at_ns: int
    ) -> ResolvedMinuteReference:
        del known_at_ns
        instrument_id = hashlib.sha256(f"fixture:{symbol}".encode()).hexdigest()[:32]
        return ResolvedMinuteReference(
            instrument_id=instrument_id,
            market_id="SYNTHETIC",
            mapping_id=f"mapping:{symbol}:v1",
            instrument_revision_id=f"instrument:{symbol}:v1",
            tick_revision_id=f"tick:{symbol}:v1",
            tick_value_currency_nanos=10_000_000,
            currency="USD",
            session_id=hashlib.sha256(b"2026-09-11-session").hexdigest()[:32],
            trading_date=datetime.fromtimestamp(
                event_time_ns // 1_000_000_000, tz=UTC
            ).date(),
            session_open_ns=EVENT_NS,
            session_close_ns=EVENT_NS + SESSION_MINUTES * NANOSECONDS_PER_MINUTE,
            corporate_action_ids=(),
            reference_sha256=hashlib.sha256(b"fixture-reference").hexdigest(),
            quality_reasons=(),
        )


def _manifest(count: int) -> SourceManifest:
    return SourceManifest(
        source_name="prompt-56-benchmark",
        source_version="v1",
        source_object_id="prompt-56-benchmark-object",
        storage_path=f"raw/benchmark/{SOURCE_SHA256}.json",
        object_sha256=SOURCE_SHA256,
        size_bytes=count * 128,
        record_count=count,
        schema_name="synthetic-minute-bars-jsonl",
        schema_version="1.0.0",
        times=ManifestTimeRange(
            event_time_min_ns=EVENT_NS,
            event_time_max_ns=EVENT_NS + (SESSION_MINUTES - 1) * NANOSECONDS_PER_MINUTE,
            publication_time_min_ns=EVENT_NS,
            publication_time_max_ns=EVENT_NS,
            receive_time_min_ns=EVENT_NS,
            receive_time_max_ns=EVENT_NS,
            processing_time_min_ns=EVENT_NS + 1,
            processing_time_max_ns=EVENT_NS + 1,
            revision_time_min_ns=EVENT_NS,
            revision_time_max_ns=EVENT_NS,
        ),
        universe_snapshot_sha256=UNIVERSE_SHA256,
        lineage=ManifestLineage(),
    )


def _bars(count: int, manifest_id: str) -> Iterator[RawMinuteBar]:
    for index in range(count):
        symbol_index, minute = divmod(index, SESSION_MINUTES)
        base = Decimal(10_000 + symbol_index * 10)
        yield RawMinuteBar(
            symbol=f"S{symbol_index:04d}",
            minute_start_exchange_time_ns=(EVENT_NS + minute * NANOSECONDS_PER_MINUTE),
            open_value=base / 100,
            high_value=(base + 2) / 100,
            low_value=(base - 2) / 100,
            close_value=(base + (minute % 3) - 1) / 100,
            volume=100 + minute,
            trade_count=10,
            vwap_value=base / 100,
            price_encoding=RawPriceEncoding.CURRENCY_UNITS_DECIMAL,
            source_manifest_id=manifest_id,
            source_object_id="prompt-56-benchmark-object",
            source_record_id=f"record:{index}",
            source_publication_time_ns=EVENT_NS,
            source_availability_time_ns=EVENT_NS,
            local_receipt_time_ns=EVENT_NS,
            local_processing_time_ns=EVENT_NS + 1,
            market_id="SYNTHETIC",
        )


def _pit_record(index: int) -> PointInTimeRecord:
    symbol = f"P{index:06d}"
    payload = SymbolMapping(symbol, f"instrument-{index}", "SYNTHETIC")
    return PointInTimeRecord(
        record_id=f"pit-{index}",
        logical_key=symbol,
        kind=RecordKind.SYMBOL_MAPPING,
        event_time_ns=EVENT_NS,
        publication_time_ns=EVENT_NS,
        receive_time_ns=EVENT_NS,
        processing_time_ns=EVENT_NS + 1,
        revision_time_ns=EVENT_NS,
        validity=ValidityInterval(EVENT_NS, None),
        source=DataSource(
            source_id=f"source-{index}",
            kind=SourceKind.SYNTHETIC_REPLAY,
            provider="prompt-56-benchmark",
            document_id=f"document-{index}",
            content_sha256=hashlib.sha256(f"pit:{index}".encode()).digest(),
            authenticated=True,
        ),
        version=1,
        payload=payload,
    )


def run_benchmark(rows: int, pit_records: int, pit_queries: int) -> dict[str, object]:
    """Run one bounded benchmark and return machine-readable results."""
    if rows <= 0 or pit_records <= 0 or pit_queries <= 0:
        message = "benchmark counts must be positive"
        raise ValueError(message)
    manifest = _manifest(rows)
    normalizer = CanonicalMinuteNormalizer(_Resolver())
    usage_before = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    with tempfile.TemporaryDirectory(prefix="aegis-prompt56-") as temporary:
        root = Path(temporary)
        with CanonicalMinuteSpool(
            root / "minute-spool.sqlite3",
            instrument_buckets=1,
            maximum_records=rows + 1,
            maximum_bytes=max(64_000_000, rows * 16_384),
        ) as spool:
            ingest_started = time.perf_counter_ns()
            ingest: SpoolIngestResult = spool.ingest(
                manifest, _bars(rows, manifest.manifest_id), normalizer
            )
            ingest_elapsed = time.perf_counter_ns() - ingest_started
            parquet_started = time.perf_counter_ns()
            parquet = write_partition_parquet(
                spool,
                "2026-09-11",
                0,
                root / "partition.parquet",
                batch_rows=min(65_536, rows),
            )
            parquet_elapsed = time.perf_counter_ns() - parquet_started
            spool_bytes = spool.path.stat().st_size

        with PersistentPointInTimeStore(
            root / "pit.sqlite3",
            capacity=pit_records + 1,
            maximum_bytes=max(4_000_000, pit_records * 4_096),
        ) as store:
            for index in range(pit_records):
                store.append(_pit_record(index))
            query_started = time.perf_counter_ns()
            for _ in range(pit_queries):
                store.as_known_at(EVENT_NS + 1)
            query_elapsed = time.perf_counter_ns() - query_started
            pit_hash = store.deterministic_hash()
            pit_bytes = store.path.stat().st_size

        usage_after = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        return {
            "economic_value_claimed": False,
            "hardware": {
                "machine": platform.machine(),
                "processor": platform.processor() or "unreported",
                "python": platform.python_version(),
                "system": platform.system(),
            },
            "live_trading_capable": False,
            "normalization": {
                "elapsed_ns": ingest_elapsed,
                "inserted_records": ingest.inserted_records,
                "peak_rss_delta_kib": max(usage_after - usage_before, 0),
                "rows": rows,
                "rows_per_second": rows * 1_000_000_000 / ingest_elapsed,
                "spool_bytes": spool_bytes,
                "spool_bytes_per_row": spool_bytes / rows,
            },
            "parquet": {
                "bytes": parquet.size_bytes,
                "bytes_per_row": parquet.size_bytes / rows,
                "elapsed_ns": parquet_elapsed,
                "object_sha256": parquet.sha256,
                "rows_per_second": rows * 1_000_000_000 / parquet_elapsed,
            },
            "persistent_point_in_time": {
                "database_bytes": pit_bytes,
                "logical_sha256": pit_hash,
                "query_elapsed_ns": query_elapsed,
                "query_operations": pit_queries,
                "queries_per_second": pit_queries * 1_000_000_000 / query_elapsed,
                "records": pit_records,
            },
            "schema_version": "1.0.0",
            "seed": SEED,
        }


def main() -> int:
    """Run the benchmark CLI."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--rows", type=int, default=12_480)
    parser.add_argument("--pit-records", type=int, default=1_000)
    parser.add_argument("--pit-queries", type=int, default=100)
    values = parser.parse_args()
    report = run_benchmark(values.rows, values.pit_records, values.pit_queries)
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
