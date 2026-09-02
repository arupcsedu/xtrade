"""Deterministic smoke benchmark for point-in-time queries and leakage checks."""

from __future__ import annotations

import argparse
import hashlib
import json
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Final

from aegis_mx_research import (
    DatasetManifest,
    DatasetSample,
    DatasetSplit,
    DataSource,
    IndexMembership,
    LeakageValidator,
    PointInTimeRecord,
    PointInTimeStore,
    RecordKind,
    SourceKind,
    SplitMethod,
    ValidityInterval,
)

if TYPE_CHECKING:
    from collections.abc import Callable

SEED: Final = 20_260_831
WARMUP_ITERATIONS: Final = 8
MAX_ITERATIONS: Final = 100_000
MAX_RECORDS: Final = 10_000
BASE_NS: Final = 2_000_000_000_000_000_000


@dataclass(frozen=True, slots=True)
class TimingSummary:
    """Integer latency distribution for one bounded offline operation."""

    name: str
    iterations: int
    p50_ns: int
    p95_ns: int
    p99_ns: int
    maximum_ns: int
    throughput_per_second: int


def _percentile(values: list[int], percentile: int) -> int:
    ordered = sorted(values)
    index = ((len(ordered) - 1) * percentile) // 100
    return ordered[index]


def _time(name: str, operation: Callable[[], None], iterations: int) -> TimingSummary:
    for _ in range(WARMUP_ITERATIONS):
        operation()
    durations: list[int] = []
    started = time.perf_counter_ns()
    for _ in range(iterations):
        one_started = time.perf_counter_ns()
        operation()
        durations.append(time.perf_counter_ns() - one_started)
    elapsed = max(time.perf_counter_ns() - started, 1)
    return TimingSummary(
        name,
        iterations,
        _percentile(durations, 50),
        _percentile(durations, 95),
        _percentile(durations, 99),
        max(durations),
        (iterations * 1_000_000_000) // elapsed,
    )


def _fixtures(
    record_count: int,
) -> tuple[PointInTimeStore, DatasetManifest, int]:
    store = PointInTimeStore(record_count + 16)
    source = DataSource(
        source_id="benchmark-membership",
        kind=SourceKind.SYNTHETIC_REPLAY,
        provider="aegis-fixture",
        document_id="point-in-time-benchmark",
        content_sha256=hashlib.sha256(f"point-in-time:{SEED}".encode()).digest(),
        authenticated=True,
    )
    records: list[PointInTimeRecord] = []
    instruments: list[str] = []
    for ordinal in range(record_count):
        instrument_id = f"SYNTH-{ordinal:05d}"
        instruments.append(instrument_id)
        payload = IndexMembership("SYNTH-BENCHMARK-INDEX", instrument_id, included=True)
        records.append(
            PointInTimeRecord(
                record_id=f"membership-{ordinal:05d}",
                logical_key=f"SYNTH-BENCHMARK-INDEX:{instrument_id}",
                kind=RecordKind.INDEX_MEMBERSHIP,
                event_time_ns=BASE_NS - 1_000_000,
                publication_time_ns=BASE_NS - 2_000_000,
                receive_time_ns=BASE_NS - 1_900_000,
                processing_time_ns=BASE_NS - 1_800_000,
                revision_time_ns=BASE_NS - 2_000_000,
                validity=ValidityInterval(BASE_NS - 1_000_000, None),
                source=source,
                version=1,
                payload=payload,
            )
        )
    store.extend(records)
    record_ids = tuple(record.record_id for record in records)
    universe = tuple(instruments)
    samples = (
        DatasetSample(
            "train",
            BASE_NS,
            BASE_NS - 500_000,
            BASE_NS - 100_000,
            BASE_NS + 1,
            BASE_NS + 100,
            DatasetSplit.TRAIN,
            record_ids,
            universe,
        ),
        DatasetSample(
            "test",
            BASE_NS + 1_000_000,
            BASE_NS + 500_000,
            BASE_NS + 900_000,
            BASE_NS + 1_000_001,
            BASE_NS + 1_000_100,
            DatasetSplit.TEST,
            record_ids,
            universe,
        ),
    )
    manifest = DatasetManifest(
        "point-in-time-benchmark",
        BASE_NS + 2_000_000,
        SplitMethod.WALK_FORWARD,
        samples,
        "SYNTH-BENCHMARK-INDEX",
    )
    return store, manifest, BASE_NS


def main() -> int:
    """Run bounded timings and retain JSON evidence."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--iterations", type=int, default=100)
    parser.add_argument("--records", type=int, default=256)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    if not 1 <= arguments.iterations <= MAX_ITERATIONS:
        parser.error(f"--iterations must be within [1, {MAX_ITERATIONS}]")
    if not 1 <= arguments.records <= MAX_RECORDS:
        parser.error(f"--records must be within [1, {MAX_RECORDS}]")
    store, manifest, cutoff = _fixtures(arguments.records)
    validator = LeakageValidator()

    def query() -> None:
        if len(store.as_known_at(cutoff)) != arguments.records:
            msg = "point-in-time query benchmark lost a record"
            raise RuntimeError(msg)

    def validate() -> None:
        if not validator.validate(manifest, store).accepted:
            msg = "point-in-time benchmark fixture leaked"
            raise RuntimeError(msg)

    summaries = (
        _time("as_known_at", query, arguments.iterations),
        _time("leakage_validation", validate, arguments.iterations),
    )
    payload = {
        "schema": "aegis-mx-point-in-time-benchmark-v1",
        "seed": SEED,
        "record_count": arguments.records,
        "sample_count": len(manifest.samples),
        "accepted": True,
        "measurements": [asdict(summary) for summary in summaries],
    }
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    for summary in summaries:
        print(
            f"{summary.name}: p50={summary.p50_ns}ns p95={summary.p95_ns}ns "
            f"p99={summary.p99_ns}ns max={summary.maximum_ns}ns "
            f"throughput={summary.throughput_per_second}/s"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
