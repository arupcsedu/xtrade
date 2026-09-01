"""Deterministic synthetic and replay tests for related market specialists."""

from __future__ import annotations

import hashlib
import json
from dataclasses import replace
from pathlib import Path
from typing import Any, cast

import pytest
from aegis_mx_intelligence import market_specialists as specialist_module
from aegis_mx_intelligence import market_specialists_types as types_module
from aegis_mx_intelligence.contracts import (
    ConfigurationVersion,
    FeatureSnapshotId,
    GlobalEventId,
    Identifier128,
    InstrumentId,
    ModelId,
    ModelVersion,
    SessionId,
    VenueId,
)
from aegis_mx_intelligence.market_specialists import (
    AuctionSpecialist,
    AuctionSpecialistConfig,
    HiddenLiquiditySpecialist,
    HiddenLiquiditySpecialistConfig,
    IndexRebalanceSpecialist,
    IndexRebalanceSpecialistConfig,
    MarketSpecialistEvaluationError,
)
from aegis_mx_intelligence.market_specialists_types import (
    AuctionAllocationAssumption,
    AuctionInputBundle,
    AuctionSnapshot,
    AuctionVolumeForecast,
    BookSide,
    DisplayedDepthObservation,
    EtfRebalanceContext,
    HiddenLiquidityAssumption,
    HiddenLiquidityInputBundle,
    HistoricalAuctionBehavior,
    HistoricalRebalanceAuction,
    IndexRebalanceAnnouncement,
    LiquidityExecution,
    PartialFillSequence,
    PassiveFlowAssumption,
    PriceImpactObservation,
    ProvenanceKind,
    ProvenanceRecord,
    RebalanceAuctionState,
    RebalanceEstimate,
    RebalanceInputBundle,
    ReplenishmentObservation,
    SignedIntRange,
    SpecialistContext,
    SpecialistDecision,
    SpecialistReasonCode,
    SymbolPolicySnapshot,
    VenueBehaviorPrior,
)

FIXTURES = Path(__file__).parent / "fixtures" / "market_specialists"
INSTRUMENT = InstrumentId(Identifier128(1, 1))
VENUE = VenueId(Identifier128(2, 2))
CONFIGURATION = ConfigurationVersion(Identifier128(3, 3))


def _load(name: str) -> dict[str, Any]:
    return cast(
        "dict[str, Any]",
        json.loads((FIXTURES / name).read_text(encoding="utf-8")),
    )


def _identifier(seed: int) -> Identifier128:
    return Identifier128(seed, seed + 1)


def _provenance(
    source_id: str,
    kind: ProvenanceKind,
    received_ns: int,
    *,
    ordinal: int,
    quality_ppm: int = 950_000,
    authenticated: bool = True,
    exchange_time_ns: int | None = None,
) -> ProvenanceRecord:
    content = hashlib.sha256(f"synthetic:{source_id}:{ordinal}".encode()).digest()
    return ProvenanceRecord(
        source_id=source_id,
        source_event_id=GlobalEventId(_identifier(100 + ordinal)),
        kind=kind,
        content_sha256=content,
        received_wall_clock_utc_ns=received_ns,
        data_quality_score_ppm=quality_ppm,
        authenticated=authenticated,
        event_wall_clock_utc_ns=(
            None if exchange_time_ns is not None else received_ns - 1
        ),
        exchange_event_time_ns=exchange_time_ns,
    )


def _context(
    as_of: int,
    provenance: tuple[ProvenanceRecord, ...],
    *,
    event_ordinal: int,
    quality_ppm: int = 950_000,
    disabled: bool = False,
) -> SpecialistContext:
    policy = SymbolPolicySnapshot(
        GlobalEventId(_identifier(20 + event_ordinal)),
        CONFIGURATION,
        as_of - 1_000,
        (INSTRUMENT,) if disabled else (),
        "synthetic-disable" if disabled else "enabled",
    )
    return SpecialistContext(
        GlobalEventId(_identifier(30 + event_ordinal)),
        SessionId(_identifier(4)),
        INSTRUMENT,
        FeatureSnapshotId(_identifier(5 + event_ordinal)),
        CONFIGURATION,
        as_of,
        as_of - 1_000,
        1_000_000 + event_ordinal,
        quality_ppm,
        provenance,
        policy,
    )


