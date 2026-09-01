"""Off-hot-path options regime, volatility, and dealer-pressure signal service."""

from __future__ import annotations

import hashlib
import math
from dataclasses import dataclass, field, fields
from typing import Final, cast

from aegis_mx_intelligence.build_info import VERSION, current_build_info
from aegis_mx_intelligence.contracts import (
    ForecastId,
    ForecastTargetCode,
    ForecastUnitCode,
    GlobalEventId,
    ModelForecastContractInput,
    ModelId,
    ModelVersion,
    build_model_forecast_contract,
)
from aegis_mx_intelligence.news_types import derived_identifier
from aegis_mx_intelligence.options_pricing import (
    GreeksEngine,
    ImpliedVolatilityConfig,
    ImpliedVolatilitySolver,
    OptionsNumericalError,
    QuoteValidationConfig,
    QuoteValidator,
    VolatilitySurfaceBuilder,
    VolatilitySurfaceConfig,
    black_scholes_price_currency_nanos,
)
from aegis_mx_intelligence.options_types import (
    MAX_INT64,
    NANOS_PER_CURRENCY_UNIT,
    NANOS_PER_DAY,
    PPM,
    SHA256_BYTES,
    DealerPressureEstimate,
    DealerSideAssumption,
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
    OptionsServiceMetrics,
    OptionsServiceState,
    OptionsServiceStatus,
    QuoteAssessment,
    QuoteValidity,
    SurfacePoint,
    SurfaceQuality,
    VolatilitySurface,
)

MINIMUM_TERM_EXPIRATIONS: Final = 2


class OptionsEvaluationError(ValueError):
    """Fail-closed options evaluation, replay, or service-lifecycle error."""


def _clamp(value: int, lower: int, upper: int) -> int:
    return min(upper, max(lower, value))


def _median(values: list[int]) -> int:
    if not values:
        msg = "options median requires at least one value"
        raise OptionsEvaluationError(msg)
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[middle]
    return (ordered[middle - 1] + ordered[middle]) // 2


@dataclass(frozen=True, slots=True)
class OptionsAnalyticsConfig:
    """Immutable configuration for deterministic options analytics."""

    model_id: ModelId
    model_version: ModelVersion
    forecast_ttl_ns: int = 5_000_000_000
    forecast_horizon_ns: int = 300_000_000_000
    dealer_side_assumption: DealerSideAssumption = (
        DealerSideAssumption.UNKNOWN_SYMMETRIC
    )
    low_volatility_ppm: int = 150_000
    high_volatility_ppm: int = 500_000
    event_skew_threshold_ppm: int = 100_000
    ood_volatility_ppm: int = 2_000_000
    quote_validation: QuoteValidationConfig = field(
        default_factory=QuoteValidationConfig
    )
    implied_volatility: ImpliedVolatilityConfig = field(
        default_factory=ImpliedVolatilityConfig
    )
    surface: VolatilitySurfaceConfig = field(default_factory=VolatilitySurfaceConfig)
    audit_capacity: int = 256

    def __post_init__(self) -> None:
        """Validate durations, ordered regime thresholds, and audit bounds."""
        if self.forecast_ttl_ns <= 0 or self.forecast_horizon_ns <= 0:
            msg = "options forecast TTL and horizon must be positive"
            raise ValueError(msg)
        if not 0 < self.low_volatility_ppm < self.high_volatility_ppm:
            msg = "options volatility regime thresholds are not ordered"
            raise ValueError(msg)
        if self.event_skew_threshold_ppm <= 0 or self.ood_volatility_ppm <= 0:
            msg = "options skew and OOD thresholds must be positive"
            raise ValueError(msg)
        if self.audit_capacity <= 0:
            msg = "options audit capacity must be positive"
            raise ValueError(msg)

    def sha256(self) -> str:
        """Return a deterministic, secret-free effective configuration hash."""
        values = (
            self.model_id.hex(),
            self.model_version.hex(),
            self.forecast_ttl_ns,
            self.forecast_horizon_ns,
            int(self.dealer_side_assumption),
            self.low_volatility_ppm,
            self.high_volatility_ppm,
            self.event_skew_threshold_ppm,
            self.ood_volatility_ppm,
            self.quote_validation.stale_after_ns,
            self.quote_validation.maximum_spread_ppm_of_underlying,
            self.quote_validation.arbitrage_tolerance_currency_nanos,
            self.implied_volatility.maximum_iterations,
            self.implied_volatility.price_tolerance_currency_nanos,
            self.implied_volatility.volatility_tolerance_ppm,
            self.implied_volatility.maximum_volatility_ppm,
            self.surface.minimum_points,
            self.surface.minimum_expirations,
            self.surface.price_tolerance_currency_nanos,
            self.audit_capacity,
        )
        return hashlib.sha256(
            "|".join(str(value) for value in values).encode("ascii")
        ).hexdigest()


