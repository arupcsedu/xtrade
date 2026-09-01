from __future__ import annotations

import math
from dataclasses import replace
from datetime import UTC, datetime
from typing import TYPE_CHECKING

import pytest
from aegis_mx_intelligence import options_analytics as options_analytics_module
from aegis_mx_intelligence import options_pricing as options_pricing_module
from aegis_mx_intelligence import options_types as options_types_module
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
from aegis_mx_intelligence.options_analytics import (
    ConfidenceEstimator,
    DealerPressureEstimator,
    OptionImpliedMoveEstimator,
    OptionsAnalyticsConfig,
    OptionsAnalyticsEngine,
    OptionsEvaluationError,
    OptionsSignalService,
    SkewAndTermStructureFeatures,
    SyntheticSurfaceGenerator,
    UnusualVolumeDetector,
    ZeroDteConcentrationEstimator,
)
from aegis_mx_intelligence.options_pricing import (
    ContractMaster,
    GreeksEngine,
    ImpliedVolatilityConfig,
    ImpliedVolatilitySolver,
    OptionsNumericalError,
    OptionSymbolParser,
    QuoteValidationConfig,
    QuoteValidator,
    VolatilitySurfaceBuilder,
    VolatilitySurfaceConfig,
    arbitrage_bounds_currency_nanos,
    black_scholes_price_currency_nanos,
)
from aegis_mx_intelligence.options_types import (
    NANOS_PER_DAY,
    PPM,
    CarryObservation,
    DealerPressureEstimate,
    DealerSideAssumption,
    ExerciseStyle,
    GreeksSnapshot,
    HedgingPressureDistribution,
    OpenInterestObservation,
    OptionContract,
    OptionQuote,
    OptionRight,
    OptionsAnalyticsResult,
    OptionsDecision,
    OptionsInputBundle,
    OptionsReasonCode,
    OptionsRegime,
    OptionsServiceState,
    OptionTrade,
    ParsedOptionSymbol,
    QuoteValidity,
    SettlementStyle,
    SurfacePoint,
    SurfaceQuality,
    UnderlyingObservation,
    VolatilitySurface,
)

if TYPE_CHECKING:
    from collections.abc import Callable


def _id(high: int, low: int) -> Identifier128:
    return Identifier128(high, low)


def _ns(year: int, month: int, day: int, hour: int = 0) -> int:
    return int(datetime(year, month, day, hour, tzinfo=UTC).timestamp() * 1e9)


AS_OF = _ns(2026, 9, 1, 14)
SPOT = 100_000_000_000
UNDERLYING_ID = InstrumentId(_id(2, 1))
VENUE_ID = VenueId(_id(3, 1))


def _contract(
    ordinal: int,
    strike_units: int,
    right: OptionRight,
    *,
    expiry_month: int = 9,
    expiry_day: int = 1,
    style: ExerciseStyle = ExerciseStyle.EUROPEAN,
    revision: int = 1,
    corporate_action_revision: int = 0,
) -> OptionContract:
    strike_milli = strike_units * 1_000
    symbol = (
        f"TEST  26{expiry_month:02d}{expiry_day:02d}"
        f"{'C' if right is OptionRight.CALL else 'P'}{strike_milli:08d}"
    )
    return OptionContract(
        contract_id=GlobalEventId(_id(10, ordinal)),
        option_instrument_id=InstrumentId(_id(11, ordinal)),
        underlying_instrument_id=UNDERLYING_ID,
        osi_symbol=symbol,
        root="TEST",
        right=right,
        exercise_style=style,
        settlement_style=SettlementStyle.PHYSICAL,
        strike_currency_nanos=strike_units * 1_000_000_000,
        multiplier_units=100,
        expiration_wall_clock_utc_ns=_ns(2026, expiry_month, expiry_day, 21),
        reference_version=revision,
        corporate_action_revision=corporate_action_revision,
        deliverable_units=100,
        effective_wall_clock_utc_ns=_ns(2026, 8, 1),
    )