def _rebalance_bundle(
    *,
    disabled: bool = False,
    quality_ppm: int = 950_000,
    authenticated: bool = True,
    history_count: int = 3,
    assumption: PassiveFlowAssumption = (
        PassiveFlowAssumption.UNKNOWN_BOUNDED_TRACKING
    ),
) -> tuple[RebalanceInputBundle, IndexRebalanceSpecialistConfig]:
    fixture = _load("closing_auction_rebalance_v1.json")
    values = fixture["rebalance"]
    as_of = int(fixture["as_of_wall_clock_utc_ns"])
    provenance = (
        _provenance(
            "announcement",
            ProvenanceKind.PROVIDER_ANNOUNCEMENT,
            as_of - 20_000,
            ordinal=1,
            quality_ppm=quality_ppm,
            authenticated=authenticated,
        ),
        _provenance(
            "etf",
            ProvenanceKind.ETF_DATA,
            as_of - 10_000,
            ordinal=2,
            quality_ppm=quality_ppm,
        ),
        _provenance(
            "rebalance-history",
            ProvenanceKind.HISTORICAL_AUCTION,
            as_of - 30_000,
            ordinal=3,
            quality_ppm=quality_ppm,
        ),
    )
    context = _context(
        as_of,
        provenance,
        event_ordinal=1,
        quality_ppm=quality_ppm,
        disabled=disabled,
    )
    announcement = IndexRebalanceAnnouncement(
        "announcement",
        str(values["index_id"]),
        int(values["revision"]),
        as_of - 100_000,
        int(fixture["effective_auction_wall_clock_utc_ns"]),
        int(values["old_weight_ppm"]),
        int(values["new_weight_ppm"]),
        int(values["float_adjusted_shares"]),
        int(values["estimated_tracking_assets_currency_nanos"]),
        int(values["tracking_assets_uncertainty_ppm"]),
        int(values["passive_participation_lower_ppm"]),
        int(values["passive_participation_upper_ppm"]),
    )
    etf = EtfRebalanceContext(
        "etf",
        int(values["reference_price_currency_nanos"]),
        int(values["etf_net_assets_currency_nanos"]),
        int(values["etf_net_flow_currency_nanos"]),
        int(values["average_daily_volume_shares"]),
        int(values["forecast_closing_auction_volume_shares"]),
    )
    history = tuple(
        HistoricalRebalanceAuction(
            "rebalance-history",
            as_of - 1_000_000 - index,
            as_of - 500_000 - index,
            int(item["passive_demand_shares"]),
            int(item["auction_volume_shares"]),
            int(item["realized_impact_ppm"]),
            bool(item["reversed"]),
        )
        for index, item in enumerate(values["historical"][:history_count])
    )
    bundle = RebalanceInputBundle(context, announcement, etf, history, assumption)
    config = IndexRebalanceSpecialistConfig(
        ModelId(_identifier(40)),
        ModelVersion(_identifier(41)),
    )
    return bundle, config


def _auction_bundle(
    rebalance: RebalanceEstimate | None,
    *,
    disabled: bool = False,
    quality_ppm: int = 950_000,
    stale: bool = False,
    history_count: int = 3,
    assumption: AuctionAllocationAssumption = (
        AuctionAllocationAssumption.QUEUE_PRIORITY_UNKNOWN
    ),
) -> tuple[AuctionInputBundle, AuctionSpecialistConfig]:
    fixture = _load("closing_auction_rebalance_v1.json")
    values = fixture["auction"]
    as_of = int(fixture["as_of_wall_clock_utc_ns"])
    snapshot_received = as_of - (3_000_000_000 if stale else 1_000)
    provenance = (
        _provenance(
            "auction",
            ProvenanceKind.AUCTION_FEED,
            snapshot_received,
            ordinal=4,
            quality_ppm=quality_ppm,
            exchange_time_ns=int(fixture["as_of_exchange_event_time_ns"]),
        ),
        _provenance(
            "auction-history",
            ProvenanceKind.HISTORICAL_AUCTION,
            as_of - 20_000,
            ordinal=5,
            quality_ppm=quality_ppm,
        ),
        _provenance(
            "volume",
            ProvenanceKind.VOLUME_FORECAST,
            as_of - 10_000,
            ordinal=6,
            quality_ppm=quality_ppm,
        ),
        _provenance(
            "rebalance-state",
            ProvenanceKind.REBALANCE_STATE,
            as_of - 5_000,
            ordinal=7,
            quality_ppm=quality_ppm,
        ),
    )
    context = _context(
        as_of,
        provenance,
        event_ordinal=2,
        quality_ppm=quality_ppm,
        disabled=disabled,
    )
    cutoff = int(values["cutoff_wall_clock_utc_ns"])
    snapshot = AuctionSnapshot(
        "auction",
        VENUE,
        int(values["signed_imbalance_quantity"]),
        int(values["paired_quantity"]),
        int(values["indicative_match_price_currency_nanos"]),
        int(values["reference_price_currency_nanos"]),
        cutoff,
        cutoff - as_of,
    )
    history = tuple(
        HistoricalAuctionBehavior(
            "auction-history",
            as_of - 100_000 - index,
            int(item["signed_imbalance_quantity"]),
            int(item["paired_quantity"]),
            int(item["auction_volume_quantity"]),
            int(item["clearing_return_ppm"]),
            int(item["fill_probability_ppm"]),
        )
        for index, item in enumerate(values["historical"][:history_count])
    )
    volume = AuctionVolumeForecast(
        "volume",
        int(values["forecast_quantity"]),
        int(values["forecast_confidence_ppm"]),
        as_of - 1,
    )
    state = RebalanceAuctionState(
        "rebalance-state",
        rebalance is not None,
        SignedIntRange(0, 0) if rebalance is None else rebalance.auction_demand_shares,
        0 if rebalance is None else rebalance.uncertainty_ppm,
    )
    bundle = AuctionInputBundle(
        context,
        snapshot,
        history,
        volume,
        state,
        assumption,
    )
    config = AuctionSpecialistConfig(
        ModelId(_identifier(42)),
        ModelVersion(_identifier(43)),
    )
    return bundle, config


