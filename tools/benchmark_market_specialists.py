"""Deterministic smoke benchmark for three advisory market specialists."""

from __future__ import annotations

import argparse
import hashlib
import json
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Final

from aegis_mx_intelligence import (
    AuctionAllocationAssumption,
    AuctionInputBundle,
    AuctionSnapshot,
    AuctionSpecialist,
    AuctionSpecialistConfig,
    AuctionVolumeForecast,
    BookSide,
    ConfigurationVersion,
    DisplayedDepthObservation,
    EtfRebalanceContext,
    FeatureSnapshotId,
    GlobalEventId,
    HiddenLiquidityAssumption,
    HiddenLiquidityInputBundle,
    HiddenLiquiditySpecialist,
    HiddenLiquiditySpecialistConfig,
    HistoricalAuctionBehavior,
    HistoricalRebalanceAuction,
    Identifier128,
    IndexRebalanceAnnouncement,
    IndexRebalanceSpecialist,
    IndexRebalanceSpecialistConfig,
    InstrumentId,
    LiquidityExecution,
    ModelId,
    ModelVersion,
    PartialFillSequence,
    PassiveFlowAssumption,
    PriceImpactObservation,
    ProvenanceKind,
    ProvenanceRecord,
    RebalanceAuctionState,
    RebalanceInputBundle,
    ReplenishmentObservation,
    SessionId,
    SpecialistContext,
    SpecialistDecision,
    SymbolPolicySnapshot,
    VenueBehaviorPrior,
    VenueId,
)

if TYPE_CHECKING:
    from collections.abc import Callable

SEED: Final = 20_260_828
WARMUP_ITERATIONS: Final = 8
MAX_ITERATIONS: Final = 100_000
AS_OF_NS: Final = 2_000_000_000_000_000_000
EXCHANGE_NS: Final = 1_000_000_000
INSTRUMENT: Final = InstrumentId(Identifier128(1, 2))
VENUE: Final = VenueId(Identifier128(3, 4))
CONFIGURATION: Final = ConfigurationVersion(Identifier128(5, 6))


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
    source_id: str,
    kind: ProvenanceKind,
    ordinal: int,
) -> ProvenanceRecord:
    return ProvenanceRecord(
        source_id,
        GlobalEventId(_identifier(100 + ordinal)),
        kind,
        hashlib.sha256(f"benchmark:{source_id}:{SEED}".encode()).digest(),
        AS_OF_NS - ordinal,
        950_000,
        authenticated=True,
        event_wall_clock_utc_ns=AS_OF_NS - ordinal - 1,
    )


def _context(
    provenance: tuple[ProvenanceRecord, ...], ordinal: int
) -> SpecialistContext:
    policy = SymbolPolicySnapshot(
        GlobalEventId(_identifier(200 + ordinal)),
        CONFIGURATION,
        AS_OF_NS - 1_000,
        (),
        "benchmark-enabled",
    )
    return SpecialistContext(
        GlobalEventId(_identifier(300 + ordinal)),
        SessionId(_identifier(7)),
        INSTRUMENT,
        FeatureSnapshotId(_identifier(400 + ordinal)),
        CONFIGURATION,
        AS_OF_NS,
        EXCHANGE_NS,
        1_000_000 + ordinal,
        950_000,
        provenance,
        policy,
    )