class SkewAndTermStructureFeatures:
    """Deterministic surface skew and expiry-slope features."""

    @staticmethod
    def calculate(
        surface: VolatilitySurface,
        spot_currency_nanos: int,
    ) -> tuple[int | None, int | None]:
        """Return put-minus-call wing skew and far-minus-near median IV."""
        if not surface.points:
            return None, None
        put_wing = [
            point.implied_volatility_ppm
            for point in surface.points
            if point.right is OptionRight.PUT
            and point.strike_currency_nanos < spot_currency_nanos
        ]
        call_wing = [
            point.implied_volatility_ppm
            for point in surface.points
            if point.right is OptionRight.CALL
            and point.strike_currency_nanos > spot_currency_nanos
        ]
        skew = (
            None
            if not put_wing or not call_wing
            else _median(put_wing) - _median(call_wing)
        )
        expirations = sorted(
            {point.expiration_wall_clock_utc_ns for point in surface.points}
        )
        slope = None
        if len(expirations) >= MINIMUM_TERM_EXPIRATIONS:
            near = [
                point.implied_volatility_ppm
                for point in surface.points
                if point.expiration_wall_clock_utc_ns == expirations[0]
            ]
            far = [
                point.implied_volatility_ppm
                for point in surface.points
                if point.expiration_wall_clock_utc_ns == expirations[-1]
            ]
            slope = _median(far) - _median(near)
        return skew, slope