def _hidden_bundle(
    *,
    side: BookSide = BookSide.BID,
    disabled: bool = False,
    quality_ppm: int = 950_000,
    stale: bool = False,
    sparse: bool = False,
) -> tuple[HiddenLiquidityInputBundle, HiddenLiquiditySpecialistConfig]:
    fixture = _load("hidden_liquidity_v1.json")
    as_of = int(fixture["as_of_wall_clock_utc_ns"])
    received = as_of - (2_000_000_000 if stale else 1_000)
    provenance = (
        _provenance(
            "book",
            ProvenanceKind.ORDER_BOOK,
            received,
            ordinal=8,
            quality_ppm=quality_ppm,
            exchange_time_ns=int(fixture["as_of_exchange_event_time_ns"]),
        ),
        _provenance(
            "executions",
            ProvenanceKind.EXECUTION_FEED,
            received,
            ordinal=9,
            quality_ppm=quality_ppm,
            exchange_time_ns=int(fixture["as_of_exchange_event_time_ns"]),
        ),
        _provenance(
            "venue",
            ProvenanceKind.VENUE_BEHAVIOR,
            as_of - 10_000,
            ordinal=10,
            quality_ppm=quality_ppm,
        ),
    )
    context = _context(
        as_of,
        provenance,
        event_ordinal=3,
        quality_ppm=quality_ppm,
        disabled=disabled,
    )
    price = int(fixture["candidate_price_ticks"])
    displayed = tuple(
        DisplayedDepthObservation(
            "book",
            price,
            side,
            int(quantity),
            int(fixture["as_of_exchange_event_time_ns"]) - index,
        )
        for index, quantity in enumerate(fixture["displayed_quantities"])
    )
    executions = tuple(
        LiquidityExecution(
            "executions",
            price,
            side,
            int(quantity),
            int(fixture["as_of_exchange_event_time_ns"]) - index,
        )
        for index, quantity in enumerate(
            fixture["execution_quantities"][:1]
            if sparse
            else fixture["execution_quantities"]
        )
    )
    replenishments = (
        ()
        if sparse
        else tuple(
            ReplenishmentObservation(
                "book",
                price,
                side,
                int(quantity),
                10_000 + index,
                int(fixture["as_of_exchange_event_time_ns"]) - index,
            )
            for index, quantity in enumerate(fixture["replenishments"])
        )
    )
    sequences = (
        ()
        if sparse
        else tuple(
            PartialFillSequence(
                "executions",
                price,
                side,
                int(item["initial_displayed_quantity"]),
                int(item["cumulative_executed_quantity"]),
                int(item["replenishment_count"]),
                int(item["duration_ns"]),
            )
            for item in fixture["partial_fill_sequences"]
        )
    )
    impacts = tuple(
        PriceImpactObservation(
            "executions",
            side,
            100,
            int(value),
            1_000_000_000,
            int(fixture["as_of_exchange_event_time_ns"]) - index,
        )
        for index, value in enumerate(fixture["price_impacts_ppm"])
    )
    venue_values = fixture["venue_behavior"]
    venue = VenueBehaviorPrior(
        "venue",
        VENUE,
        int(venue_values["baseline_replenishment_probability_ppm"]),
        int(venue_values["false_positive_probability_ppm"]),
        int(venue_values["typical_displayed_quantity"]),
        int(venue_values["maximum_latent_multiple_ppm"]),
    )
    bundle = HiddenLiquidityInputBundle(
        context,
        VENUE,
        price,
        side,
        displayed,
        executions,
        replenishments,
        sequences,
        impacts,
        venue,
        HiddenLiquidityAssumption.REPLENISHMENT_IS_INFERENTIAL,
    )
    config = HiddenLiquiditySpecialistConfig(
        ModelId(_identifier(44)),
        ModelVersion(_identifier(45)),
    )
    return bundle, config