def _base_bundle(
    *,
    volatility_ppm: int = 300_000,
    assumption: DealerSideAssumption = DealerSideAssumption.UNKNOWN_SYMMETRIC,
) -> tuple[OptionsInputBundle, OptionsAnalyticsConfig]:
    contracts = tuple(
        _contract(
            ordinal,
            strike,
            right,
            expiry_month=month,
            expiry_day=day,
        )
        for ordinal, (month, day, strike, right) in enumerate(
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
    underlying = UnderlyingObservation(
        UNDERLYING_ID,
        SPOT,
        AS_OF - 1_000,
        AS_OF - 500,
        AS_OF,
    )
    carry = CarryObservation(20_000, 5_000, AS_OF - 1, AS_OF)
    placeholder = OptionQuote(
        contracts[0].contract_id,
        VENUE_ID,
        1,
        2,
        1,
        1,
        AS_OF - 1_000,
        AS_OF - 500,
        AS_OF,
    )
    template = OptionsInputBundle(
        session_id=SessionId(_id(4, 1)),
        underlying_instrument_id=UNDERLYING_ID,
        feature_snapshot_id=FeatureSnapshotId(_id(5, 1)),
        configuration_version=ConfigurationVersion(_id(6, 1)),
        contracts=contracts,
        quotes=(placeholder,),
        trades=tuple(
            OptionTrade(item.contract_id, 1_000_000_000, index + 1, AS_OF - 2, AS_OF)
            for index, item in enumerate(contracts)
        ),
        open_interest=tuple(
            OpenInterestObservation(item.contract_id, 100 + index, AS_OF - 1, AS_OF)
            for index, item in enumerate(contracts)
        ),
        underlying=underlying,
        carry=carry,
        as_of_wall_clock_utc_ns=AS_OF,
        production_process_monotonic_time_ns=10_000,
    )
    quotes = SyntheticSurfaceGenerator.quotes(
        contracts,
        template,
        volatility_ppm,
        10_000,
    )
    bundle = replace(template, quotes=quotes)
    config = OptionsAnalyticsConfig(
        model_id=ModelId(_id(7, 1)),
        model_version=ModelVersion(_id(8, 1)),
        dealer_side_assumption=assumption,
        surface=VolatilitySurfaceConfig(
            minimum_points=4,
            minimum_expirations=2,
            price_tolerance_currency_nanos=1_000_000,
        ),
    )
    return bundle, config


def test_symbol_parser_and_contract_master() -> None:
    parsed = OptionSymbolParser.parse("TEST  260901C00100000")
    assert parsed == ParsedOptionSymbol("TEST", 20260901, OptionRight.CALL, 100_000)

    for invalid in (
        "short",
        "TEST  260901X00100000",
        "TEST  26AA01C00100000",
        "TEST  260231C00100000",
        "TE ST 260901C00100000",
    ):
        with pytest.raises(ValueError):
            OptionSymbolParser.parse(invalid)

    master = ContractMaster()
    contract = _contract(1, 100, OptionRight.CALL)
    master.add(contract)
    master.add(contract)
    assert master.size == 1
    assert master.get(contract.contract_id) == contract
    with pytest.raises(KeyError):
        master.get(_id(99, 99))
    with pytest.raises(ValueError):
        master.add(replace(contract, root="FAIL"))
    with pytest.raises(ValueError):
        master.add(replace(contract, strike_currency_nanos=99_000_000_000))

    changed_same_id = replace(contract, multiplier_units=101)
    with pytest.raises(ValueError):
        master.add(changed_same_id)

    revised = replace(
        contract,
        contract_id=GlobalEventId(_id(10, 99)),
        reference_version=2,
        effective_wall_clock_utc_ns=_ns(2026, 8, 2),
        corporate_action_revision=1,
    )
    master.add(revised)
    assert master.size == 2
    with pytest.raises(ValueError):
        master.add(
            replace(
                revised,
                contract_id=GlobalEventId(_id(10, 100)),
                reference_version=1,
            )
        )


def test_bsm_price_bounds_iv_and_greeks_accuracy() -> None:
    as_of = _ns(2026, 1, 1)
    contract = replace(
        _contract(1, 100, OptionRight.CALL),
        osi_symbol="TEST  270101C00100000",
        expiration_wall_clock_utc_ns=_ns(2027, 1, 1),
        effective_wall_clock_utc_ns=as_of - 1,
    )
    carry = CarryObservation(0, 0, as_of - 1, as_of)
    price = black_scholes_price_currency_nanos(contract, SPOT, carry, as_of, 200_000)
    assert price / 1e9 == pytest.approx(7.965567, abs=2e-6)
    lower, upper = arbitrage_bounds_currency_nanos(contract, SPOT, carry, as_of)
    assert lower == 0
    assert upper == SPOT

    solver = ImpliedVolatilitySolver(
        ImpliedVolatilityConfig(price_tolerance_currency_nanos=100)
    )
    recovered = solver.solve(contract, price, SPOT, carry, as_of)
    assert recovered == pytest.approx(200_000, abs=2)
    greeks = GreeksEngine().calculate(contract, SPOT, carry, as_of, recovered)
    assert greeks.delta_ppm == pytest.approx(539_828, abs=10)
    assert greeks.gamma_per_currency_ppb == pytest.approx(19_848_000, rel=1e-3)
    assert greeks.vega_currency_nanos_per_vol_ppm == pytest.approx(39_695, rel=1e-3)
    assert math.isfinite(greeks.vanna_per_vol_ppb)
    assert math.isfinite(greeks.charm_per_year_ppb)

    put = replace(
        contract,
        contract_id=GlobalEventId(_id(10, 2)),
        option_instrument_id=InstrumentId(_id(11, 2)),
        osi_symbol="TEST  270101P00100000",
        right=OptionRight.PUT,
    )
    put_price = black_scholes_price_currency_nanos(put, SPOT, carry, as_of, 200_000)
    assert put_price == pytest.approx(price, abs=2)
    assert GreeksEngine().calculate(put, SPOT, carry, as_of, 200_000).delta_ppm < 0


def test_pricing_and_solver_fail_closed() -> None:
    contract = _contract(1, 100, OptionRight.CALL)
    carry = CarryObservation(0, 0, AS_OF - 1, AS_OF)
    american = replace(contract, exercise_style=ExerciseStyle.AMERICAN)
    with pytest.raises(OptionsNumericalError):
        black_scholes_price_currency_nanos(american, SPOT, carry, AS_OF, 200_000)
    with pytest.raises(OptionsNumericalError):
        black_scholes_price_currency_nanos(contract, SPOT, carry, AS_OF, 0)
    with pytest.raises(OptionsNumericalError):
        black_scholes_price_currency_nanos(contract, SPOT, carry, AS_OF + 10**16, 1)
    with pytest.raises(OptionsNumericalError):
        ImpliedVolatilitySolver(ImpliedVolatilityConfig()).solve(
            contract, SPOT * 2, SPOT, carry, AS_OF
        )
    tight_solver = ImpliedVolatilitySolver(
        ImpliedVolatilityConfig(maximum_volatility_ppm=2)
    )
    price = black_scholes_price_currency_nanos(contract, SPOT, carry, AS_OF, 200_000)
    with pytest.raises(OptionsNumericalError):
        tight_solver.solve(contract, price, SPOT, carry, AS_OF)
    lower, _ = arbitrage_bounds_currency_nanos(contract, SPOT, carry, AS_OF)
    assert (
        ImpliedVolatilitySolver(ImpliedVolatilityConfig()).solve(
            contract, lower, SPOT, carry, AS_OF
        )
        == 0
    )


def test_quote_validation_all_quality_states() -> None:
    contract = _contract(1, 100, OptionRight.CALL)
    carry = CarryObservation(0, 0, AS_OF - 1, AS_OF)
    midpoint = black_scholes_price_currency_nanos(contract, SPOT, carry, AS_OF, 300_000)
    quote = OptionQuote(
        contract.contract_id,
        VENUE_ID,
        midpoint - 10_000,
        midpoint + 10_000,
        1,
        1,
        AS_OF - 10,
        AS_OF - 5,
        AS_OF,
    )
    validator = QuoteValidator(QuoteValidationConfig(stale_after_ns=100))
    assert validator.assess(contract, quote, SPOT, carry, AS_OF).validity is (
        QuoteValidity.VALID
    )
    cases = (
        (
            replace(quote, contract_id=GlobalEventId(_id(99, 1))),
            contract,
            AS_OF,
            QuoteValidity.INVALID,
        ),
        (
            quote,
            replace(contract, exercise_style=ExerciseStyle.AMERICAN),
            AS_OF,
            QuoteValidity.UNSUPPORTED_STYLE,
        ),
        (quote, contract, contract.expiration_wall_clock_utc_ns, QuoteValidity.EXPIRED),
        (
            replace(quote, received_wall_clock_utc_ns=AS_OF - 101),
            contract,
            AS_OF,
            QuoteValidity.STALE,
        ),
        (
            replace(
                quote,
                bid_price_currency_nanos=midpoint + 1,
                ask_price_currency_nanos=midpoint,
            ),
            contract,
            AS_OF,
            QuoteValidity.CROSSED,
        ),
        (
            replace(quote, bid_price_currency_nanos=0, ask_price_currency_nanos=SPOT),
            contract,
            AS_OF,
            QuoteValidity.WIDE,
        ),
        (
            replace(
                quote,
                bid_price_currency_nanos=SPOT + 2_000,
                ask_price_currency_nanos=SPOT + 3_000,
            ),
            contract,
            AS_OF,
            QuoteValidity.ARBITRAGE_INCONSISTENT,
        ),
    )
    for candidate, candidate_contract, as_of, expected in cases:
        assert (
            validator.assess(candidate_contract, candidate, SPOT, carry, as_of).validity
            is expected
        )


def test_engine_publishes_features_replay_and_assumption_distributions() -> None:
    bundle, config = _base_bundle()
    engine = OptionsAnalyticsEngine(config)
    result = engine.evaluate(bundle)
    assert result.decision is OptionsDecision.PUBLISH
    assert result.regime is OptionsRegime.BALANCED
    assert result.expected_volatility_ppm == pytest.approx(300_000, abs=2)
    assert result.option_implied_move_ppm is not None
    assert result.zero_dte_concentration_ppm is not None
    assert result.unusual_volume_score_ppm is not None
    assert result.expected_pinning_pressure_ppm is not None
    assert result.dealer_pressure.assumption is DealerSideAssumption.UNKNOWN_SYMMETRIC
    assert result.dealer_pressure.signed_gamma_exposure_currency_nanos == 0
    assert OptionsReasonCode.UNKNOWN_DEALER_SIDE in result.reason_codes
    assert result.forecast_contract_bytes is not None
    assert result.sha256 == engine.verify_replay(bundle, result.sha256).sha256
    with pytest.raises(OptionsEvaluationError):
        engine.verify_replay(bundle, b"short")
    with pytest.raises(OptionsEvaluationError):
        engine.verify_replay(bundle, bytes(32))

    for assumption, sign in (
        (DealerSideAssumption.DEALER_SHORT_CUSTOMER_POSITION, -1),
        (DealerSideAssumption.DEALER_LONG_CUSTOMER_POSITION, 1),
    ):
        _, alternate_config = _base_bundle(assumption=assumption)
        alternate = OptionsAnalyticsEngine(alternate_config).evaluate(bundle)
        assert (
            math.copysign(
                1,
                alternate.dealer_pressure.signed_gamma_exposure_currency_nanos,
            )
            == sign
        )
        assert alternate.dealer_pressure.assumption_uncertainty_ppm == 350_000


def test_engine_recovery_abstention_ood_and_regimes() -> None:
    bundle, config = _base_bundle()
    crossed_quotes = tuple(
        replace(
            quote,
            bid_price_currency_nanos=quote.ask_price_currency_nanos + 1,
        )
        for quote in bundle.quotes
    )
    failed = OptionsAnalyticsEngine(config).evaluate(
        replace(bundle, quotes=crossed_quotes)
    )
    assert failed.decision is OptionsDecision.ABSTAIN
    assert OptionsReasonCode.CROSSED_QUOTES in failed.reason_codes
    assert failed.expected_volatility_ppm is None
    recovered = OptionsAnalyticsEngine(config).evaluate(bundle)
    assert recovered.decision is OptionsDecision.PUBLISH

    stale = replace(
        bundle,
        quotes=tuple(
            replace(quote, received_wall_clock_utc_ns=AS_OF - 3_000_000_000)
            for quote in bundle.quotes
        ),
    )
    stale_result = OptionsAnalyticsEngine(config).evaluate(stale)
    assert stale_result.decision is OptionsDecision.ABSTAIN
    assert stale_result.ood_score_ppm == PPM
    assert OptionsReasonCode.OOD_INPUT in stale_result.reason_codes

    no_market_depth = replace(bundle, trades=(), open_interest=())
    result = OptionsAnalyticsEngine(config).evaluate(no_market_depth)
    assert result.decision is OptionsDecision.PUBLISH
    assert result.unusual_volume_score_ppm is None
    assert result.zero_dte_concentration_ppm is None
    assert result.expected_pinning_pressure_ppm is None

    low_bundle, low_config = _base_bundle(volatility_ppm=100_000)
    assert OptionsAnalyticsEngine(low_config).evaluate(low_bundle).regime is (
        OptionsRegime.LOW_VOLATILITY
    )
    high_bundle, high_config = _base_bundle(volatility_ppm=600_000)
    assert OptionsAnalyticsEngine(high_config).evaluate(high_bundle).regime is (
        OptionsRegime.HIGH_VOLATILITY
    )


def _point(
    ordinal: int,
    expiry: int,
    strike: int,
    right: OptionRight,
    volatility: int,
    price: int,
) -> SurfacePoint:
    return SurfacePoint(
        GlobalEventId(_id(10, ordinal)),
        expiry,
        strike,
        right,
        volatility,
        0,
        price,
        GreeksSnapshot(500_000, 1_000, 1_000, 1_000, 1_000),
    )


def test_surface_arbitrage_features_and_confidence_edges() -> None:
    builder = VolatilitySurfaceBuilder(
        VolatilitySurfaceConfig(minimum_points=3, minimum_expirations=1)
    )
    expiry = AS_OF + 10 * NANOS_PER_DAY
    healthy = builder.build(
        (
            _point(1, expiry, 90, OptionRight.CALL, 300_000, 12_000_000_000),
            _point(2, expiry, 100, OptionRight.CALL, 300_000, 5_000_000_000),
            _point(3, expiry, 110, OptionRight.CALL, 300_000, 1_000_000_000),
        ),
        AS_OF,
    )
    assert healthy.quality is SurfaceQuality.HEALTHY
    convex_bad = builder.build(
        (
            _point(1, expiry, 90, OptionRight.CALL, 300_000, 12_000_000_000),
            _point(2, expiry, 100, OptionRight.CALL, 300_000, 11_000_000_000),
            _point(3, expiry, 110, OptionRight.CALL, 300_000, 2_000_000_000),
        ),
        AS_OF,
    )
    assert convex_bad.quality is SurfaceQuality.INVALID
    assert OptionsReasonCode.CONVEXITY_ARBITRAGE in convex_bad.reason_codes
    calendar_bad = builder.build(
        (
            _point(1, expiry, 100, OptionRight.PUT, 800_000, 3_000_000_000),
            _point(
                2, expiry + NANOS_PER_DAY, 100, OptionRight.PUT, 100_000, 3_100_000_000
            ),
            _point(3, expiry, 110, OptionRight.PUT, 300_000, 4_000_000_000),
        ),
        AS_OF,
    )
    assert OptionsReasonCode.CALENDAR_ARBITRAGE in calendar_bad.reason_codes

    empty = VolatilitySurface((), SurfaceQuality.DEGRADED, ())
    assert SkewAndTermStructureFeatures.calculate(empty, SPOT) == (None, None)
    assert OptionImpliedMoveEstimator.estimate(empty, SPOT) is None
    assert ConfidenceEstimator.estimate((), empty, 0, PPM) == (0, PPM)


def test_volume_zero_dte_and_pressure_helpers() -> None:
    bundle, config = _base_bundle()
    surface = OptionsAnalyticsEngine(config).evaluate(bundle).surface
    assert UnusualVolumeDetector.score(bundle) is not None
    assert ZeroDteConcentrationEstimator.score(bundle) == 0
    zero_dte_contracts = (
        replace(
            bundle.contracts[0],
            expiration_wall_clock_utc_ns=AS_OF + 1_000_000_000,
        ),
        *bundle.contracts[1:],
    )
    zero_dte_score = ZeroDteConcentrationEstimator.score(
        replace(bundle, contracts=zero_dte_contracts)
    )
    assert zero_dte_score is not None
    assert zero_dte_score > 0
    zero_oi = replace(
        bundle,
        open_interest=tuple(
            replace(item, open_interest_contracts=0) for item in bundle.open_interest
        ),
    )
    assert UnusualVolumeDetector.score(zero_oi) == PPM
    no_trades = replace(zero_oi, trades=())
    assert UnusualVolumeDetector.score(no_trades) is None
    assert ZeroDteConcentrationEstimator.score(no_trades) is None

    estimate = DealerPressureEstimator.estimate(
        surface,
        bundle.contracts,
        (),
        SPOT,
        DealerSideAssumption.UNKNOWN_SYMMETRIC,
    )
    assert estimate.gross_gamma_exposure_currency_nanos == 0


def test_signal_service_lifecycle_metrics_logs_and_bounded_audit() -> None:
    bundle, config = _base_bundle()
    service = OptionsSignalService(OptionsAnalyticsEngine(config), audit_capacity=1)
    assert service.status().state is OptionsServiceState.STARTING
    with pytest.raises(OptionsEvaluationError):
        service.analyze(bundle)
    service.start()
    assert service.status().ready
    first = service.analyze(bundle)
    assert first.decision is OptionsDecision.PUBLISH
    service.analyze(bundle)
    assert len(service.structured_logs) == 1
    assert "options_requests_total 2\n" in service.prometheus_metrics()
    assert service.shutdown()
    assert not service.status().healthy
    with pytest.raises(OptionsEvaluationError):
        service.analyze(bundle)
    assert "options_rejected_after_shutdown_total 1\n" in service.prometheus_metrics()
    with pytest.raises(OptionsEvaluationError):
        service.start()


def test_type_contract_validation_edges() -> None:
    with pytest.raises(ValueError):
        ParsedOptionSymbol("bad root", 20260901, OptionRight.CALL, 1)
    with pytest.raises(ValueError):
        ParsedOptionSymbol("TEST", 19990101, OptionRight.CALL, 1)
    with pytest.raises(ValueError):
        ParsedOptionSymbol("TEST", 20260901, OptionRight.CALL, 100_000_000)
    with pytest.raises(ValueError):
        HedgingPressureDistribution(1, 2, 3)
    with pytest.raises(ValueError):
        DealerPressureEstimate(
            DealerSideAssumption.UNKNOWN_SYMMETRIC,
            PPM + 1,
            0,
            0,
            0,
            0,
            0,
            0,
            HedgingPressureDistribution(400_000, 200_000, 400_000),
        )
    for creator in (
        lambda: QuoteValidationConfig(stale_after_ns=0),
        lambda: QuoteValidationConfig(maximum_spread_ppm_of_underlying=0),
        lambda: ImpliedVolatilityConfig(maximum_iterations=0),
        lambda: VolatilitySurfaceConfig(minimum_points=0),
        lambda: OptionsAnalyticsConfig(
            ModelId(_id(7, 1)), ModelVersion(_id(8, 1)), forecast_ttl_ns=0
        ),
        lambda: OptionsAnalyticsConfig(
            ModelId(_id(7, 1)),
            ModelVersion(_id(8, 1)),
            low_volatility_ppm=500_000,
            high_volatility_ppm=100_000,
        ),
        lambda: OptionsSignalService(
            OptionsAnalyticsEngine(_base_bundle()[1]), audit_capacity=0
        ),
    ):
        with pytest.raises(ValueError):
            creator()


def test_result_and_input_are_content_addressed_and_reject_future_data() -> None:
    bundle, config = _base_bundle()
    assert bundle.sha256 == bundle.sha256
    assert bundle.canonical_bytes() == bundle.canonical_bytes()
    result = OptionsAnalyticsEngine(config).evaluate(bundle)
    assert result.sha256 == result.sha256

    with pytest.raises(ValueError):
        replace(bundle, schema_version="2.0.0")
    with pytest.raises(ValueError):
        replace(
            bundle,
            quotes=(bundle.quotes[0], bundle.quotes[0]),
        )
    with pytest.raises(ValueError):
        replace(
            bundle,
            underlying=replace(
                bundle.underlying,
                received_wall_clock_utc_ns=AS_OF + 1,
            ),
        )
    with pytest.raises(ValueError):
        replace(
            result,
            decision=OptionsDecision.ABSTAIN,
        )
    with pytest.raises(ValueError):
        replace(result, forecast_contract_bytes=b"bad")
    with pytest.raises(ValueError):
        OptionsAnalyticsResult(
            **{
                **{
                    field: getattr(result, field)
                    for field in result.__dataclass_fields__
                },
                "input_sha256": bytes(32),
            }
        )


def test_remaining_numerical_and_surface_failure_branches(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    bundle, _ = _base_bundle()
    contract = bundle.contracts[0]
    with pytest.raises(OptionsNumericalError):
        black_scholes_price_currency_nanos(
            contract,
            0,
            bundle.carry,
            AS_OF,
            200_000,
        )

    monkeypatch.setattr(options_pricing_module, "_normal_pdf", lambda _: math.inf)
    with pytest.raises(OptionsNumericalError):
        black_scholes_price_currency_nanos(
            contract,
            SPOT,
            bundle.carry,
            AS_OF,
            200_000,
        )
    monkeypatch.undo()

    theoretical = black_scholes_price_currency_nanos(
        contract,
        SPOT,
        bundle.carry,
        AS_OF,
        321_123,
    )
    one_step = ImpliedVolatilitySolver(
        ImpliedVolatilityConfig(
            maximum_iterations=1,
            price_tolerance_currency_nanos=0,
            volatility_tolerance_ppm=1,
        )
    )
    with pytest.raises(OptionsNumericalError, match="iteration"):
        one_step.solve(contract, theoretical, SPOT, bundle.carry, AS_OF)

    with pytest.raises(ValueError):
        OptionSymbolParser.parse("TEST \t260901C00100000")
    with pytest.raises(ValueError):
        SyntheticSurfaceGenerator.quotes(bundle.contracts, bundle, 300_000, -1)

    builder = VolatilitySurfaceBuilder(
        VolatilitySurfaceConfig(minimum_points=2, minimum_expirations=1)
    )
    expiry = AS_OF + NANOS_PER_DAY
    call_monotonic_bad = builder.build(
        (
            _point(1, expiry, 90, OptionRight.CALL, 300_000, 1_000),
            _point(2, expiry, 100, OptionRight.CALL, 300_000, 2_000_000),
        ),
        AS_OF,
    )
    assert call_monotonic_bad.quality is SurfaceQuality.INVALID
    put_monotonic_bad = builder.build(
        (
            _point(1, expiry, 90, OptionRight.PUT, 300_000, 2_000_000),
            _point(2, expiry, 100, OptionRight.PUT, 300_000, 1_000),
        ),
        AS_OF,
    )
    assert put_monotonic_bad.quality is SurfaceQuality.INVALID


def test_remaining_feature_regime_and_service_branches() -> None:
    bundle, config = _base_bundle()
    engine = OptionsAnalyticsEngine(config)
    result = engine.evaluate(bundle)
    one_expiry = VolatilitySurface(
        tuple(
            point
            for point in result.surface.points
            if point.expiration_wall_clock_utc_ns
            == result.surface.points[0].expiration_wall_clock_utc_ns
        ),
        SurfaceQuality.HEALTHY,
        (),
    )
    _, slope = SkewAndTermStructureFeatures.calculate(one_expiry, SPOT)
    assert slope is None
    call_only = VolatilitySurface(
        tuple(point for point in one_expiry.points if point.right is OptionRight.CALL),
        SurfaceQuality.HEALTHY,
        (),
    )
    assert OptionImpliedMoveEstimator.estimate(call_only, SPOT) is None

    invalid_surface = replace(result.surface, quality=SurfaceQuality.INVALID)
    confidence, _ = ConfidenceEstimator.estimate(
        result.quote_assessments,
        invalid_surface,
        len(bundle.open_interest),
        1,
    )
    assert confidence == 0
    assert engine._regime(300_000, 0, invalid_surface) is OptionsRegime.STRESSED
    assert (
        engine._regime(
            300_000,
            config.event_skew_threshold_ppm,
            result.surface,
        )
        is OptionsRegime.EVENT_PREMIUM
    )

    empty_bundle = replace(
        bundle,
        contracts=(),
        quotes=(),
        trades=(),
        open_interest=(),
    )
    empty_result = engine.evaluate(empty_bundle)
    assert OptionsReasonCode.NO_CONTRACTS in empty_result.reason_codes
    assert empty_result.regime is OptionsRegime.INSUFFICIENT

    lower, _ = arbitrage_bounds_currency_nanos(
        bundle.contracts[0],
        SPOT,
        bundle.carry,
        AS_OF,
    )
    zero_time_value_quote = replace(
        bundle.quotes[0],
        bid_price_currency_nanos=lower,
        ask_price_currency_nanos=lower + 1,
    )
    numerical_bundle = replace(
        bundle,
        quotes=(zero_time_value_quote, *bundle.quotes[1:]),
    )
    service = OptionsSignalService(engine, 2)
    service.start()
    numerical_result = service.analyze(numerical_bundle)
    assert numerical_result.decision is OptionsDecision.ABSTAIN
    assert "options_numerical_failures_total 1\n" in service.prometheus_metrics()
    crossed = replace(
        bundle,
        quotes=tuple(
            replace(
                quote,
                bid_price_currency_nanos=quote.ask_price_currency_nanos + 1,
            )
            for quote in bundle.quotes
        ),
    )
    assert service.analyze(crossed).decision is OptionsDecision.ABSTAIN
    assert "options_abstentions_total 2\n" in service.prometheus_metrics()


def test_remaining_contract_validation_and_canonical_branches() -> None:
    bundle, config = _base_bundle()
    result = OptionsAnalyticsEngine(config).evaluate(bundle)
    contract = bundle.contracts[0]
    quote = bundle.quotes[0]
    trade = bundle.trades[0]
    interest = bundle.open_interest[0]

    invalid_constructors = (
        lambda: replace(contract, corporate_action_revision=-1),
        lambda: replace(
            contract,
            effective_wall_clock_utc_ns=contract.expiration_wall_clock_utc_ns,
        ),
        lambda: replace(quote, ask_price_currency_nanos=0),
        lambda: replace(quote, bid_price_currency_nanos=-1),
        lambda: replace(trade, quantity_contracts=0),
        lambda: replace(interest, open_interest_contracts=-1),
        lambda: replace(
            interest,
            available_wall_clock_utc_ns=interest.as_of_wall_clock_utc_ns - 1,
        ),
        lambda: CarryObservation(PPM * 6, 0, AS_OF - 1, AS_OF),
        lambda: CarryObservation(0, PPM * 6, AS_OF - 1, AS_OF),
        lambda: CarryObservation(0, 0, AS_OF, AS_OF - 1),
        lambda: OptionsAnalyticsConfig(
            config.model_id,
            config.model_version,
            event_skew_threshold_ppm=0,
        ),
        lambda: OptionsAnalyticsConfig(
            config.model_id,
            config.model_version,
            audit_capacity=0,
        ),
    )
    for creator in invalid_constructors:
        with pytest.raises(ValueError):
            creator()

    duplicate_contract = replace(
        bundle.contracts[1],
        contract_id=contract.contract_id,
    )
    unknown_quote = replace(quote, contract_id=GlobalEventId(_id(99, 2)))
    invalid_bundles: tuple[Callable[[], object], ...] = (
        lambda: replace(bundle, quotes=bundle.quotes * 1_366),
        lambda: replace(
            bundle,
            contracts=(contract, duplicate_contract, *bundle.contracts[2:]),
        ),
        lambda: replace(
            bundle,
            underlying=replace(
                bundle.underlying, instrument_id=InstrumentId(_id(99, 1))
            ),
        ),
        lambda: replace(bundle, quotes=(unknown_quote,)),
        lambda: replace(
            bundle,
            open_interest=(bundle.open_interest[0], bundle.open_interest[0]),
        ),
        lambda: replace(
            bundle,
            carry=replace(bundle.carry, available_wall_clock_utc_ns=AS_OF + 1),
        ),
        lambda: replace(
            bundle,
            quotes=(replace(quote, received_wall_clock_utc_ns=AS_OF + 1),),
        ),
        lambda: replace(
            bundle,
            trades=(replace(trade, received_wall_clock_utc_ns=AS_OF + 1),),
        ),
        lambda: replace(
            bundle,
            open_interest=(replace(interest, available_wall_clock_utc_ns=AS_OF + 1),),
        ),
    )
    for bundle_creator in invalid_bundles:
        with pytest.raises(ValueError):
            bundle_creator()

    invalid_results: tuple[Callable[[], object], ...] = (
        lambda: replace(result, schema_version="2.0.0"),
        lambda: replace(result, confidence_ppm=PPM + 1),
        lambda: replace(
            result,
            reason_codes=(result.reason_codes[0], result.reason_codes[0]),
        ),
        lambda: replace(result, forecast_id=None),
        lambda: replace(
            result,
            valid_until_process_monotonic_time_ns=(
                result.production_process_monotonic_time_ns
            ),
        ),
    )
    for result_creator in invalid_results:
        with pytest.raises(ValueError):
            result_creator()

    with pytest.raises(TypeError):
        options_types_module._canonical_value(1.5)
    with pytest.raises(OptionsEvaluationError):
        options_analytics_module._median([])
