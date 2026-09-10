#!/usr/bin/env python3
"""Benchmark bounded POC usage, manifest verification, hashing, and admission."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import statistics
import sys
import tempfile
import time
import tracemalloc
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Final

from aegis_mx_research import (
    DECIMAL_GB,
    DataRepository,
    FileSystemState,
    ManifestLineage,
    ManifestTimeRange,
    QuotaEvidence,
    SourceManifest,
    StorageRequest,
    encode_manifest,
)

if TYPE_CHECKING:
    from collections.abc import Callable, Sequence

SEED: Final = 20260909
UNIVERSE_SHA256: Final = "cd" * 32


@dataclass(frozen=True, slots=True)
class Measurement:
    """One bounded benchmark result with raw monotonic samples."""

    name: str
    iterations: int
    operation_bytes: int
    samples_ns: tuple[int, ...]
    peak_memory_bytes: int

    def to_dict(self) -> dict[str, object]:
        """Return exact samples and stable summary statistics."""
        ordered = tuple(sorted(self.samples_ns))
        elapsed = sum(self.samples_ns)
        return {
            "iterations": self.iterations,
            "latency_ns": {
                "maximum": max(ordered),
                "p50": _percentile(ordered, 0.50),
                "p95": _percentile(ordered, 0.95),
                "p99": _percentile(ordered, 0.99),
            },
            "name": self.name,
            "operation_bytes_decimal": self.operation_bytes,
            "peak_memory_bytes_decimal": self.peak_memory_bytes,
            "raw_samples_ns": list(self.samples_ns),
            "throughput_bytes_per_second": (
                0
                if elapsed == 0
                else (self.operation_bytes * self.iterations * 1_000_000_000) // elapsed
            ),
        }


def _percentile(ordered: tuple[int, ...], percentile: float) -> int:
    index = max(0, min(len(ordered) - 1, round((len(ordered) - 1) * percentile)))
    return ordered[index]


def _measure(
    name: str,
    iterations: int,
    operation_bytes: int,
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
    return Measurement(name, iterations, operation_bytes, tuple(samples), peak)


def _time_range(index: int) -> ManifestTimeRange:
    start = 10 + index * 20
    return ManifestTimeRange(
        event_time_min_ns=start,
        event_time_max_ns=start + 1,
        publication_time_min_ns=start + 2,
        publication_time_max_ns=start + 3,
        receive_time_min_ns=start + 4,
        receive_time_max_ns=start + 5,
        processing_time_min_ns=start + 6,
        processing_time_max_ns=start + 7,
        revision_time_min_ns=start + 2,
        revision_time_max_ns=start + 3,
    )


def _prepare_repository(
    root: Path, file_count: int, object_bytes: int
) -> DataRepository:
    filesystem = FileSystemState(500 * DECIMAL_GB, 150 * DECIMAL_GB)
    repository = DataRepository(
        root,
        filesystem_probe=lambda _path: filesystem,
        git_worktree=Path.cwd(),
    )
    repository.initialize()
    manifest_dir = root / "manifests" / "source"
    manifest_dir.mkdir(parents=True, exist_ok=True)
    object_dir = root / "raw" / "benchmark"
    object_dir.mkdir(parents=True, exist_ok=True)
    for index in range(file_count):
        payload = bytes([index % 251]) * object_bytes
        relative = f"raw/benchmark/object-{index:06d}.bin"
        (root / relative).write_bytes(payload)
        manifest = SourceManifest(
            source_name="synthetic-benchmark",
            source_version="v1",
            source_object_id=f"object-{index:06d}",
            storage_path=relative,
            object_sha256=hashlib.sha256(payload).hexdigest(),
            size_bytes=len(payload),
            record_count=1,
            schema_name="benchmark-object",
            schema_version="1.0.0",
            times=_time_range(index),
            universe_snapshot_sha256=UNIVERSE_SHA256,
            lineage=ManifestLineage(),
        )
        (manifest_dir / f"{manifest.manifest_id}.json").write_bytes(
            encode_manifest(manifest)
        )
    return repository


def _arguments(arguments: Sequence[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iterations", type=int, default=100)
    parser.add_argument("--file-count", type=int, default=256)
    parser.add_argument("--object-bytes", type=int, default=4096)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("build/reports/benchmarks/data-repository.json"),
    )
    parsed = parser.parse_args(arguments)
    if parsed.iterations < 1 or parsed.file_count < 1 or parsed.object_bytes < 1:
        parser.error("iterations, file-count, and object-bytes must be positive")
    return parsed


def main(arguments: Sequence[str] | None = None) -> int:
    """Run deterministic local-only measurements and write a JSON report."""
    parsed = _arguments(arguments)
    with tempfile.TemporaryDirectory(prefix="aegis-data-benchmark-") as temporary:
        repository = _prepare_repository(
            Path(temporary) / "data",
            parsed.file_count,
            parsed.object_bytes,
        )
        if not repository.verify().passed:
            message = "benchmark fixture failed repository verification"
            raise RuntimeError(message)
        total_object_bytes = parsed.file_count * parsed.object_bytes
        quota = QuotaEvidence(
            limit_bytes=250 * DECIMAL_GB,
            used_bytes=10 * DECIMAL_GB,
            source="benchmark-authoritative-fixture",
            observed_at_utc="2026-09-09T16:00:00Z",
            authoritative=True,
        )
        request = StorageRequest("benchmark-admission", 1_000_000, 100_000, 100_000)
        hash_payload = b"a" * max(parsed.object_bytes, 1_048_576)
        measurements = (
            _measure(
                "recursive-usage-scan",
                parsed.iterations,
                total_object_bytes,
                repository.usage,
            ),
            _measure(
                "manifest-object-verification",
                parsed.iterations,
                total_object_bytes,
                repository.verify,
            ),
            _measure(
                "sha256-throughput",
                parsed.iterations,
                len(hash_payload),
                lambda: hashlib.sha256(hash_payload).digest(),
            ),
            _measure(
                "serialized-admission",
                parsed.iterations,
                total_object_bytes,
                lambda: repository.estimate(request, quota),
            ),
        )
    report = {
        "data_root_persisted": False,
        "environment": {
            "logical_cpu_count": os.cpu_count(),
            "machine": platform.machine(),
            "operating_system": platform.platform(),
            "python": sys.version.split()[0],
        },
        "file_count": parsed.file_count,
        "measurements": [measurement.to_dict() for measurement in measurements],
        "network_access_performed": False,
        "object_bytes_decimal": parsed.object_bytes,
        "report_schema_version": "1.0.0",
        "sample_clock": "time.perf_counter_ns",
        "seed": SEED,
        "tracemalloc_enabled": True,
        "summary_median_ns": {
            measurement.name: int(statistics.median(measurement.samples_ns))
            for measurement in measurements
        },
    }
    parsed.output.parent.mkdir(parents=True, exist_ok=True)
    parsed.output.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