def test_rebalance_closing_auction_and_hidden_liquidity_replay() -> None:
    rebalance_bundle, rebalance_config = _rebalance_bundle()
    rebalance_model = IndexRebalanceSpecialist(rebalance_config)
    rebalance = rebalance_model.evaluate(rebalance_bundle)
    assert rebalance.decision is SpecialistDecision.PUBLISH
    assert rebalance.estimate is not None
    assert rebalance.estimate.passive_flow_currency_nanos.lower > 0
    assert rebalance.estimate.auction_demand_shares.upper > 0
    assert rebalance.forecast_contract_bytes is not None
    assert (
        rebalance_model.verify_replay(rebalance_bundle, rebalance.sha256) == rebalance
    )

    auction_bundle, auction_config = _auction_bundle(rebalance.estimate)
    auction_model = AuctionSpecialist(auction_config)
    auction = auction_model.evaluate(auction_bundle)
    assert auction.decision is SpecialistDecision.PUBLISH
    assert auction.estimate is not None
    assert SpecialistReasonCode.REBALANCE_ACTIVE in auction.reason_codes
    assert auction.estimate.recommended_participation_cap_quantity > 0
    assert auction_model.verify_replay(auction_bundle, auction.sha256) == auction

    hidden_bundle, hidden_config = _hidden_bundle()
    hidden_model = HiddenLiquiditySpecialist(hidden_config)
    hidden = hidden_model.evaluate(hidden_bundle)
    assert hidden.decision is SpecialistDecision.PUBLISH
    assert hidden.estimate is not None
    assert hidden.estimate.iceberg_probability_ppm > 0
    assert hidden.estimate.latent_size_range.lower_quantity > 0
    assert hidden.estimate.assumption is (
        HiddenLiquidityAssumption.REPLENISHMENT_IS_INFERENTIAL
    )
    assert hidden_model.verify_replay(hidden_bundle, hidden.sha256) == hidden

    for model in (rebalance_model, auction_model, hidden_model):
        assert not hasattr(model, "submit_order")


def test_symbol_disablement_overrides_every_specialist() -> None:
    rebalance_bundle, rebalance_config = _rebalance_bundle(disabled=True)
    rebalance = IndexRebalanceSpecialist(rebalance_config).evaluate(rebalance_bundle)
    assert rebalance.decision is SpecialistDecision.ABSTAIN
    assert SpecialistReasonCode.SYMBOL_DISABLED in rebalance.reason_codes

    auction_bundle, auction_config = _auction_bundle(None, disabled=True)
    auction = AuctionSpecialist(auction_config).evaluate(auction_bundle)
    assert auction.decision is SpecialistDecision.ABSTAIN
    assert SpecialistReasonCode.SYMBOL_DISABLED in auction.reason_codes

    hidden_bundle, hidden_config = _hidden_bundle(disabled=True)
    hidden = HiddenLiquiditySpecialist(hidden_config).evaluate(hidden_bundle)
    assert hidden.decision is SpecialistDecision.ABSTAIN
    assert SpecialistReasonCode.SYMBOL_DISABLED in hidden.reason_codes


def test_data_quality_authentication_freshness_and_evidence_fail_closed() -> None:
    low_bundle, low_config = _rebalance_bundle(quality_ppm=100_000)
    low = IndexRebalanceSpecialist(low_config).evaluate(low_bundle)
    assert low.decision is SpecialistDecision.ABSTAIN
    assert SpecialistReasonCode.LOW_DATA_QUALITY in low.reason_codes

    unauth_bundle, unauth_config = _rebalance_bundle(authenticated=False)
    unauth = IndexRebalanceSpecialist(unauth_config).evaluate(unauth_bundle)
    assert SpecialistReasonCode.UNAUTHENTICATED_SOURCE in unauth.reason_codes

    short_bundle, short_config = _rebalance_bundle(history_count=2)
    short = IndexRebalanceSpecialist(short_config).evaluate(short_bundle)
    assert SpecialistReasonCode.INSUFFICIENT_HISTORY in short.reason_codes

    stale_auction_bundle, stale_auction_config = _auction_bundle(None, stale=True)
    stale_auction = AuctionSpecialist(stale_auction_config).evaluate(
        stale_auction_bundle
    )
    assert stale_auction.decision is SpecialistDecision.ABSTAIN
    assert SpecialistReasonCode.STALE_INPUT in stale_auction.reason_codes

    short_auction_bundle, short_auction_config = _auction_bundle(None, history_count=2)
    short_auction = AuctionSpecialist(short_auction_config).evaluate(
        short_auction_bundle
    )
    assert SpecialistReasonCode.INSUFFICIENT_HISTORY in short_auction.reason_codes

    sparse_bundle, sparse_config = _hidden_bundle(sparse=True)
    sparse = HiddenLiquiditySpecialist(sparse_config).evaluate(sparse_bundle)
    assert sparse.decision is SpecialistDecision.ABSTAIN
    assert SpecialistReasonCode.INSUFFICIENT_LIQUIDITY_EVIDENCE in sparse.reason_codes

    stale_hidden_bundle, stale_hidden_config = _hidden_bundle(stale=True)
    stale_hidden = HiddenLiquiditySpecialist(stale_hidden_config).evaluate(
        stale_hidden_bundle
    )
    assert SpecialistReasonCode.STALE_INPUT in stale_hidden.reason_codes


