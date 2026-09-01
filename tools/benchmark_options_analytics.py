"""Deterministic smoke benchmark for the off-path options signal service."""

from __future__ import annotations

import argparse
import json
import time
from dataclasses import asdict, dataclass, replace
from datetime import UTC, datetime
from pathlib import Path
from typing import TYPE_CHECKING, Final

from aegis_mx_intelligence import (
    CarryObservation,
    ConfigurationVersion,
    ExerciseStyle,
    FeatureSnapshotId,
    GlobalEventId,
    Identifier128,
    InstrumentId,
    ModelId,
    ModelVersion,
    OpenInterestObservation,
    OptionContract,
    OptionQuote,
    OptionRight,
    OptionsAnalyticsConfig,
    OptionsAnalyticsEngine,
    OptionsDecision,
    OptionsInputBundle,
    OptionTrade,
    SessionId,
    SettlementStyle,
    SyntheticSurfaceGenerator,
    UnderlyingObservation,
    VenueId,
    VolatilitySurfaceConfig,
)

if TYPE_CHECKING:
    from collections.abc import Callable

SEED: Final = 20_260_828
WARMUP_ITERATIONS: Final = 8
MAX_ITERATIONS: Final = 100_000
SPOT_NANOS: Final = 100_000_000_000
UNDERLYING_ID: Final = InstrumentId(Identifier128(1, 2))


@dataclass(frozen=True, slots=True)
class TimingSummary:
    """Integer latency distribution for one bounded operation."""

    name: str
    iterations: int
    p50_ns: int
    p95_ns: int
    p99_ns: int
    throughput_per_second: int


def _identifier(seed: int) -> Identifier128:
    return Identifier128(seed, seed + 1)


def _wall_time_ns(year: int, month: int, day: int, hour: int = 0) -> int:
    return int(datetime(year, month, day, hour, tzinfo=UTC).timestamp() * 1e9)


def _contract(
    ordinal: int,
    month: int,
    day: int,
    strike: int,
    right: OptionRight,
) -> OptionContract:
    strike_milli = strike * 1_000
    symbol = (
        f"BENCH 26{month:02d}{day:02d}"
        f"{'C' if right is OptionRight.CALL else 'P'}{strike_milli:08d}"
    )
    return OptionContract(
        GlobalEventId(_identifier(100 + ordinal)),
        InstrumentId(_identifier(200 + ordinal)),
        UNDERLYING_ID,
        symbol,
        "BENCH",
        right,
        ExerciseStyle.EUROPEAN,
        SettlementStyle.PHYSICAL,
        strike * 1_000_000_000,
        100,
        _wall_time_ns(2026, month, day, 21),
        1,
        0,
        100,
        _wall_time_ns(2026, 8, 1),
    )


def _fixture() -> tuple[OptionsAnalyticsEngine, OptionsInputBundle]:
    as_of = _wall_time_ns(2026, 9, 1, 14)
    contracts = tuple(
        _contract(index, month, day, strike, right)
        for index, (month, day, strike, right) in enumerate(
            (
                (9, 8, 90, OptionRight.PUT),
                (9, 8, 100, OptionRight.PUT),
                (9, 8, 110, OptionRight.PUT),
                (9, 8, 90, OptionRight.CALL),
                (9, 8, 100, OptionRight.CALL),
                (9, 8, 110, OptionRight.CALL),
                (10, 1, 90, OptionRight.PUT),
                (10, 1, 100, OptionRight.PUT),
                (10, 1, 110, OptionRight.PUT),
                (10, 1, 90, OptionRight.CALL),
                (10, 1, 100, OptionRight.CALL),
                (10, 1, 110, OptionRight.CALL),
            ),
            start=1,
        )
    )
    venue = VenueId(_identifier(2))
    underlying = UnderlyingObservation(
        UNDERLYING_ID,
        SPOT_NANOS,
        as_of - 1_000,
        as_of - 500,
        as_of,
    )
    carry = CarryObservation(20_000, 5_000, as_of - 1, as_of)
    placeholder = OptionQuote(
        contracts[0].contract_id,
        venue,
        1,
        2,
        1,
        1,
        as_of - 1_000,
        as_of - 500,
        as_of,
    )
    template = OptionsInputBundle(
        SessionId(_identifier(3)),
        UNDERLYING_ID,
        FeatureSnapshotId(_identifier(4)),
        ConfigurationVersion(_identifier(5)),
        contracts,
        (placeholder,),
        tuple(
            OptionTrade(item.contract_id, 1_000_000_000, 10, as_of - 10, as_of)
            for item in contracts
        ),
        tuple(
            OpenInterestObservation(item.contract_id, 1_000, as_of - 1, as_of)
            for item in contracts
        ),
        underlying,
        carry,
        as_of,
        100_000,
    )
    quotes = SyntheticSurfaceGenerator.quotes(
        contracts,
        template,
        300_000,
        10_000,
    )
    bundle = replace(template, quotes=quotes)
    engine = OptionsAnalyticsEngine(
        OptionsAnalyticsConfig(
            ModelId(_identifier(6)),
            ModelVersion(_identifier(7)),
            surface=VolatilitySurfaceConfig(
                minimum_points=4,
                minimum_expirations=2,
                price_tolerance_currency_nanos=1_000_000,
            ),
        )
    )
    if engine.evaluate(bundle).decision is not OptionsDecision.PUBLISH:
        msg = "options benchmark fixture must publish"
        raise RuntimeError(msg)
    return engine, bundle


def _percentile(ordered: list[int], percentile: int) -> int:
    return ordered[((len(ordered) - 1) * percentile) // 100]


def _measure(
    name: str,
    iterations: int,
    operation: Callable[[], object],
) -> TimingSummary:
    for _ in range(WARMUP_ITERATIONS):
        operation()
    durations = []
    started = time.perf_counter_ns()
    for _ in range(iterations):
        sample_started = time.perf_counter_ns()
        operation()
        durations.append(time.perf_counter_ns() - sample_started)
    elapsed = max(1, time.perf_counter_ns() - started)
    ordered = sorted(durations)
    return TimingSummary(
        name,
        iterations,
        _percentile(ordered, 50),
        _percentile(ordered, 95),
        _percentile(ordered, 99),
        (iterations * 1_000_000_000) // elapsed,
    )


def run(iterations: int) -> dict[str, object]:
    """Measure full-chain evaluation and detailed-result hashing."""
    if not 0 < iterations <= MAX_ITERATIONS:
        msg = f"iterations must be within [1, {MAX_ITERATIONS}]"
        raise ValueError(msg)
    engine, bundle = _fixture()
    result = engine.evaluate(bundle)
    summaries = (
        _measure(
            "options_full_chain_evaluate",
            iterations,
            lambda: engine.evaluate(bundle),
        ),
        _measure(
            "options_result_replay_hash",
            iterations,
            lambda: result.sha256,
        ),
    )
    return {
        "benchmark_scope": "infrastructure_only",
        "dealer_inventory_observed": False,
        "economic_value_claim": False,
        "seed": SEED,
        "surface_contract_count": len(bundle.contracts),
        "summaries": [asdict(summary) for summary in summaries],
    }


def main() -> int:
    """Write stable-shape JSON; host timings are diagnostic only."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--iterations", type=int, default=100)
    parser.add_argument("--output", required=True, type=Path)
    arguments = parser.parse_args()
    result = run(arguments.iterations)
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":  # pragma: no cover - benchmark command entry point.
    raise SystemExit(main())
