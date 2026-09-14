"""Synthetic infrastructure benchmark for the Prompt 57 dataset builder."""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import resource
import tempfile
import time
from dataclasses import replace
from datetime import UTC, date, datetime, timedelta
from pathlib import Path
from typing import Final, cast

from aegis_mx_intelligence import ConfigurationVersion, Identifier128, InstrumentId
from aegis_mx_research.canonical_minute import CanonicalMinuteRecord, MinuteDataQuality
from aegis_mx_research.data_repository import (
    DECIMAL_GB,
    DataRepository,
    FileSystemState,
    QuotaEvidence,
    StoragePolicy,
)
from aegis_mx_research.feature_dataset import (
    DATASET_SEED,
    FeatureDatasetBuilder,
    InMemoryCanonicalSource,
    build_and_publish_feature_dataset,
)
from aegis_mx_research.forecast_contracts import (
    ExchangeCalendar,
    StaticInstrumentResolver,
    TradingSession,
    parse_ticker_universe,
)

MINUTE_NS: Final = 60_000_000_000
DAY_NS: Final = 86_400_000_000_000
BASE_NS: Final = 1_700_000_000_000_000_000
MINIMUM_SESSIONS: Final = 87
MAXIMUM_SESSIONS: Final = 512
MAXIMUM_MINUTES: Final = 390
INSTRUMENT = InstrumentId(Identifier128(0xA57, 57))
INSTRUMENT_HEX: Final = cast("Identifier128", INSTRUMENT).hex()
SOURCE_SHA256: Final = hashlib.sha256(b"prompt-57-benchmark").hexdigest()


def _digest(value: object) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def _calendar(sessions: int, minutes: int) -> ExchangeCalendar:
    version = ConfigurationVersion(Identifier128(0xCA1, 57))
    values = tuple(
        TradingSession(
            f"BENCH-{index:04d}",
            BASE_NS + index * DAY_NS,
            BASE_NS + index * DAY_NS + minutes * MINUTE_NS,
        )
        for index in range(sessions)
    )
    return ExchangeCalendar("BENCH-XNYS", version, values)


def _record(session: int, minute: int, minutes: int) -> CanonicalMinuteRecord:
    opened = BASE_NS + session * DAY_NS
    start = opened + minute * MINUTE_NS
    price = 10_000 + session + minute % 7
    provisional = CanonicalMinuteRecord(
        instrument_id=INSTRUMENT_HEX,
        source_ticker="BENCH",
        market_id="SYNTHETIC",
        session_id=f"BENCH-{session:04d}",
        trading_date=date(2026, 1, 1) + timedelta(days=session),
        minute_start_exchange_time_ns=start,
        minute_end_exchange_time_ns=start + MINUTE_NS,
        session_open_exchange_time_ns=opened,
        session_close_exchange_time_ns=opened + minutes * MINUTE_NS,
        currency="USD",
        tick_value_currency_nanos=10_000_000,
        open_ticks=price,
        high_ticks=price + 2,
        low_ticks=price - 2,
        close_ticks=price + 1,
        volume_shares=100 + minute,
        trade_count=10,
        vwap_ticks=price,
        source_publication_time_ns=start + MINUTE_NS + 1,
        source_availability_time_ns=start + MINUTE_NS + 2,
        local_receipt_time_ns=start + MINUTE_NS + 3,
        local_processing_time_ns=start + MINUTE_NS + 4,
        data_quality_state=MinuteDataQuality.VALID,
        data_quality_reasons=(),
        source_manifest_id="source-" + "11" * 32,
        source_object_id="prompt-57-benchmark",
        source_record_id=f"BENCH:{session}:{minute}",
        mapping_id="mapping-benchmark-v1",
        instrument_revision_id="instrument-benchmark-v1",
        tick_revision_id="tick-benchmark-v1",
        corporate_action_ids=(),
        reference_sha256=SOURCE_SHA256,
        record_sha256="",
    )
    return replace(
        provisional,
        record_sha256=_digest(provisional.payload(include_hash=False)),
    )


def run_benchmark(sessions: int, minutes: int) -> dict[str, object]:
    """Build and publish one bounded synthetic dataset and return raw metrics."""
    if not MINIMUM_SESSIONS <= sessions <= MAXIMUM_SESSIONS:
        message = f"sessions must be {MINIMUM_SESSIONS}..{MAXIMUM_SESSIONS}"
        raise ValueError(message)
    if not 0 < minutes <= MAXIMUM_MINUTES:
        message = f"minutes must be 1..{MAXIMUM_MINUTES}"
        raise ValueError(message)
    calendar = _calendar(sessions, minutes)
    universe = parse_ticker_universe(
        b"BENCH\n", StaticInstrumentResolver({"BENCH": INSTRUMENT})
    )
    records = tuple(
        _record(session, minute, minutes)
        for session in range(sessions)
        for minute in range(minutes)
    )
    source = InMemoryCanonicalSource(
        records,
        maximum_records=len(records) + 1,
        source_partition_ids=("partition-prompt-57-benchmark",),
    )
    builder = FeatureDatasetBuilder(
        universe, calendar, maximum_records_per_instrument=len(records) + 1
    )
    rss_before = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    with tempfile.TemporaryDirectory(prefix="aegis-prompt57-") as temporary:
        repository = DataRepository(
            Path(temporary) / "data",
            policy=StoragePolicy(),
            filesystem_probe=lambda _path: FileSystemState(
                total_bytes=500 * DECIMAL_GB,
                free_bytes=400 * DECIMAL_GB,
            ),
            git_worktree=Path.cwd(),
        )
        repository.initialize()
        quota = QuotaEvidence(
            limit_bytes=10 * (1 << 40),
            used_bytes=1_000_000,
            source="synthetic-benchmark-quota",
            observed_at_utc="2026-09-12T12:00:00Z",
            authoritative=True,
        )
        started = time.perf_counter_ns()
        published = build_and_publish_feature_dataset(
            repository, quota, builder, source
        )
        elapsed_ns = time.perf_counter_ns() - started
        usage = repository.usage()
        rss_after = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return {
        "dataset_id": published.dataset_id,
        "dataset_seed": DATASET_SEED,
        "economic_value_claimed": False,
        "elapsed_ns": elapsed_ns,
        "feature_rows": published.sample_count,
        "feature_rows_per_second": (
            published.sample_count * 1_000_000_000 / elapsed_ns
        ),
        "hardware": {
            "machine": platform.machine(),
            "processor": platform.processor() or "unreported",
            "python": platform.python_version(),
            "system": platform.system(),
        },
        "input_minute_rows": len(records),
        "label_rows": published.sample_count * 12,
        "labels_per_second": published.sample_count * 12 * 1_000_000_000 / elapsed_ns,
        "leakage_status": published.leakage.status,
        "live_trading_capable": False,
        "manifest_sha256": published.manifest_sha256,
        "minutes_per_session": minutes,
        "object_count": published.object_count,
        "peak_rss_delta_kib": max(rss_after - rss_before, 0),
        "published_logical_bytes": usage.logical_bytes,
        "schema_version": "1.0.0",
        "session_summaries": published.session_summary_count,
        "sessions": sessions,
        "synthetic_infrastructure_only": True,
        "timestamp_utc": datetime.now(UTC).isoformat(),
    }


def main() -> int:
    """Run the bounded CLI benchmark and optionally retain its raw JSON."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sessions", type=int, default=90)
    parser.add_argument("--minutes", type=int, default=61)
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    report = run_benchmark(arguments.sessions, arguments.minutes)
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if arguments.output is not None:
        arguments.output.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        arguments.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