def test_explicit_assumptions_and_sell_side_distributions() -> None:
    rebalance_bundle, rebalance_config = _rebalance_bundle(
        assumption=PassiveFlowAssumption.FULL_WEIGHT_DELTA_AT_CLOSE
    )
    rebalance = IndexRebalanceSpecialist(rebalance_config).evaluate(rebalance_bundle)
    assert SpecialistReasonCode.ASSUMPTION_LIMITED not in rebalance.reason_codes

    auction_bundle, auction_config = _auction_bundle(
        None,
        assumption=AuctionAllocationAssumption.PRO_RATA_APPROXIMATION,
    )
    auction = AuctionSpecialist(auction_config).evaluate(auction_bundle)
    assert SpecialistReasonCode.ASSUMPTION_LIMITED not in auction.reason_codes

    ask_bundle, ask_config = _hidden_bundle(side=BookSide.ASK)
    ask = HiddenLiquiditySpecialist(ask_config).evaluate(ask_bundle)
    assert ask.decision is SpecialistDecision.PUBLISH
    assert ask.estimate is not None
    assert ask.estimate.support_resistance_persistence_ppm > 0


def test_replay_digest_mismatch_rejected() -> None:
    bundle, config = _rebalance_bundle()
    model = IndexRebalanceSpecialist(config)
    with pytest.raises(MarketSpecialistEvaluationError):
        model.verify_replay(bundle, b"short")
    with pytest.raises(MarketSpecialistEvaluationError):
        model.verify_replay(bundle, bytes(32))


def test_helper_config_hash_and_config_validation_paths() -> None:
    with pytest.raises(MarketSpecialistEvaluationError, match="positive denominator"):
        specialist_module._trunc_div(1, 0)
    with pytest.raises(MarketSpecialistEvaluationError, match="median"):
        specialist_module._median([])
    assert specialist_module._median([2, 4]) == 3
    with pytest.raises(MarketSpecialistEvaluationError, match="quantile"):
        specialist_module._quantile([], 50)
    with pytest.raises(MarketSpecialistEvaluationError, match="quantile"):
        specialist_module._quantile([1], 101)

    context = _context(100_000, (), event_ordinal=20)
    assert specialist_module._effective_quality(context) == 0
    assert SpecialistReasonCode.MISSING_PROVENANCE in specialist_module._common_reasons(
        context, 1
    )

    rebalance_config = _rebalance_bundle()[1]
    auction_config = _auction_bundle(None)[1]
    hidden_config = _hidden_bundle()[1]
    for config, model in (
        (rebalance_config, IndexRebalanceSpecialist(rebalance_config)),
        (auction_config, AuctionSpecialist(auction_config)),
        (hidden_config, HiddenLiquiditySpecialist(hidden_config)),
    ):
        assert len(config.sha256()) == 64
        assert model.configuration_sha256 == config.sha256()

    with pytest.raises(ValueError):
        replace(rebalance_config, forecast_ttl_ns=0)
    with pytest.raises(ValueError):
        replace(rebalance_config, forecast_horizon_ns=0)
    with pytest.raises(ValueError):
        replace(rebalance_config, forecast_ttl_ns=types_module.MAX_INT64 + 1)
    with pytest.raises(ValueError):
        replace(rebalance_config, minimum_data_quality_ppm=1_000_001)
    with pytest.raises(ValueError):
        replace(rebalance_config, minimum_history_count=0)
    with pytest.raises(ValueError):
        replace(rebalance_config, maximum_etf_input_age_ns=0)
    with pytest.raises(ValueError):
        replace(rebalance_config, timing_window_before_ns=0)

    with pytest.raises(ValueError):
        replace(auction_config, forecast_ttl_ns=0)
    with pytest.raises(ValueError):
        replace(auction_config, minimum_data_quality_ppm=-1)
    with pytest.raises(ValueError):
        replace(auction_config, minimum_history_count=0)
    with pytest.raises(ValueError):
        replace(auction_config, maximum_input_age_ns=0)
    with pytest.raises(ValueError):
        replace(auction_config, maximum_participation_ppm=0)
    with pytest.raises(ValueError):
        replace(auction_config, maximum_participation_ppm=1_000_001)

    with pytest.raises(ValueError):
        replace(hidden_config, forecast_horizon_ns=0)
    with pytest.raises(ValueError):
        replace(hidden_config, minimum_data_quality_ppm=-1)
    with pytest.raises(ValueError):
        replace(hidden_config, minimum_evidence_count=0)
    with pytest.raises(ValueError):
        replace(hidden_config, maximum_input_age_ns=0)