class UnusualVolumeDetector:
    """Observed trade-volume to observed-open-interest ratio."""

    @staticmethod
    def score(bundle: OptionsInputBundle) -> int | None:
        """Return a capped PPM ratio without inferring trade direction."""
        if not bundle.trades or not bundle.open_interest:
            return None
        volume = sum(item.quantity_contracts for item in bundle.trades)
        open_interest = sum(
            item.open_interest_contracts for item in bundle.open_interest
        )
        if open_interest <= 0:
            return PPM if volume > 0 else 0
        return min(PPM, (volume * PPM) // open_interest)


class ZeroDteConcentrationEstimator:
    """Observed same-UTC-date volume concentration estimator."""

    @staticmethod
    def score(bundle: OptionsInputBundle) -> int | None:
        """Return the share of trade volume expiring within the current UTC day."""
        if not bundle.trades:
            return None
        contract_index = {item.contract_id: item for item in bundle.contracts}
        day_end = ((bundle.as_of_wall_clock_utc_ns // NANOS_PER_DAY) + 1) * (
            NANOS_PER_DAY
        )
        total = sum(item.quantity_contracts for item in bundle.trades)
        zero_dte = sum(
            item.quantity_contracts
            for item in bundle.trades
            if contract_index[item.contract_id].expiration_wall_clock_utc_ns <= day_end
        )
        return (zero_dte * PPM) // total


class OptionImpliedMoveEstimator:
    """Nearest-expiry, nearest-strike observed straddle estimator."""

    @staticmethod
    def estimate(
        surface: VolatilitySurface,
        spot_currency_nanos: int,
    ) -> int | None:
        """Return ATM straddle midpoint divided by observed underlying midpoint."""
        if not surface.points:
            return None
        nearest_expiry = min(
            point.expiration_wall_clock_utc_ns for point in surface.points
        )
        points = [
            point
            for point in surface.points
            if point.expiration_wall_clock_utc_ns == nearest_expiry
        ]
        strikes = sorted(
            {point.strike_currency_nanos for point in points},
            key=lambda strike: (abs(strike - spot_currency_nanos), strike),
        )
        for strike in strikes:
            call = next(
                (
                    item
                    for item in points
                    if item.strike_currency_nanos == strike
                    and item.right is OptionRight.CALL
                ),
                None,
            )
            put = next(
                (
                    item
                    for item in points
                    if item.strike_currency_nanos == strike
                    and item.right is OptionRight.PUT
                ),
                None,
            )
            if call is not None and put is not None:
                return min(
                    PPM,
                    ((call.midpoint_currency_nanos + put.midpoint_currency_nanos) * PPM)
                    // spot_currency_nanos,
                )
        return None


class DealerPressureEstimator:
    """Approximate exposure under an explicit, unobservable dealer-side sign."""

    @staticmethod
    def estimate(
        surface: VolatilitySurface,
        contracts: tuple[OptionContract, ...],
        open_interest: tuple[OpenInterestObservation, ...],
        spot_currency_nanos: int,
        assumption: DealerSideAssumption,
    ) -> DealerPressureEstimate:
        """Aggregate bounded one-percent gamma/vanna and one-day charm shocks."""
        contract_index = {item.contract_id: item for item in contracts}
        interest_index = {
            item.contract_id: item.open_interest_contracts for item in open_interest
        }
        gamma = 0
        vanna = 0
        charm = 0
        spot_units = spot_currency_nanos / NANOS_PER_CURRENCY_UNIT
        for point in surface.points:
            oi = interest_index.get(point.contract_id, 0)
            contract = contract_index[point.contract_id]
            quantity = oi * contract.multiplier_units
            gamma_value = point.greeks.gamma_per_currency_ppb / 1_000_000_000
            vanna_value = point.greeks.vanna_per_vol_ppb / 1_000_000_000
            charm_value = point.greeks.charm_per_year_ppb / 1_000_000_000
            gamma += round(
                abs(0.5 * gamma_value * (0.01 * spot_units) ** 2 * quantity)
                * NANOS_PER_CURRENCY_UNIT
            )
            vanna += round(
                abs(vanna_value * 0.01 * spot_units * quantity)
                * NANOS_PER_CURRENCY_UNIT
            )
            charm += round(
                abs(charm_value * spot_units * quantity / 365.0)
                * NANOS_PER_CURRENCY_UNIT
            )
        gamma = min(MAX_INT64, gamma)
        vanna = min(MAX_INT64, vanna)
        charm = min(MAX_INT64, charm)
        if assumption is DealerSideAssumption.UNKNOWN_SYMMETRIC:
            sign = 0
            uncertainty = PPM
            distribution = HedgingPressureDistribution(400_000, 200_000, 400_000)
        elif assumption is DealerSideAssumption.DEALER_SHORT_CUSTOMER_POSITION:
            sign = -1
            uncertainty = 350_000
            distribution = HedgingPressureDistribution(700_000, 100_000, 200_000)
        else:
            sign = 1
            uncertainty = 350_000
            distribution = HedgingPressureDistribution(200_000, 100_000, 700_000)
        return DealerPressureEstimate(
            assumption=assumption,
            assumption_uncertainty_ppm=uncertainty,
            gross_gamma_exposure_currency_nanos=gamma,
            gross_vanna_exposure_currency_nanos=vanna,
            gross_charm_exposure_currency_nanos=charm,
            signed_gamma_exposure_currency_nanos=sign * gamma,
            signed_vanna_exposure_currency_nanos=sign * vanna,
            signed_charm_exposure_currency_nanos=sign * charm,
            distribution=distribution,
        )


class ConfidenceEstimator:
    """Bounded quality and out-of-distribution estimator."""

    @staticmethod
    def estimate(
        assessments: tuple[QuoteAssessment, ...],
        surface: VolatilitySurface,
        open_interest_count: int,
        ood_volatility_ppm: int,
    ) -> tuple[int, int]:
        """Return confidence and OOD scores from observable quality only."""
        if not assessments:
            return 0, PPM
        valid = sum(item.validity is QuoteValidity.VALID for item in assessments)
        confidence = (valid * PPM) // len(assessments)
        if surface.quality is SurfaceQuality.DEGRADED:
            confidence //= 2
        elif surface.quality is SurfaceQuality.INVALID:
            confidence = 0
        if open_interest_count == 0:
            confidence = (confidence * 3) // 4
        extreme = sum(
            point.implied_volatility_ppm > ood_volatility_ppm
            for point in surface.points
        )
        invalid = len(assessments) - valid
        ood = ((invalid + extreme) * PPM) // max(1, len(assessments))
        return _clamp(confidence, 0, PPM), _clamp(ood, 0, PPM)


class SyntheticSurfaceGenerator:
    """License-clean deterministic BSM quote generator for infrastructure tests."""

    @staticmethod
    def quotes(
        contracts: tuple[OptionContract, ...],
        bundle_template: OptionsInputBundle,
        volatility_ppm: int,
        half_spread_currency_nanos: int,
    ) -> tuple[OptionQuote, ...]:
        """Generate reproducible theoretical midpoints with a fixed spread."""
        if half_spread_currency_nanos < 0:
            msg = "synthetic half spread cannot be negative"
            raise ValueError(msg)
        output: list[OptionQuote] = []
        venue = bundle_template.quotes[0].venue_id
        for contract in contracts:
            midpoint = black_scholes_price_currency_nanos(
                contract,
                bundle_template.underlying.midpoint_currency_nanos,
                bundle_template.carry,
                bundle_template.as_of_wall_clock_utc_ns,
                volatility_ppm,
            )
            output.append(
                OptionQuote(
                    contract_id=contract.contract_id,
                    venue_id=venue,
                    bid_price_currency_nanos=max(
                        0, midpoint - half_spread_currency_nanos
                    ),
                    ask_price_currency_nanos=midpoint + half_spread_currency_nanos,
                    bid_quantity_contracts=10,
                    ask_quantity_contracts=10,
                    exchange_event_time_ns=bundle_template.underlying.exchange_event_time_ns,
                    nic_receive_time_ns=bundle_template.underlying.nic_receive_time_ns,
                    received_wall_clock_utc_ns=bundle_template.as_of_wall_clock_utc_ns,
                )
            )
        return tuple(output)


class OptionsAnalyticsEngine:
    """Pure deterministic vertical slice for options signal publication."""

    _CRITICAL_REASONS: Final = frozenset(
        {
            OptionsReasonCode.NO_CONTRACTS,
            OptionsReasonCode.NO_VALID_QUOTES,
            OptionsReasonCode.INSUFFICIENT_SURFACE,
            OptionsReasonCode.CALENDAR_ARBITRAGE,
            OptionsReasonCode.CONVEXITY_ARBITRAGE,
            OptionsReasonCode.NUMERICAL_FAILURE,
        }
    )

    def __init__(self, config: OptionsAnalyticsConfig) -> None:
        """Bind immutable validator, solver, Greeks, and surface components."""
        self._config = config
        self._validator = QuoteValidator(config.quote_validation)
        self._solver = ImpliedVolatilitySolver(config.implied_volatility)
        self._greeks = GreeksEngine()
        self._surface = VolatilitySurfaceBuilder(config.surface)

    @property
    def configuration_sha256(self) -> str:
        """Expose the deterministic effective configuration hash."""
        return self._config.sha256()

    @staticmethod
    def _require_stable_volatility(volatility_ppm: int) -> None:
        if volatility_ppm == 0:
            msg = "zero time value has no stable Greeks"
            raise OptionsNumericalError(msg)

    def _assess_and_build(
        self,
        bundle: OptionsInputBundle,
    ) -> tuple[tuple[QuoteAssessment, ...], VolatilitySurface, bool]:
        contract_index = {item.contract_id: item for item in bundle.contracts}
        assessments: list[QuoteAssessment] = []
        points: list[SurfacePoint] = []
        numerical_failure = False
        for quote in sorted(bundle.quotes, key=lambda item: item.contract_id.hex()):
            contract = contract_index[quote.contract_id]
            assessment = self._validator.assess(
                contract,
                quote,
                bundle.underlying.midpoint_currency_nanos,
                bundle.carry,
                bundle.as_of_wall_clock_utc_ns,
            )
            assessments.append(assessment)
            if assessment.validity is not QuoteValidity.VALID:
                continue
            midpoint = cast("int", assessment.midpoint_currency_nanos)
            try:
                volatility = self._solver.solve(
                    contract,
                    midpoint,
                    bundle.underlying.midpoint_currency_nanos,
                    bundle.carry,
                    bundle.as_of_wall_clock_utc_ns,
                )
                self._require_stable_volatility(volatility)
                greeks = self._greeks.calculate(
                    contract,
                    bundle.underlying.midpoint_currency_nanos,
                    bundle.carry,
                    bundle.as_of_wall_clock_utc_ns,
                    volatility,
                )
                log_moneyness = round(
                    math.log(
                        contract.strike_currency_nanos
                        / bundle.underlying.midpoint_currency_nanos
                    )
                    * PPM
                )
            except OptionsNumericalError:
                numerical_failure = True
                continue
            points.append(
                SurfacePoint(
                    contract_id=contract.contract_id,
                    expiration_wall_clock_utc_ns=(
                        contract.expiration_wall_clock_utc_ns
                    ),
                    strike_currency_nanos=contract.strike_currency_nanos,
                    right=contract.right,
                    implied_volatility_ppm=volatility,
                    log_moneyness_ppm=log_moneyness,
                    midpoint_currency_nanos=midpoint,
                    greeks=greeks,
                )
            )
        return (
            tuple(assessments),
            self._surface.build(tuple(points), bundle.as_of_wall_clock_utc_ns),
            numerical_failure,
        )

    def _reasons(
        self,
        bundle: OptionsInputBundle,
        assessments: tuple[QuoteAssessment, ...],
        surface: VolatilitySurface,
        *,
        numerical_failure: bool,
    ) -> tuple[OptionsReasonCode, ...]:
        reasons: set[OptionsReasonCode] = set(surface.reason_codes)
        if not bundle.contracts:
            reasons.add(OptionsReasonCode.NO_CONTRACTS)
        if not any(item.validity is QuoteValidity.VALID for item in assessments):
            reasons.add(OptionsReasonCode.NO_VALID_QUOTES)
        mapping = {
            QuoteValidity.STALE: OptionsReasonCode.STALE_QUOTES,
            QuoteValidity.WIDE: OptionsReasonCode.WIDE_QUOTES,
            QuoteValidity.CROSSED: OptionsReasonCode.CROSSED_QUOTES,
            QuoteValidity.ARBITRAGE_INCONSISTENT: (
                OptionsReasonCode.ARBITRAGE_INCONSISTENT
            ),
            QuoteValidity.EXPIRED: OptionsReasonCode.EXPIRED_CONTRACT,
            QuoteValidity.UNSUPPORTED_STYLE: (
                OptionsReasonCode.UNSUPPORTED_EXERCISE_STYLE
            ),
        }
        for assessment in assessments:
            reason = mapping.get(assessment.validity)
            if reason is not None:
                reasons.add(reason)
        if numerical_failure:
            reasons.add(OptionsReasonCode.NUMERICAL_FAILURE)
        if not bundle.open_interest:
            reasons.add(OptionsReasonCode.MISSING_OPEN_INTEREST)
        if not bundle.trades:
            reasons.add(OptionsReasonCode.MISSING_TRADES)
        if (
            self._config.dealer_side_assumption
            is DealerSideAssumption.UNKNOWN_SYMMETRIC
        ):
            reasons.add(OptionsReasonCode.UNKNOWN_DEALER_SIDE)
        return tuple(sorted(reasons, key=int))

    def _regime(
        self,
        expected_volatility: int | None,
        skew: int | None,
        surface: VolatilitySurface,
    ) -> OptionsRegime:
        if expected_volatility is None:
            return OptionsRegime.INSUFFICIENT
        if surface.quality is SurfaceQuality.INVALID:
            return OptionsRegime.STRESSED
        if abs(skew or 0) >= self._config.event_skew_threshold_ppm:
            return OptionsRegime.EVENT_PREMIUM
        if expected_volatility < self._config.low_volatility_ppm:
            return OptionsRegime.LOW_VOLATILITY
        if expected_volatility > self._config.high_volatility_ppm:
            return OptionsRegime.HIGH_VOLATILITY
        return OptionsRegime.BALANCED

    @staticmethod
    def _pinning_pressure(
        surface: VolatilitySurface,
        open_interest: tuple[OpenInterestObservation, ...],
    ) -> int | None:
        if not surface.points or not open_interest:
            return None
        oi = {item.contract_id: item.open_interest_contracts for item in open_interest}
        by_strike: dict[int, int] = {}
        for point in surface.points:
            exposure = abs(point.greeks.gamma_per_currency_ppb) * oi.get(
                point.contract_id, 0
            )
            by_strike[point.strike_currency_nanos] = (
                by_strike.get(point.strike_currency_nanos, 0) + exposure
            )
        total = sum(by_strike.values())
        return None if total == 0 else (max(by_strike.values()) * PPM) // total

    def _forecast(
        self,
        bundle: OptionsInputBundle,
        expected_volatility: int,
        confidence: int,
        ood: int,
    ) -> tuple[ForecastId, int, bytes]:
        forecast_id = ForecastId(
            derived_identifier(
                "options-forecast",
                bundle.sha256,
                self._config.model_version.hex(),
            )
        )
        expiration = (
            bundle.production_process_monotonic_time_ns + self._config.forecast_ttl_ns
        )
        lower = max(0, (expected_volatility * 4) // 5)
        upper = min(MAX_INT64, (expected_volatility * 6) // 5)
        contract = ModelForecastContractInput(
            record_id=GlobalEventId(
                derived_identifier("options-forecast-record", forecast_id.hex())
            ),
            forecast_id=forecast_id,
            session_id=bundle.session_id,
            model_id=self._config.model_id,
            model_version=self._config.model_version,
            instrument_id=bundle.underlying_instrument_id,
            feature_snapshot_id=bundle.feature_snapshot_id,
            configuration_version=bundle.configuration_version,
            expected_return_ppm=0,
            return_p10_ppm=0,
            return_p50_ppm=0,
            return_p90_ppm=0,
            probability_down_ppm=0,
            probability_flat_ppm=PPM,
            probability_up_ppm=0,
            volatility_ppm=expected_volatility,
            confidence_ppm=confidence,
            calibration_score_ppm=0,
            data_quality_score_ppm=confidence,
            ood_score_ppm=ood,
            horizon_ns=self._config.forecast_horizon_ns,
            as_of_exchange_event_time_ns=bundle.underlying.exchange_event_time_ns,
            production_process_monotonic_time_ns=(
                bundle.production_process_monotonic_time_ns
            ),
            expiration_process_monotonic_time_ns=expiration,
            forecast_target=ForecastTargetCode.REALIZED_VOLATILITY,
            forecast_unit=ForecastUnitCode.VOLATILITY_PPM,
            target_value=expected_volatility,
            target_p10=lower,
            target_p50=expected_volatility,
            target_p90=upper,
        )
        return forecast_id, expiration, build_model_forecast_contract(contract)

    def evaluate(self, bundle: OptionsInputBundle) -> OptionsAnalyticsResult:
        """Evaluate observed options state and publish or strictly abstain."""
        assessments, surface, numerical_failure = self._assess_and_build(bundle)
        expected_volatility = (
            None
            if not surface.points
            else _median([item.implied_volatility_ppm for item in surface.points])
        )
        skew, term_slope = SkewAndTermStructureFeatures.calculate(
            surface,
            bundle.underlying.midpoint_currency_nanos,
        )
        unusual = UnusualVolumeDetector.score(bundle)
        zero_dte = ZeroDteConcentrationEstimator.score(bundle)
        implied_move = OptionImpliedMoveEstimator.estimate(
            surface,
            bundle.underlying.midpoint_currency_nanos,
        )
        dealer = DealerPressureEstimator.estimate(
            surface,
            bundle.contracts,
            bundle.open_interest,
            bundle.underlying.midpoint_currency_nanos,
            self._config.dealer_side_assumption,
        )
        pinning = self._pinning_pressure(surface, bundle.open_interest)
        confidence, ood = ConfidenceEstimator.estimate(
            assessments,
            surface,
            len(bundle.open_interest),
            self._config.ood_volatility_ppm,
        )
        reasons = self._reasons(
            bundle,
            assessments,
            surface,
            numerical_failure=numerical_failure,
        )
        if ood == PPM:
            reasons = tuple(sorted({*reasons, OptionsReasonCode.OOD_INPUT}, key=int))
        publish = expected_volatility is not None and not any(
            item in self._CRITICAL_REASONS for item in reasons
        )
        forecast: tuple[ForecastId, int, bytes] | None = None
        if publish:
            forecast = self._forecast(
                bundle,
                cast("int", expected_volatility),
                confidence,
                ood,
            )
        return OptionsAnalyticsResult(
            model_id=self._config.model_id,
            model_version=self._config.model_version,
            input_sha256=bundle.sha256,
            decision=OptionsDecision.PUBLISH if publish else OptionsDecision.ABSTAIN,
            regime=self._regime(expected_volatility, skew, surface),
            quote_assessments=assessments,
            surface=surface,
            dealer_pressure=dealer,
            expected_volatility_ppm=(expected_volatility if publish else None),
            option_implied_move_ppm=implied_move,
            skew_ppm=skew,
            term_structure_slope_ppm=term_slope,
            unusual_volume_score_ppm=unusual,
            zero_dte_concentration_ppm=zero_dte,
            expected_pinning_pressure_ppm=pinning,
            confidence_ppm=confidence,
            ood_score_ppm=ood,
            reason_codes=reasons,
            production_process_monotonic_time_ns=(
                bundle.production_process_monotonic_time_ns
            ),
            forecast_id=None if forecast is None else forecast[0],
            valid_until_process_monotonic_time_ns=(
                None if forecast is None else forecast[1]
            ),
            forecast_contract_bytes=None if forecast is None else forecast[2],
        )

    def verify_replay(
        self,
        bundle: OptionsInputBundle,
        expected_result_sha256: bytes,
    ) -> OptionsAnalyticsResult:
        """Re-evaluate and require a byte-stable detailed output digest."""
        if len(expected_result_sha256) != SHA256_BYTES:
            msg = "expected options replay digest must contain 32 bytes"
            raise OptionsEvaluationError(msg)
        result = self.evaluate(bundle)
        if result.sha256 != expected_result_sha256:
            msg = "options replay output digest mismatch"
            raise OptionsEvaluationError(msg)
        return result


class OptionsSignalService:
    """Non-trading service wrapper with bounded metrics, audit, and shutdown."""

    def __init__(self, engine: OptionsAnalyticsEngine, audit_capacity: int) -> None:
        """Create a starting service without any order-entry dependency."""
        if audit_capacity <= 0:
            msg = "options service audit capacity must be positive"
            raise ValueError(msg)
        self._engine = engine
        self._audit_capacity = audit_capacity
        self._state = OptionsServiceState.STARTING
        self._metrics = OptionsServiceMetrics()
        self._audit: list[dict[str, str | int]] = []

    def start(self) -> None:
        """Enter readiness after construction and immutable config validation."""
        if self._state is OptionsServiceState.STOPPED:
            msg = "stopped options service cannot restart in the same process epoch"
            raise OptionsEvaluationError(msg)
        self._state = OptionsServiceState.READY

    def _record(self, event: str, bundle_sha256: bytes) -> None:
        entry: dict[str, str | int] = {
            "event": event,
            "input_sha256": bundle_sha256.hex(),
            "ordinal": self._metrics.requests_total,
        }
        if len(self._audit) == self._audit_capacity:
            self._audit.pop(0)
        self._audit.append(entry)

    def analyze(self, bundle: OptionsInputBundle) -> OptionsAnalyticsResult:
        """Evaluate one bundle without network, disk, OMS, or gateway calls."""
        if self._state is not OptionsServiceState.READY:
            if self._state is OptionsServiceState.STOPPED:
                self._metrics.rejected_after_shutdown_total += 1
            msg = "options service is not ready"
            raise OptionsEvaluationError(msg)
        self._metrics.requests_total += 1
        result = self._engine.evaluate(bundle)
        if result.decision is OptionsDecision.PUBLISH:
            self._metrics.publications_total += 1
            event = "forecast_published"
        else:
            self._metrics.abstentions_total += 1
            event = "forecast_abstained"
        if OptionsReasonCode.NUMERICAL_FAILURE in result.reason_codes:
            self._metrics.numerical_failures_total += 1
        self._record(event, bundle.sha256)
        return result

    def status(self) -> OptionsServiceStatus:
        """Expose common build, health, readiness, and configuration state."""
        healthy = self._state in {
            OptionsServiceState.STARTING,
            OptionsServiceState.READY,
            OptionsServiceState.DEGRADED,
        }
        return OptionsServiceStatus(
            state=self._state,
            healthy=healthy,
            ready=self._state is OptionsServiceState.READY,
            version=VERSION,
            configuration_sha256=self._engine.configuration_sha256,
            live_trading_capable=current_build_info().live_trading_capable,
        )

    def prometheus_metrics(self) -> str:
        """Render fixed-cardinality metrics without contract labels."""
        return "".join(
            f"options_{item.name} {getattr(self._metrics, item.name)}\n"
            for item in fields(self._metrics)
        )

    @property
    def structured_logs(self) -> tuple[dict[str, str | int], ...]:
        """Return a bounded immutable copy of structured audit events."""
        return tuple(dict(item) for item in self._audit)

    def shutdown(self) -> bool:
        """Stop admission; this synchronous service has no queued work to drain."""
        self._state = OptionsServiceState.STOPPED
        return True
