"""Deterministic smoke benchmark for the off-path macro release specialist."""

from __future__ import annotations

import argparse
import json
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Final

from aegis_mx_intelligence import (
    Annualization,
    ConfigurationVersion,
    CrossAssetKind,
    CrossAssetObservation,
    CrossAssetUnit,
    EvidenceExcerpt,
    ExpectedReleaseField,
    FeatureSnapshotId,
    GlobalEventId,
    HistoricalMacroSurprise,
    Identifier128,
    InstrumentId,
    MacroActualRelease,
    MacroCalendarEvent,
    MacroConsensusEstimate,
    MacroConsensusSnapshot,
    MacroeconomicReleaseSpecialist,
    MacroEventType,
    MacroEvidenceReference,
    MacroInputBundle,
    MacroMarketSnapshot,
    MacroMetricIdentity,
    MacroMetricRole,
    MacroMetricUnit,
    MacroSourceEvidence,
    MacroSourceKind,
    MacroSourceQuality,
    MacroSpecialistConfig,
    MacroValueObservation,
    ModelId,
    ModelVersion,
    ReleaseCompletenessState,
    SeasonalAdjustment,
    SessionId,
    sha256_bytes,
)

if TYPE_CHECKING:
    from collections.abc import Callable

SEED: Final = 20_260_828
WARMUP_ITERATIONS: Final = 8
MAX_ITERATIONS: Final = 100_000
SCHEDULED: Final = 1_800_000_000_000_000_000
TARGET: Final = InstrumentId(Identifier128(300, 301))


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


def _reference(source_id: str) -> tuple[MacroEvidenceReference, ...]:
    return (MacroEvidenceReference(source_id, 1),)


def _source(
    source_id: str,
    kind: MacroSourceKind,
    publication_ns: int,
    receipt_ns: int,
    quality: MacroSourceQuality = MacroSourceQuality.SYNTHETIC_REPLAY,
) -> MacroSourceEvidence:
    text = f"{source_id} fictional macro benchmark evidence"
    return MacroSourceEvidence(
        source_id=source_id,
        source_global_event_id=GlobalEventId(_identifier(100 + int(kind))),
        kind=kind,
        quality=quality,
        authenticated=True,
        provider_id="benchmark-mock",
        document_id=f"{source_id}-document",
        source_content_sha256=sha256_bytes(f"raw-{text}"),
        sanitized_content_sha256=sha256_bytes(text),
        publication_wall_clock_utc_ns=publication_ns,
        received_wall_clock_utc_ns=receipt_ns,
        excerpts=(EvidenceExcerpt(1, 0, len(text), text),),
    )