def test_zero_negative_and_no_impact_calculation_paths() -> None:
    rebalance_bundle, rebalance_config = _rebalance_bundle()
    zero_announcement = replace(
        rebalance_bundle.announcement,
        new_weight_ppm=rebalance_bundle.announcement.old_weight_ppm,
    )
    zero = IndexRebalanceSpecialist(rebalance_config).evaluate(
        replace(rebalance_bundle, announcement=zero_announcement)
    )
    assert zero.decision is SpecialistDecision.PUBLISH
    assert zero.estimate is not None
    assert zero.estimate.passive_flow_currency_nanos == SignedIntRange(0, 0)

    sell_announcement = replace(
        rebalance_bundle.announcement,
        old_weight_ppm=rebalance_bundle.announcement.new_weight_ppm,
        new_weight_ppm=rebalance_bundle.announcement.old_weight_ppm,
    )
    sell = IndexRebalanceSpecialist(rebalance_config).evaluate(
        replace(rebalance_bundle, announcement=sell_announcement)
    )
    assert sell.estimate is not None
    assert sell.estimate.passive_flow_currency_nanos.upper < 0

    auction_bundle, auction_config = _auction_bundle(None)
    sell_auction = AuctionSpecialist(auction_config).evaluate(
        replace(
            auction_bundle,
            snapshot=replace(
                auction_bundle.snapshot,
                signed_imbalance_quantity=(
                    -auction_bundle.snapshot.signed_imbalance_quantity
                ),
            ),
        )
    )
    assert sell_auction.decision is SpecialistDecision.PUBLISH

    hidden_bundle, hidden_config = _hidden_bundle()
    no_impact = HiddenLiquiditySpecialist(hidden_config).evaluate(
        replace(hidden_bundle, price_impacts=())
    )
    assert no_impact.decision is SpecialistDecision.PUBLISH
    assert no_impact.estimate is not None
    assert no_impact.estimate.support_resistance_persistence_ppm > 0


def test_stale_etf_state_abstains() -> None:
    bundle, config = _rebalance_bundle()
    etf_source = bundle.context.provenance[1]
    stale_receipt = (
        bundle.context.as_of_wall_clock_utc_ns - config.maximum_etf_input_age_ns - 1
    )
    stale_source = replace(
        etf_source,
        received_wall_clock_utc_ns=stale_receipt,
        event_wall_clock_utc_ns=stale_receipt - 1,
    )
    stale_context = replace(
        bundle.context,
        provenance=(
            bundle.context.provenance[0],
            stale_source,
            *bundle.context.provenance[2:],
        ),
    )
    result = IndexRebalanceSpecialist(config).evaluate(
        replace(bundle, context=stale_context)
    )
    assert result.decision is SpecialistDecision.ABSTAIN
    assert SpecialistReasonCode.STALE_INPUT in result.reason_codes


def test_low_probability_hidden_liquidity_clamps_to_zero() -> None:
    bundle, config = _hidden_bundle()
    weak = replace(
        bundle,
        displayed_depth=(),
        replenishments=(),
        partial_fill_sequences=(),
        executions=bundle.executions[:3],
        price_impacts=(),
        venue_behavior=replace(
            bundle.venue_behavior,
            baseline_replenishment_probability_ppm=1_000_000,
            false_positive_probability_ppm=1_000_000,
        ),
    )
    result = HiddenLiquiditySpecialist(config).evaluate(weak)
    assert result.decision is SpecialistDecision.PUBLISH
    assert result.estimate is not None
    assert result.estimate.iceberg_probability_ppm == 0


