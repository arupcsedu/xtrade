"""Deterministic smoke benchmark for the off-path earnings specialist."""

from __future__ import annotations

import argparse
import json
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Final

from aegis_mx_intelligence import (
    AccountingBasis,
    ConfigurationVersion,
    EarningsEvidenceReference,
    EarningsInputBundle,
    EarningsMarketFeatures,
    EarningsMetricIdentity,
    EarningsMetricKind,
    EarningsMetricUnit,
    EarningsSourceEvidence,
    EarningsSourceKind,
    EarningsSpecialist,
    EarningsSpecialistConfig,
    EstimateDistribution,
    EvidenceExcerpt,
    FeatureSnapshotId,
    GlobalEventId,
    Identifier128,
    InstrumentId,
    ModelId,
    ModelVersion,
    NormalizedEarningsMetric,
    SessionId,
    sha256_bytes,
)

if TYPE_CHECKING:
    from collections.abc import Callable

SEED: Final = 20_260_828
WARMUP_ITERATIONS: Final = 8
MAX_ITERATIONS: Final = 100_000
RELEASE_TIME: Final = 1_800_000_000_000_000_000


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


def _source(
    source_id: str, kind: EarningsSourceKind, publication_time_ns: int
) -> EarningsSourceEvidence:
    text = f"{source_id} fictional earnings evidence"
    return EarningsSourceEvidence(
        source_id=source_id,
        source_global_event_id=GlobalEventId(
            _identifier(100 if source_id == "release" else 110)
        ),
        kind=kind,
        provider_id="benchmark-mock",
        document_id=f"{source_id}-document",
        source_content_sha256=sha256_bytes(f"raw-{text}"),
        sanitized_content_sha256=sha256_bytes(text),
        publication_wall_clock_utc_ns=publication_time_ns,
        received_wall_clock_utc_ns=publication_time_ns + 1,
        excerpts=(EvidenceExcerpt(1, 0, len(text), text),),
    )


def _fixture() -> tuple[EarningsSpecialist, EarningsInputBundle]:
    identity = EarningsMetricIdentity(
        EarningsMetricKind.EPS,
        AccountingBasis.GAAP,
        EarningsMetricUnit.CURRENCY_NANOS_PER_SHARE,
        "FY2026-Q2",
    )
    evidence = (EarningsEvidenceReference("release", 1),)
    consensus_evidence = (EarningsEvidenceReference("consensus", 1),)
    bundle = EarningsInputBundle(
        earnings_event_id=GlobalEventId(_identifier(1)),
        session_id=SessionId(_identifier(3)),
        instrument_id=InstrumentId(_identifier(5)),
        configuration_version=ConfigurationVersion(_identifier(7)),
        revision=1,
        as_of_wall_clock_utc_ns=RELEASE_TIME + 1_000,
        production_process_monotonic_time_ns=500_000,
        sources=(
            _source(
                "release",
                EarningsSourceKind.OFFICIAL_EARNINGS_RELEASE,
                RELEASE_TIME,
            ),
            _source(
                "consensus",
                EarningsSourceKind.CONSENSUS_ESTIMATES,
                RELEASE_TIME - 100,
            ),
        ),
        actuals=(NormalizedEarningsMetric(identity, 2_500_000_000, evidence),),
        consensus=(
            EstimateDistribution(
                identity,
                2_300_000_000,
                100_000_000,
                2_100_000_000,
                2_450_000_000,
                12,
                RELEASE_TIME - 1,
                consensus_evidence,
            ),
        ),
        prior_reported_values=(),
        current_guidance=(),
        prior_guidance=(),
        one_time_adjustments=(),
        historical_reactions=(),
        market_features=EarningsMarketFeatures(
            feature_snapshot_id=FeatureSnapshotId(_identifier(9)),
            as_of_exchange_event_time_ns=RELEASE_TIME,
            available_wall_clock_utc_ns=RELEASE_TIME,
            return_since_release_ppm=0,
            realized_volatility_ppm=0,
            is_post_release=False,
        ),
    )
    specialist = EarningsSpecialist(
        EarningsSpecialistConfig(
            ModelId(_identifier(11)), ModelVersion(_identifier(13))
        )
    )
    return specialist, bundle


def _percentile(ordered: list[int], percentile: int) -> int:
    index = ((len(ordered) - 1) * percentile) // 100
    return ordered[index]


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
        name=name,
        iterations=iterations,
        p50_ns=_percentile(ordered, 50),
        p95_ns=_percentile(ordered, 95),
        p99_ns=_percentile(ordered, 99),
        throughput_per_second=(iterations * 1_000_000_000) // elapsed,
    )


def run(iterations: int) -> dict[str, object]:
    """Measure deterministic evaluation and canonical replay hashing."""
    if not 0 < iterations <= MAX_ITERATIONS:
        msg = f"iterations must be within [1, {MAX_ITERATIONS}]"
        raise ValueError(msg)
    specialist, bundle = _fixture()
    result = specialist.evaluate(bundle)
    summaries = (
        _measure(
            "earnings_specialist_evaluate",
            iterations,
            lambda: specialist.evaluate(bundle),
        ),
        _measure(
            "earnings_result_replay_hash",
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


if __name__ == "__main__":  # pragma: no cover - exercised as a benchmark command.
    raise SystemExit(main())