def _fixture() -> tuple[MacroeconomicReleaseSpecialist, MacroInputBundle]:
    identity = MacroMetricIdentity(
        MacroEventType.CPI,
        "cpi.all_items.mom",
        MacroMetricRole.HEADLINE,
        MacroMetricUnit.PERCENT_PPM,
        "2026-07",
        SeasonalAdjustment.SEASONALLY_ADJUSTED,
        Annualization.NOT_ANNUALIZED,
    )
    calendar_id = GlobalEventId(_identifier(1))
    expected = ExpectedReleaseField(
        identity=identity,
        required=True,
        positive_surprise_direction=-1,
        forecast_weight_ppm=1_000_000,
        evidence=_reference("calendar"),
    )
    calendar = MacroCalendarEvent(
        calendar_id,
        MacroEventType.CPI,
        "benchmark-calendar",
        SCHEDULED,
        (expected,),
    )
    consensus = MacroConsensusSnapshot(
        GlobalEventId(_identifier(3)),
        calendar_id,
        SCHEDULED - 5_000,
        (
            MacroConsensusEstimate(
                identity,
                30_000,
                1_000,
                28_000,
                32_000,
                20,
                SCHEDULED - 6_000,
                _reference("consensus"),
            ),
        ),
    )
    receipt = SCHEDULED + 100
    release = MacroActualRelease(
        1,
        ReleaseCompletenessState.COMPLETE,
        SCHEDULED,
        receipt,
        (
            MacroValueObservation(
                identity,
                32_000,
                "release",
                _reference("release"),
            ),
        ),
        (),
    )
    response_window = (receipt - 10, receipt + 10)
    response_specs = (
        (
            TARGET,
            CrossAssetKind.EQUITY_INDEX_FUTURE,
            CrossAssetUnit.RETURN_PPM,
            -20_000,
        ),
        (
            InstrumentId(_identifier(310)),
            CrossAssetKind.TREASURY_YIELD,
            CrossAssetUnit.YIELD_CHANGE_BASIS_POINTS,
            2,
        ),
        (
            InstrumentId(_identifier(320)),
            CrossAssetKind.FX_PROXY,
            CrossAssetUnit.RETURN_PPM,
            10_000,
        ),
        (
            InstrumentId(_identifier(330)),
            CrossAssetKind.VOLATILITY_INSTRUMENT,
            CrossAssetUnit.RETURN_PPM,
            30_000,
        ),
        (
            InstrumentId(_identifier(340)),
            CrossAssetKind.SECTOR_ETF,
            CrossAssetUnit.RETURN_PPM,
            -15_000,
        ),
    )
    market = MacroMarketSnapshot(
        FeatureSnapshotId(_identifier(5)),
        "market",
        55_000,
        receipt + 20,
        tuple(
            CrossAssetObservation(
                instrument,
                kind,
                unit,
                value,
                response_window[0],
                response_window[1],
            )
            for instrument, kind, unit, value in response_specs
        ),
    )
    history = tuple(
        HistoricalMacroSurprise(
            GlobalEventId(_identifier(20 + index)),
            MacroEventType.CPI,
            SCHEDULED - 20_000 - index,
            SCHEDULED - 10_000 - index,
            value,
            "history",
        )
        for index, value in enumerate((-2_000_000, -1_000_000, 0, 1_000_000, 1_500_000))
    )
    sources = (
        _source(
            "calendar",
            MacroSourceKind.OFFICIAL_CALENDAR,
            SCHEDULED - 10_000,
            SCHEDULED - 9_000,
        ),
        _source(
            "consensus",
            MacroSourceKind.CONSENSUS_SNAPSHOT,
            SCHEDULED - 8_000,
            SCHEDULED - 7_000,
        ),
        _source(
            "history",
            MacroSourceKind.HISTORICAL_SURPRISE,
            SCHEDULED - 30_000,
            SCHEDULED - 29_000,
        ),
        _source(
            "release",
            MacroSourceKind.OFFICIAL_RELEASE,
            SCHEDULED,
            receipt,
            MacroSourceQuality.OFFICIAL_PRIMARY,
        ),
        _source("market", MacroSourceKind.MARKET_FEATURES, receipt, receipt + 10),
    )
    bundle = MacroInputBundle(
        SessionId(_identifier(7)),
        TARGET,
        ConfigurationVersion(_identifier(9)),
        calendar,
        consensus,
        sources,
        history,
        receipt + 30,
        500_000,
        release,
        market,
    )
    specialist = MacroeconomicReleaseSpecialist(
        MacroSpecialistConfig(ModelId(_identifier(11)), ModelVersion(_identifier(13)))
    )
    return specialist, bundle


def _percentile(ordered: list[int], percentile: int) -> int:
    return ordered[((len(ordered) - 1) * percentile) // 100]


def _measure(
    name: str, iterations: int, operation: Callable[[], object]
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
    """Measure deterministic complete-release evaluation and result hashing."""
    if not 0 < iterations <= MAX_ITERATIONS:
        msg = f"iterations must be within [1, {MAX_ITERATIONS}]"
        raise ValueError(msg)
    specialist, bundle = _fixture()
    result = specialist.evaluate(bundle)
    summaries = (
        _measure(
            "macro_specialist_complete_release_evaluate",
            iterations,
            lambda: specialist.evaluate(bundle),
        ),
        _measure(
            "macro_result_replay_hash",
            iterations,
            lambda: result.sha256,
        ),
    )
    return {
        "benchmark_scope": "infrastructure_only",
        "economic_value_claim": False,
        "seed": SEED,
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