def test_primitive_provenance_policy_and_context_validation() -> None:
    with pytest.raises(ValueError, match="bounded ASCII"):
        types_module._bounded_text("", "value")
    with pytest.raises(ValueError, match="positive"):
        types_module._positive(0, "value")
    with pytest.raises(ValueError, match="nonnegative"):
        types_module._nonnegative(-1, "value")
    with pytest.raises(ValueError, match="signed 64-bit"):
        types_module._signed(1 << 63, "value")
    with pytest.raises(ValueError, match="PPM"):
        types_module._ppm(-1, "value")
    with pytest.raises(ValueError, match="impact"):
        types_module._impact(10_000_001, "value")

    bundle, _ = _rebalance_bundle()
    context = bundle.context
    source = context.provenance[0]
    with pytest.raises(ValueError, match="nonzero SHA-256"):
        replace(source, content_sha256=bytes(32))
    with pytest.raises(ValueError, match="requires an event"):
        replace(
            source,
            event_wall_clock_utc_ns=None,
            exchange_event_time_ns=None,
        )
    with pytest.raises(ValueError, match="follows receipt"):
        replace(
            source,
            event_wall_clock_utc_ns=source.received_wall_clock_utc_ns + 1,
        )
    with pytest.raises(ValueError, match="exchange_event_time"):
        replace(source, event_wall_clock_utc_ns=None, exchange_event_time_ns=0)

    with pytest.raises(ValueError, match="unique"):
        replace(context.symbol_policy, disabled_instrument_ids=(INSTRUMENT, INSTRUMENT))
    assert len(context.symbol_policy.sha256) == 32

    with pytest.raises(ValueError, match="collection"):
        replace(
            context,
            provenance=(source,) * (types_module.MAX_COLLECTION_SIZE + 1),
        )
    with pytest.raises(ValueError, match="identifiers"):
        replace(context, provenance=(source, source))
    with pytest.raises(ValueError, match="unavailable"):
        replace(
            context,
            provenance=(
                replace(
                    source,
                    received_wall_clock_utc_ns=context.as_of_wall_clock_utc_ns + 1,
                ),
            ),
        )
    with pytest.raises(ValueError, match="not effective"):
        replace(
            context,
            symbol_policy=replace(
                context.symbol_policy,
                effective_wall_clock_utc_ns=context.as_of_wall_clock_utc_ns + 1,
            ),
        )
    with pytest.raises(ValueError, match="not effective"):
        replace(
            context,
            symbol_policy=replace(
                context.symbol_policy,
                configuration_version=ConfigurationVersion(_identifier(999)),
            ),
        )
    with pytest.raises(ValueError, match="does not resolve"):
        context.require_sources("missing")
    assert len(context.sha256) == 32


def test_range_rebalance_input_and_estimate_validation() -> None:
    with pytest.raises(ValueError, match="lower exceeds"):
        SignedIntRange(2, 1)
    with pytest.raises(ValueError, match="lower exceeds"):
        types_module.QuantityRange(2, 1)
    with pytest.raises(ValueError, match="not ordered"):
        types_module.DistributionPpm(1, 0, 2)

    bundle, _ = _rebalance_bundle()
    announcement = bundle.announcement
    with pytest.raises(ValueError, match="revision"):
        replace(announcement, revision=0)
    with pytest.raises(ValueError, match="follow announcement"):
        replace(
            announcement,
            effective_auction_wall_clock_utc_ns=(
                announcement.announcement_wall_clock_utc_ns
            ),
        )
    with pytest.raises(ValueError, match="lower exceeds"):
        replace(
            announcement,
            passive_participation_lower_ppm=900_000,
            passive_participation_upper_ppm=800_000,
        )

    history = bundle.history[0]
    with pytest.raises(ValueError, match="availability precedes"):
        replace(
            history,
            available_wall_clock_utc_ns=history.event_wall_clock_utc_ns - 1,
        )
    with pytest.raises(ValueError, match="unsupported"):
        replace(bundle, schema_version="0.0.0")
    with pytest.raises(ValueError, match="history exceeds"):
        replace(
            bundle,
            history=(history,) * (types_module.MAX_COLLECTION_SIZE + 1),
        )
    with pytest.raises(ValueError, match="does not resolve"):
        replace(bundle, etf=replace(bundle.etf, source_id="missing"))
    with pytest.raises(ValueError, match="unavailable"):
        replace(
            bundle,
            announcement=replace(
                announcement,
                announcement_wall_clock_utc_ns=(
                    bundle.context.as_of_wall_clock_utc_ns + 1
                ),
            ),
        )
    with pytest.raises(ValueError, match="unavailable"):
        replace(
            bundle,
            history=(
                replace(
                    history,
                    available_wall_clock_utc_ns=(
                        bundle.context.as_of_wall_clock_utc_ns + 1
                    ),
                ),
                *bundle.history[1:],
            ),
        )
    assert len(bundle.sha256) == 32

    estimate = (
        IndexRebalanceSpecialist(_rebalance_bundle()[1]).evaluate(bundle).estimate
    )
    assert estimate is not None
    with pytest.raises(ValueError, match="not increasing"):
        replace(
            estimate,
            timing_end_wall_clock_utc_ns=estimate.timing_start_wall_clock_utc_ns,
        )


