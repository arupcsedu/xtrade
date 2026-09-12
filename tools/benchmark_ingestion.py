#!/usr/bin/env python3
"""Benchmark the bounded, socket-free provider-neutral ingestion boundary."""

from __future__ import annotations

import argparse
import json
import platform
import statistics
import sys
import tempfile
import time
import tracemalloc
from dataclasses import dataclass
from datetime import UTC, date, datetime
from pathlib import Path
from typing import TYPE_CHECKING, Final

from aegis_mx_research import (
    DECIMAL_GB,
    DataEntitlement,
    DataEntitlementState,
    DataRepository,
    DeterministicMockHttpS3Provider,
    DownloadCheckpoint,
    FetchRequest,
    FileSystemState,
    IngestionCoordinator,
    ManifestTimeRange,
    MockObject,
    QuotaEvidence,
    RateLimitPolicy,
    RetryPolicy,
)

if TYPE_CHECKING:
    from collections.abc import Callable, Sequence

SEED: Final = 20260831
UNIVERSE_SHA256: Final = "ab" * 32
POLICY_SHA256: Final = "cd" * 32
APPROVAL_SHA256: Final = "ef" * 32


@dataclass(frozen=True, slots=True)
class Measurement:
    """Raw monotonic samples and process-local allocation evidence."""

    name: str
    operation_bytes: int
    samples_ns: tuple[int, ...]
    peak_memory_bytes: int

    def to_dict(self) -> dict[str, object]:
        """Return labeled latency and throughput statistics."""
        ordered = tuple(sorted(self.samples_ns))
        elapsed = sum(ordered)
        return {
            "iterations": len(ordered),
            "latency_ns": {
                "maximum": ordered[-1],
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
                else self.operation_bytes * len(ordered) * 1_000_000_000 // elapsed
            ),
        }


def _percentile(ordered: tuple[int, ...], percentile: float) -> int:
    index = round((len(ordered) - 1) * percentile)
    return ordered[index]


def _measure(
    name: str,
    operation_bytes: int,
    iterations: int,
    operation: Callable[[int], object],
) -> Measurement:
    operation(-1)
    samples: list[int] = []
    tracemalloc.start()
    for index in range(iterations):
        started = time.perf_counter_ns()
        operation(index)
        samples.append(time.perf_counter_ns() - started)
    _, peak = tracemalloc.get_traced_memory()
    tracemalloc.stop()
    return Measurement(name, operation_bytes, tuple(samples), peak)


def _repository(root: Path) -> DataRepository:
    return DataRepository(
        root,
        filesystem_probe=lambda _path: FileSystemState(
            total_bytes=500 * DECIMAL_GB,
            free_bytes=200 * DECIMAL_GB,
        ),
        git_worktree=Path.cwd(),
    )


def _quota() -> QuotaEvidence:
    return QuotaEvidence(
        limit_bytes=250 * DECIMAL_GB,
        used_bytes=10 * DECIMAL_GB,
        source="ingestion-benchmark-fixture",
        observed_at_utc="2026-09-09T16:00:00Z",
        authoritative=True,
    )


def _fixture(payload_bytes: int) -> tuple[DeterministicMockHttpS3Provider, MockObject]:
    payload = b"x" * (payload_bytes - 1) + b"\n"
    base = 1_789_000_000_000_000_000
    item = MockObject(
        locator="http/mock-bucket/aapl/2026-09-08",
        coverage_date=date(2026, 9, 8),
        tickers=("AAPL",),
        payload=payload,
        times=ManifestTimeRange(
            event_time_min_ns=base,
            event_time_max_ns=base + 1,
            publication_time_min_ns=base + 2,
            publication_time_max_ns=base + 3,
            receive_time_min_ns=base + 4,
            receive_time_max_ns=base + 5,
            processing_time_min_ns=base + 6,
            processing_time_max_ns=base + 7,
            revision_time_min_ns=base + 2,
            revision_time_max_ns=base + 3,
        ),
    )
    return (
        DeterministicMockHttpS3Provider(
            provider_id="deterministic-mock",
            dataset_id="mock-minute-bars",
            objects=(item,),
        ),
        item,
    )


