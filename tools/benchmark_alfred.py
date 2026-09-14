#!/usr/bin/env python3
"""Benchmark deterministic ALFRED snapshot, index, and point-in-time queries."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import statistics
import sys
import time
import tracemalloc
from dataclasses import dataclass
from datetime import UTC, date, datetime, timedelta
from pathlib import Path
from typing import TYPE_CHECKING, Final

from aegis_mx_research.alfred import (
    APPROVED_SERIES,
    MICRO_UNITS,
    AlfredSeriesMetadata,
    AlfredSeriesSnapshot,
    AlfredValueStatus,
    AlfredVintageKind,
    AlfredVintageRecord,
    AlfredVintageStore,
    ReleaseTimePrecision,
)

if TYPE_CHECKING:
    from collections.abc import Callable, Sequence

SEED: Final = 20_260_911
MAX_ITERATIONS: Final = 10_000
MAX_OBSERVATIONS: Final = 10_000


@dataclass(frozen=True, slots=True)
class Measurement:
    """Raw monotonic samples and peak traced memory for one operation."""

    name: str
    iterations: int
    operation_records: int
    samples_ns: tuple[int, ...]
    peak_memory_bytes: int

    def to_dict(self) -> dict[str, object]:
        """Return exact samples and integer summary statistics."""
        ordered = tuple(sorted(self.samples_ns))
        elapsed = max(sum(self.samples_ns), 1)
        return {
            "iterations": self.iterations,
            "latency_ns": {
                "maximum": max(ordered),
                "p50": _percentile(ordered, 50),
                "p95": _percentile(ordered, 95),
                "p99": _percentile(ordered, 99),
            },
            "name": self.name,
            "operation_records": self.operation_records,
            "peak_memory_bytes_decimal": self.peak_memory_bytes,
            "raw_samples_ns": list(self.samples_ns),
            "throughput_records_per_second": (
                self.operation_records * self.iterations * 1_000_000_000
            )
            // elapsed,
        }


def _percentile(ordered: tuple[int, ...], percentile: int) -> int:
    index = ((len(ordered) - 1) * percentile) // 100
    return ordered[index]


def _measure(
    name: str,
    iterations: int,
    operation_records: int,
    operation: Callable[[], object],
) -> Measurement:
    operation()
    samples: list[int] = []
    tracemalloc.start()
    for _ in range(iterations):
        started = time.perf_counter_ns()
        operation()
        samples.append(time.perf_counter_ns() - started)
    _, peak = tracemalloc.get_traced_memory()
    tracemalloc.stop()
    return Measurement(
        name,
        iterations,
        operation_records,
        tuple(samples),
        peak,
    )


def _date_start_ns(value: date) -> int:
    seconds = int(datetime(value.year, value.month, value.day, tzinfo=UTC).timestamp())
    return seconds * 1_000_000_000


def _fixtures(
    observations: int,
) -> tuple[tuple[AlfredVintageRecord, ...], AlfredSeriesSnapshot, int]:
    spec = APPROVED_SERIES[0]
    records: list[AlfredVintageRecord] = []
    first_observation = date(2000, 1, 1)
    first_vintage = date(2020, 1, 1)
    receipt_ns = _date_start_ns(date(2026, 9, 11))
    for ordinal in range(observations):
        observation_date = first_observation + timedelta(days=ordinal)
        initial_vintage = first_vintage + timedelta(days=ordinal)
        revised_vintage = initial_vintage + timedelta(days=30)
        initial_known = _date_start_ns(initial_vintage + timedelta(days=1))
        revised_known = _date_start_ns(revised_vintage + timedelta(days=1))
        for revision, vintage_date, known_at, known_through in (
            (1, initial_vintage, initial_known, revised_known),
            (2, revised_vintage, revised_known, None),
        ):
            records.append(
                AlfredVintageRecord(
                    series_id=spec.series_id,
                    observation_date=observation_date,
                    observation_time_utc_ns=_date_start_ns(observation_date),
                    vintage_date=vintage_date,
                    revision_number=revision,
                    vintage_kind=(
                        AlfredVintageKind.INITIAL_RELEASE
                        if revision == 1
                        else AlfredVintageKind.REVISION
                    ),
                    value_status=AlfredValueStatus.OBSERVED,
                    value_microunits=(100 + ordinal + revision) * MICRO_UNITS,
                    native_units=spec.units,
                    release_date=None,
                    release_time_precision=ReleaseTimePrecision.NOT_AVAILABLE,
                    known_at_utc_ns=known_at,
                    known_through_utc_ns=known_through,
                    local_receipt_time_utc_ns=receipt_ns + ordinal,
                    processing_time_utc_ns=receipt_ns + observations + ordinal,
                    source_request_id=f"benchmark-source-{ordinal:05d}-{revision}",
                )
            )
    immutable_records = tuple(records)
    metadata = AlfredSeriesMetadata(
        series_id=spec.series_id,
        title=spec.title,
        units=spec.units,
        frequency=spec.frequency,
        seasonal_adjustment=spec.seasonal_adjustment,
        last_updated_utc_ns=receipt_ns,
        notes_sha256=hashlib.sha256(b"benchmark").hexdigest(),
        release_id=10,
        release_name=spec.release_name,
    )
    snapshot = AlfredSeriesSnapshot(
        plan_id=hashlib.sha256(f"alfred-benchmark:{SEED}".encode()).hexdigest(),
        spec=spec,
        metadata=metadata,
        releases=(),
        vintages=immutable_records,
        source_request_ids=("benchmark-request",),
    )
    cutoff = _date_start_ns(first_vintage + timedelta(days=observations + 31))
    return immutable_records, snapshot, cutoff


def _arguments(arguments: Sequence[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iterations", type=int, default=100)
    parser.add_argument("--observations", type=int, default=256)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("build/reports/benchmarks/alfred.json"),
    )
    parsed = parser.parse_args(arguments)
    if not 1 <= parsed.iterations <= MAX_ITERATIONS:
        parser.error(f"--iterations must be within [1, {MAX_ITERATIONS}]")
    if not 1 <= parsed.observations <= MAX_OBSERVATIONS:
        parser.error(f"--observations must be within [1, {MAX_OBSERVATIONS}]")
    return parsed


def main(arguments: Sequence[str] | None = None) -> int:
    """Run bounded local-only measurements and write raw JSON evidence."""
    parsed = _arguments(arguments)
    records, snapshot, cutoff = _fixtures(parsed.observations)
    store = AlfredVintageStore(records)

    def query() -> tuple[AlfredVintageRecord, ...]:
        result = store.as_known_at(cutoff, series_id="CPIAUCSL")
        if len(result) != parsed.observations:
            message = "ALFRED benchmark point-in-time query lost observations"
            raise RuntimeError(message)
        return result

    def serialize() -> bytes:
        payload = json.dumps(
            snapshot.to_dict(), separators=(",", ":"), sort_keys=True
        ).encode()
        hashlib.sha256(payload).digest()
        return payload

    measurements = (
        _measure(
            "vintage-index-build",
            parsed.iterations,
            len(records),
            lambda: AlfredVintageStore(records),
        ),
        _measure(
            "as-known-at-query",
            parsed.iterations,
            len(records),
            query,
        ),
        _measure(
            "canonical-snapshot-serialize-and-hash",
            parsed.iterations,
            len(records),
            serialize,
        ),
    )
    report = {
        "environment": {
            "logical_cpu_count": os.cpu_count(),
            "machine": platform.machine(),
            "operating_system": platform.platform(),
            "python": sys.version.split()[0],
        },
        "measurements": [item.to_dict() for item in measurements],
        "network_access_performed": False,
        "observation_count": parsed.observations,
        "record_count": len(records),
        "report_schema_version": "1.0.0",
        "sample_clock": "time.perf_counter_ns",
        "seed": SEED,
        "summary_median_ns": {
            item.name: int(statistics.median(item.samples_ns)) for item in measurements
        },
    }
    parsed.output.parent.mkdir(parents=True, exist_ok=True)
    parsed.output.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