def test_auction_input_distribution_and_estimate_validation() -> None:
    bundle, _ = _auction_bundle(None)
    with pytest.raises(ValueError, match="inactive"):
        replace(bundle.rebalance_state, demand_shares=SignedIntRange(1, 2))
    with pytest.raises(ValueError, match="unsupported"):
        replace(bundle, schema_version="0.0.0")
    with pytest.raises(ValueError, match="history exceeds"):
        replace(
            bundle,
            history=(bundle.history[0],) * (types_module.MAX_COLLECTION_SIZE + 1),
        )
    with pytest.raises(ValueError, match="does not resolve"):
        replace(
            bundle,
            volume_forecast=replace(bundle.volume_forecast, source_id="missing"),
        )
    with pytest.raises(ValueError, match="time_to_cutoff"):
        replace(
            bundle,
            snapshot=replace(
                bundle.snapshot,
                time_to_cutoff_ns=bundle.snapshot.time_to_cutoff_ns + 1,
            ),
        )
    with pytest.raises(ValueError, match="unavailable"):
        replace(
            bundle,
            volume_forecast=replace(
                bundle.volume_forecast,
                available_wall_clock_utc_ns=(
                    bundle.context.as_of_wall_clock_utc_ns + 1
                ),
            ),
        )
    with pytest.raises(ValueError, match="unavailable"):
        replace(
            bundle,
            history=(
                replace(
                    bundle.history[0],
                    available_wall_clock_utc_ns=(
                        bundle.context.as_of_wall_clock_utc_ns + 1
                    ),
                ),
                *bundle.history[1:],
            ),
        )
    assert len(bundle.sha256) == 32

    with pytest.raises(ValueError, match="not ordered"):
        types_module.ClearingPriceDistribution(3, 2, 4)
    with pytest.raises(ValueError, match="PPM"):
        types_module.AuctionEstimate(
            AuctionAllocationAssumption.QUEUE_PRIORITY_UNKNOWN,
            types_module.ClearingPriceDistribution(1, 2, 3),
            1_000_001,
            0,
            0,
        )


def test_hidden_liquidity_input_and_estimate_validation() -> None:
    bundle, _ = _hidden_bundle()
    with pytest.raises(ValueError, match="at least one"):
        replace(bundle.venue_behavior, maximum_latent_multiple_ppm=999_999)
    with pytest.raises(ValueError, match="unsupported"):
        replace(bundle, schema_version="0.0.0")
    with pytest.raises(ValueError, match="candidate price"):
        replace(bundle, candidate_price_ticks=0)
    with pytest.raises(ValueError, match="collection exceeds"):
        replace(
            bundle,
            executions=(bundle.executions[0],) * (types_module.MAX_COLLECTION_SIZE + 1),
        )
    with pytest.raises(ValueError, match="does not resolve"):
        replace(
            bundle,
            venue_behavior=replace(bundle.venue_behavior, source_id="missing"),
        )
    with pytest.raises(ValueError, match="different venue"):
        replace(
            bundle,
            venue_behavior=replace(
                bundle.venue_behavior,
                venue_id=VenueId(_identifier(999)),
            ),
        )

    wrong_side = BookSide.ASK
    mismatch_changes = (
        {
            "displayed_depth": (
                replace(bundle.displayed_depth[0], side=wrong_side),
                *bundle.displayed_depth[1:],
            )
        },
        {
            "executions": (
                replace(bundle.executions[0], resting_side=wrong_side),
                *bundle.executions[1:],
            )
        },
        {
            "replenishments": (
                replace(bundle.replenishments[0], side=wrong_side),
                *bundle.replenishments[1:],
            )
        },
        {
            "partial_fill_sequences": (
                replace(bundle.partial_fill_sequences[0], side=wrong_side),
                *bundle.partial_fill_sequences[1:],
            )
        },
        {
            "price_impacts": (
                replace(bundle.price_impacts[0], resting_side=wrong_side),
                *bundle.price_impacts[1:],
            )
        },
    )
    for changes in mismatch_changes:
        with pytest.raises(ValueError, match="candidate price and side"):
            replace(bundle, **changes)
    assert len(bundle.sha256) == 32

    with pytest.raises(ValueError, match="PPM"):
        types_module.HiddenLiquidityEstimate(
            HiddenLiquidityAssumption.REPLENISHMENT_IS_INFERENTIAL,
            1_000_001,
            types_module.QuantityRange(0, 1),
            0,
            0,
        )


def test_specialist_result_contract_validation_and_canonical_rejection() -> None:
    bundle, config = _rebalance_bundle()
    published = IndexRebalanceSpecialist(config).evaluate(bundle)
    disabled_bundle, disabled_config = _rebalance_bundle(disabled=True)
    abstained = IndexRebalanceSpecialist(disabled_config).evaluate(disabled_bundle)

    for changes in (
        {"input_sha256": bytes(32)},
        {"symbol_policy_sha256": bytes(32)},
        {"schema_version": "0.0.0"},
        {
            "reason_codes": (
                SpecialistReasonCode.ASSUMPTION_LIMITED,
                SpecialistReasonCode.ASSUMPTION_LIMITED,
            )
        },
        {"estimate": None},
        {
            "valid_until_process_monotonic_time_ns": (
                published.production_process_monotonic_time_ns
            )
        },
        {"forecast_contract_bytes": b"0000FAIL"},
    ):
        with pytest.raises(ValueError):
            replace(published, **changes)
    with pytest.raises(ValueError, match="abstention"):
        replace(abstained, estimate=published.estimate)
    assert len(published.canonical_bytes()) > 0
    assert len(published.sha256) == 32
    with pytest.raises(TypeError, match="unsupported canonical"):
        types_module._canonical_value(1.5)