def _coordinator(
    root: Path, provider: DeterministicMockHttpS3Provider
) -> IngestionCoordinator:
    return IngestionCoordinator(
        _repository(root),
        provider,
        DataEntitlement(
            source_id=provider.provider_id,
            dataset_id=provider.dataset_id,
            state=DataEntitlementState.AUTHORIZED,
            network_access_authorized=True,
            policy_sha256=POLICY_SHA256,
            approval_record_sha256=APPROVAL_SHA256,
            expires_at_utc=datetime(2099, 1, 1, tzinfo=UTC),
        ),
        rate_limit=RateLimitPolicy(
            requests_per_window=1_000_000,
            window_ns=1,
            maximum_wait_ns=1_000_000,
        ),
        retry=RetryPolicy(
            maximum_attempts=1,
            base_delay_ns=0,
            maximum_delay_ns=0,
            maximum_jitter_ns=0,
            deterministic_seed=SEED,
        ),
    )


def _request(
    provider: DeterministicMockHttpS3Provider, request_id: str, *, execute: bool
) -> FetchRequest:
    return FetchRequest(
        request_id=request_id,
        provider_id=provider.provider_id,
        dataset_id=provider.dataset_id,
        start_date=date(2026, 9, 8),
        end_date=date(2026, 9, 8),
        tickers=("AAPL",),
        universe_snapshot_sha256=UNIVERSE_SHA256,
        execute=execute,
        deterministic_seed=SEED,
    )


def _arguments(arguments: Sequence[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iterations", type=int, default=25)
    parser.add_argument("--object-bytes", type=int, default=65_536)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("build/reports/benchmarks/ingestion.json"),
    )
    parsed = parser.parse_args(arguments)
    if parsed.iterations < 1 or parsed.object_bytes < 1:
        parser.error("iterations and object-bytes must be positive")
    return parsed


def main(arguments: Sequence[str] | None = None) -> int:
    """Run bounded local measurements and persist all raw samples."""
    parsed = _arguments(arguments)
    with tempfile.TemporaryDirectory(prefix="aegis-ingestion-benchmark-") as temporary:
        root = Path(temporary)
        provider, item = _fixture(parsed.object_bytes)
        planner = _coordinator(root / "plan", provider)
        plan_request = _request(provider, "benchmark-plan", execute=False)
        plan_measurement = _measure(
            "dry-run-plan",
            parsed.object_bytes,
            parsed.iterations,
            lambda _index: planner.plan(plan_request, _quota()),
        )

        def execute(index: int) -> object:
            run_provider, _ = _fixture(parsed.object_bytes)
            coordinator = _coordinator(root / f"execute-{index}", run_provider)
            request = _request(run_provider, f"benchmark-execute-{index}", execute=True)
            return coordinator.execute(coordinator.plan(request, _quota()), _quota())

        execute_measurement = _measure(
            "mock-fetch-hash-checkpoint-publish",
            parsed.object_bytes,
            parsed.iterations,
            execute,
        )
        source = provider.plan(plan_request)[0]
        checkpoint = DownloadCheckpoint(
            plan_id=planner.plan(plan_request, _quota()).plan_id,
            request_id=plan_request.request_id,
            object_id=source.object_id,
            provider_id=source.provider_id,
            dataset_id=source.dataset_id,
            locator_sha256=UNIVERSE_SHA256,
            version_id=item.version_id,
            expected_size_bytes=parsed.object_bytes,
            completed_bytes=0,
            prefix_sha256=UNIVERSE_SHA256,
            attempts=0,
            sequence=0,
        )
        checkpoint_measurement = _measure(
            "checkpoint-encode",
            len(checkpoint.encode()),
            parsed.iterations,
            lambda _index: checkpoint.encode(),
        )

    measurements = (
        plan_measurement,
        execute_measurement,
        checkpoint_measurement,
    )
    report = {
        "actual_network_access_performed": False,
        "environment": {
            "machine": platform.machine(),
            "python": sys.version.split()[0],
        },
        "measurements": [measurement.to_dict() for measurement in measurements],
        "mock_remote_boundary_exercised": True,
        "object_bytes_decimal": parsed.object_bytes,
        "report_schema_version": "1.0.0",
        "sample_clock": "time.perf_counter_ns",
        "seed": SEED,
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