def _fixtures() -> tuple[
    tuple[IndexRebalanceSpecialist, RebalanceInputBundle],
    tuple[AuctionSpecialist, AuctionInputBundle],
    tuple[HiddenLiquiditySpecialist, HiddenLiquidityInputBundle],
]:
    rebalance_context = _context(
        (
            _source("announcement", ProvenanceKind.PROVIDER_ANNOUNCEMENT, 1),
            _source("etf", ProvenanceKind.ETF_DATA, 2),
            _source("rebalance-history", ProvenanceKind.HISTORICAL_AUCTION, 3),
        ),
        1,
    )
    history = tuple(
        HistoricalRebalanceAuction(
            "rebalance-history",
            AS_OF_NS - 1_000_000 - ordinal,
            AS_OF_NS - 500_000 - ordinal,
            100_000 + ordinal * 10_000,
            1_000_000,
            100 + ordinal * 25,
            bool(ordinal % 2),
        )
        for ordinal in range(3)
    )
    rebalance_bundle = RebalanceInputBundle(
        rebalance_context,
        IndexRebalanceAnnouncement(
            "announcement",
            "SYNTHETIC-INDEX",
            1,
            AS_OF_NS - 100_000,
            AS_OF_NS + 60_000_000_000,
            10_000,
            12_000,
            10_000_000,
            5_000_000_000_000_000,
            100_000,
            700_000,
            900_000,
        ),
        EtfRebalanceContext(
            "etf",
            100_000_000_000,
            1_000_000_000_000_000,
            10_000_000_000,
            5_000_000,
            1_000_000,
        ),
        history,
        PassiveFlowAssumption.UNKNOWN_BOUNDED_TRACKING,
    )
    rebalance_model = IndexRebalanceSpecialist(
        IndexRebalanceSpecialistConfig(
            ModelId(_identifier(10)),
            ModelVersion(_identifier(11)),
        )
    )
    rebalance_result = rebalance_model.evaluate(rebalance_bundle)
    if (
        rebalance_result.decision is not SpecialistDecision.PUBLISH
        or rebalance_result.estimate is None
    ):
        msg = "rebalance benchmark fixture must publish"
        raise RuntimeError(msg)

    auction_context = _context(
        (
            _source("auction", ProvenanceKind.AUCTION_FEED, 4),
            _source("auction-history", ProvenanceKind.HISTORICAL_AUCTION, 5),
            _source("volume", ProvenanceKind.VOLUME_FORECAST, 6),
            _source("rebalance-state", ProvenanceKind.REBALANCE_STATE, 7),
        ),
        2,
    )
    auction_history = tuple(
        HistoricalAuctionBehavior(
            "auction-history",
            AS_OF_NS - ordinal - 10,
            100_000 + ordinal * 10_000,
            800_000,
            1_000_000,
            100 + ordinal * 10,
            700_000,
        )
        for ordinal in range(3)
    )
    auction_bundle = AuctionInputBundle(
        auction_context,
        AuctionSnapshot(
            "auction",
            VENUE,
            150_000,
            900_000,
            100_020_000_000,
            100_000_000_000,
            AS_OF_NS + 60_000_000_000,
            60_000_000_000,
        ),
        auction_history,
        AuctionVolumeForecast("volume", 1_200_000, 900_000, AS_OF_NS - 1),
        RebalanceAuctionState(
            "rebalance-state",
            active=True,
            demand_shares=rebalance_result.estimate.auction_demand_shares,
            uncertainty_ppm=rebalance_result.estimate.uncertainty_ppm,
        ),
        AuctionAllocationAssumption.QUEUE_PRIORITY_UNKNOWN,
    )
    auction_model = AuctionSpecialist(
        AuctionSpecialistConfig(
            ModelId(_identifier(12)),
            ModelVersion(_identifier(13)),
        )
    )

    hidden_context = _context(
        (
            _source("book", ProvenanceKind.ORDER_BOOK, 8),
            _source("executions", ProvenanceKind.EXECUTION_FEED, 9),
            _source("venue", ProvenanceKind.VENUE_BEHAVIOR, 10),
        ),
        3,
    )
    price_ticks = 10_000
    hidden_bundle = HiddenLiquidityInputBundle(
        hidden_context,
        VENUE,
        price_ticks,
        BookSide.BID,
        (
            DisplayedDepthObservation("book", price_ticks, BookSide.BID, 100, 99),
            DisplayedDepthObservation("book", price_ticks, BookSide.BID, 100, 100),
        ),
        tuple(
            LiquidityExecution(
                "executions", price_ticks, BookSide.BID, quantity, 101 + ordinal
            )
            for ordinal, quantity in enumerate((80, 90, 100))
        ),
        (
            ReplenishmentObservation("book", price_ticks, BookSide.BID, 100, 10, 102),
            ReplenishmentObservation("book", price_ticks, BookSide.BID, 100, 10, 103),
        ),
        (
            PartialFillSequence(
                "executions", price_ticks, BookSide.BID, 100, 270, 2, 1_000
            ),
        ),
        (
            PriceImpactObservation(
                "executions", BookSide.BID, 270, 100, 1_000_000_000, 104
            ),
        ),
        VenueBehaviorPrior("venue", VENUE, 100_000, 50_000, 100, 10_000_000),
        HiddenLiquidityAssumption.REPLENISHMENT_IS_INFERENTIAL,
    )
    hidden_model = HiddenLiquiditySpecialist(
        HiddenLiquiditySpecialistConfig(
            ModelId(_identifier(14)),
            ModelVersion(_identifier(15)),
        )
    )

    if (
        auction_model.evaluate(auction_bundle).decision
        is not SpecialistDecision.PUBLISH
    ):
        msg = "auction benchmark fixture must publish"
        raise RuntimeError(msg)
    if hidden_model.evaluate(hidden_bundle).decision is not SpecialistDecision.PUBLISH:
        msg = "hidden-liquidity benchmark fixture must publish"
        raise RuntimeError(msg)
    return (
        (rebalance_model, rebalance_bundle),
        (auction_model, auction_bundle),
        (hidden_model, hidden_bundle),
    )


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
    """Measure each deterministic pure specialist on synthetic state."""
    if not 0 < iterations <= MAX_ITERATIONS:
        msg = f"iterations must be within [1, {MAX_ITERATIONS}]"
        raise ValueError(msg)
    rebalance, auction, hidden = _fixtures()
    summaries = (
        _measure(
            "index_rebalance_evaluate",
            iterations,
            lambda: rebalance[0].evaluate(rebalance[1]),
        ),
        _measure(
            "auction_evaluate",
            iterations,
            lambda: auction[0].evaluate(auction[1]),
        ),
        _measure(
            "hidden_liquidity_evaluate",
            iterations,
            lambda: hidden[0].evaluate(hidden[1]),
        ),
    )
    return {
        "benchmark_scope": "infrastructure_only",
        "economic_value_claim": False,
        "hidden_liquidity_observed": False,
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
