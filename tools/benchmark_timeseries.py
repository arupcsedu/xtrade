"""Deterministic smoke benchmark for the off-path time-series service core."""

from __future__ import annotations

import argparse
import json
import time
from dataclasses import asdict, dataclass
from functools import partial
from pathlib import Path
from typing import TYPE_CHECKING, Final

from aegis_mx_intelligence import (
    ConfigurationVersion,
    FeatureSnapshotId,
    GlobalEventId,
    Identifier128,
    InstrumentId,
    ModelId,
    ModelVersion,
    SessionId,
)
from aegis_mx_timeseries import (
    AdapterDevice,
    ContextBuilder,
    ContextBuildRequest,
    ForecastTarget,
    KnownFutureCovariate,
    ReferenceTimesFmAdapter,
    SessionState,
    TimeSeriesPoint,
    decode_context,
    encode_context,
)

if TYPE_CHECKING:
    from collections.abc import Callable

SEED: Final = 20_260_828


@dataclass(frozen=True, slots=True)
class TimingSummary:
    """Integer latency distribution for one bounded operation."""

    name: str
    iterations: int
    operations: int
    p50_ns: int
    p95_ns: int
    p99_ns: int
    throughput_per_second: int


def _identifier(seed: int) -> Identifier128:
    return Identifier128(seed, seed + 1)


def _context() -> bytes:
    history = tuple(
        TimeSeriesPoint(
            exchange_event_time_ns=1_800_000_000_000_000_000 + (index * 1_000),
            available_wall_clock_utc_ns=1_800_000_100_000_000_000 + (index * 1_000),
            value=((index % 7) - 3) * 100,
        )
        for index in range(128)
    )
    as_of = history[-1].exchange_event_time_ns
    point_in_time = history[-1].available_wall_clock_utc_ns
    horizons = (1_000, 2_000, 5_000, 10_000)
    context = ContextBuilder().build(
        ContextBuildRequest(
            instrument_id=InstrumentId(_identifier(1)),
            session_id=SessionId(_identifier(3)),
            feature_snapshot_id=FeatureSnapshotId(_identifier(5)),
            configuration_version=ConfigurationVersion(_identifier(7)),
            feature_definition_version=ConfigurationVersion(_identifier(9)),
            source_first_global_event_id=GlobalEventId(_identifier(11)),
            source_last_global_event_id=GlobalEventId(_identifier(13)),
            target=ForecastTarget.RETURN,
            as_of_exchange_event_time_ns=as_of,
            point_in_time_wall_clock_utc_ns=point_in_time,
            built_process_monotonic_time_ns=900,
            source_payload_sha256=bytes(range(32)),
            horizons_ns=horizons,
            quantiles_ppm=(100_000, 500_000, 900_000),
            history=history,
            future_covariates=tuple(
                KnownFutureCovariate(
                    horizon_ns=horizon,
                    exchange_event_time_ns=as_of + horizon,
                    known_wall_clock_utc_ns=point_in_time,
                    time_of_day_second=34_200 + index,
                    session_state=SessionState.OPEN,
                    scheduled_event_flags=index,
                )
                for index, horizon in enumerate(horizons)
            ),
        )
    )
    return encode_context(context)


def _percentile(ordered: list[int], percentile: int) -> int:
    index = ((len(ordered) - 1) * percentile) // 100
    return ordered[index]


def _measure(
    name: str,
    iterations: int,
    operations_per_iteration: int,
    operation: Callable[[], object],
) -> TimingSummary:
    for _ in range(8):
        operation()
    durations = []
    started = time.perf_counter_ns()
    for _ in range(iterations):
        sample_started = time.perf_counter_ns()
        operation()
        durations.append(time.perf_counter_ns() - sample_started)
    elapsed = max(1, time.perf_counter_ns() - started)
    ordered = sorted(durations)
    operations = iterations * operations_per_iteration
    return TimingSummary(
        name=name,
        iterations=iterations,
        operations=operations,
        p50_ns=_percentile(ordered, 50),
        p95_ns=_percentile(ordered, 95),
        p99_ns=_percentile(ordered, 99),
        throughput_per_second=(operations * 1_000_000_000) // elapsed,
    )


def run(iterations: int) -> dict[str, object]:
    """Run bounded serialization and reference-adapter batch categories."""
    if iterations <= 0:
        msg = "iterations must be positive"
        raise ValueError(msg)
    encoded = _context()
    context = decode_context(encoded)
    adapter = ReferenceTimesFmAdapter(
        ModelId(_identifier(101)), ModelVersion(_identifier(103))
    )
    summaries = [
        _measure("context_decode", iterations, 1, lambda: decode_context(encoded))
    ]
    for batch_size in (1, 8, 32):
        batch = (context,) * batch_size
        summaries.append(
            _measure(
                f"reference_adapter_batch_{batch_size}",
                iterations,
                batch_size,
                partial(adapter.forecast_batch, batch, AdapterDevice.CPU, None),
            )
        )
    return {
        "benchmark_scope": "infrastructure_only",
        "economic_value_claim": False,
        "seed": SEED,
        "summaries": [asdict(summary) for summary in summaries],
    }


def main() -> int:
    """Write stable-shape JSON without treating host timing as acceptance truth."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--iterations", type=int, default=100)
    parser.add_argument("--output", required=True, type=Path)
    arguments = parser.parse_args()
    result = run(arguments.iterations)
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":  # pragma: no cover - exercised as a benchmark command.
    raise SystemExit(main())
