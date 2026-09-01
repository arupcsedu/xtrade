"""Deterministic rebalance, auction, and hidden-liquidity specialists."""

from __future__ import annotations

import hashlib
from dataclasses import dataclass

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
from aegis_mx_intelligence.market_specialists_types import (
    MAX_INT64,
    PPM,
    SHA256_BYTES,
    AuctionAllocationAssumption,
    AuctionEstimate,
    AuctionInputBundle,
    BookSide,
    ClearingPriceDistribution,
    DistributionPpm,
    HiddenLiquidityEstimate,
    HiddenLiquidityInputBundle,
    PassiveFlowAssumption,
    QuantityRange,
    RebalanceEstimate,
    RebalanceInputBundle,
    SignedIntRange,
    SpecialistContext,
    SpecialistDecision,
    SpecialistKind,
    SpecialistReasonCode,
    SpecialistResult,
)
from aegis_mx_intelligence.news_types import derived_identifier

MAX_PERCENTILE = 100


class MarketSpecialistEvaluationError(ValueError):
    """Fail-closed specialist evaluation or replay error."""


def _clamp(value: int, lower: int, upper: int) -> int:
    return min(upper, max(lower, value))


def _trunc_div(numerator: int, denominator: int) -> int:
    if denominator <= 0:
        msg = "specialist deterministic division requires a positive denominator"
        raise MarketSpecialistEvaluationError(msg)
    sign = -1 if numerator < 0 else 1
    return sign * (abs(numerator) // denominator)


def _median(values: list[int]) -> int:
    if not values:
        msg = "specialist median requires at least one value"
        raise MarketSpecialistEvaluationError(msg)
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[middle]
    return _trunc_div(ordered[middle - 1] + ordered[middle], 2)


def _quantile(values: list[int], percentile: int) -> int:
    if not values or not 0 <= percentile <= MAX_PERCENTILE:
        msg = "specialist quantile input is empty or percentile is invalid"
        raise MarketSpecialistEvaluationError(msg)
    ordered = sorted(values)
    return ordered[((len(ordered) - 1) * percentile) // 100]


def _ordered_reasons(
    values: set[SpecialistReasonCode],
) -> tuple[SpecialistReasonCode, ...]:
    return tuple(sorted(values, key=int))


def _effective_quality(context: SpecialistContext) -> int:
    if not context.provenance:
        return 0
    return min(
        context.data_quality_score_ppm,
        *(item.data_quality_score_ppm for item in context.provenance),
    )


def _common_reasons(
    context: SpecialistContext,
    minimum_data_quality_ppm: int,
) -> set[SpecialistReasonCode]:
    reasons: set[SpecialistReasonCode] = set()
    if context.symbol_policy.is_disabled(context.instrument_id):
        reasons.add(SpecialistReasonCode.SYMBOL_DISABLED)
    if not context.provenance:
        reasons.add(SpecialistReasonCode.MISSING_PROVENANCE)
    if _effective_quality(context) < minimum_data_quality_ppm:
        reasons.add(SpecialistReasonCode.LOW_DATA_QUALITY)
    if any(not item.authenticated for item in context.provenance):
        reasons.add(SpecialistReasonCode.UNAUTHENTICATED_SOURCE)
    return reasons


@dataclass(frozen=True, slots=True)
class _BaseConfig:
    model_id: ModelId
    model_version: ModelVersion
    forecast_ttl_ns: int
    forecast_horizon_ns: int
    minimum_data_quality_ppm: int

    def validate(self) -> None:
        if not 0 < self.forecast_ttl_ns <= MAX_INT64 or not (
            0 < self.forecast_horizon_ns <= MAX_INT64
        ):
            msg = "specialist forecast TTL and horizon must be positive int64 values"
            raise ValueError(msg)
        if not 0 <= self.minimum_data_quality_ppm <= PPM:
            msg = "specialist minimum data quality is outside PPM bounds"
            raise ValueError(msg)

    def sha256(self, *values: int) -> str:
        encoded = "|".join(
            str(item)
            for item in (
                self.model_id.hex(),
                self.model_version.hex(),
                self.forecast_ttl_ns,
                self.forecast_horizon_ns,
                self.minimum_data_quality_ppm,
                *values,
            )
        ).encode("ascii")
        return hashlib.sha256(encoded).hexdigest()


def _forecast(
    *,
    specialist: SpecialistKind,
    context: SpecialistContext,
    input_sha256: bytes,
    config: _BaseConfig,
    expected_return_ppm: int,
    distribution: DistributionPpm,
    confidence_ppm: int,
    uncertainty_ppm: int,
) -> tuple[ForecastId, int, bytes]:
    forecast_id = ForecastId(
        derived_identifier(
            "market-specialist-forecast",
            int(specialist),
            input_sha256,
            config.model_version.hex(),
        )
    )
    expiration = context.production_process_monotonic_time_ns + config.forecast_ttl_ns
    directional_strength = min(500_000, abs(expected_return_ppm) * 5)
    probability_flat = PPM - directional_strength
    probability_down = directional_strength if expected_return_ppm < 0 else 0
    probability_up = directional_strength if expected_return_ppm > 0 else 0
    if expected_return_ppm == 0:
        probability_flat = PPM
    volatility = max(1, (distribution.p90 - distribution.p10) // 2)
    wire = ModelForecastContractInput(
        record_id=GlobalEventId(
            derived_identifier("market-specialist-record", forecast_id.hex())
        ),
        forecast_id=forecast_id,
        session_id=context.session_id,
        model_id=config.model_id,
        model_version=config.model_version,
        instrument_id=context.instrument_id,
        feature_snapshot_id=context.feature_snapshot_id,
        configuration_version=context.configuration_version,
        expected_return_ppm=expected_return_ppm,
        return_p10_ppm=distribution.p10,
        return_p50_ppm=distribution.p50,
        return_p90_ppm=distribution.p90,
        probability_down_ppm=probability_down,
        probability_flat_ppm=probability_flat,
        probability_up_ppm=probability_up,
        volatility_ppm=volatility,
        confidence_ppm=confidence_ppm,
        calibration_score_ppm=0,
        data_quality_score_ppm=_effective_quality(context),
        ood_score_ppm=uncertainty_ppm,
        horizon_ns=config.forecast_horizon_ns,
        as_of_exchange_event_time_ns=context.as_of_exchange_event_time_ns,
        production_process_monotonic_time_ns=(
            context.production_process_monotonic_time_ns
        ),
        expiration_process_monotonic_time_ns=expiration,
        forecast_target=ForecastTargetCode.RETURN,
        forecast_unit=ForecastUnitCode.RETURN_PPM,
        target_value=expected_return_ppm,
        target_p10=distribution.p10,
        target_p50=distribution.p50,
        target_p90=distribution.p90,
    )
    return forecast_id, expiration, build_model_forecast_contract(wire)


def _abstention[T](
    specialist: SpecialistKind,
    context: SpecialistContext,
    input_sha256: bytes,
    config: _BaseConfig,
    reasons: set[SpecialistReasonCode],
) -> SpecialistResult[T]:
    return SpecialistResult(
        specialist,
        context.event_id,
        context.instrument_id,
        config.model_id,
        config.model_version,
        input_sha256,
        context.symbol_policy.sha256,
        SpecialistDecision.ABSTAIN,
        _ordered_reasons(reasons),
        _effective_quality(context),
        PPM,
        None,
        context.production_process_monotonic_time_ns,
        None,
        None,
        None,
    )


def _published[T](
    specialist: SpecialistKind,
    context: SpecialistContext,
    input_sha256: bytes,
    config: _BaseConfig,
    reasons: set[SpecialistReasonCode],
    uncertainty_ppm: int,
    estimate: T,
    distribution: DistributionPpm,
) -> SpecialistResult[T]:
    confidence = min(_effective_quality(context), PPM - uncertainty_ppm)
    forecast_id, expiration, forecast_bytes = _forecast(
        specialist=specialist,
        context=context,
        input_sha256=input_sha256,
        config=config,
        expected_return_ppm=distribution.p50,
        distribution=distribution,
        confidence_ppm=confidence,
        uncertainty_ppm=uncertainty_ppm,
    )
    return SpecialistResult(
        specialist,
        context.event_id,
        context.instrument_id,
        config.model_id,
        config.model_version,
        input_sha256,
        context.symbol_policy.sha256,
        SpecialistDecision.PUBLISH,
        _ordered_reasons(reasons),
        _effective_quality(context),
        uncertainty_ppm,
        estimate,
        context.production_process_monotonic_time_ns,
        forecast_id,
        expiration,
        forecast_bytes,
    )


@dataclass(frozen=True, slots=True)
class IndexRebalanceSpecialistConfig:
    """Deterministic flow, history, and timing configuration."""

    model_id: ModelId
    model_version: ModelVersion
    forecast_ttl_ns: int = 60_000_000_000
    forecast_horizon_ns: int = 3_600_000_000_000
    minimum_data_quality_ppm: int = 700_000
    minimum_history_count: int = 3
    maximum_etf_input_age_ns: int = 300_000_000_000
    timing_window_before_ns: int = 30_000_000_000
    timing_window_after_ns: int = 300_000_000_000

    def __post_init__(self) -> None:
        """Validate common settings, history, and timing durations."""
        self.base.validate()
        if self.minimum_history_count <= 0 or self.maximum_etf_input_age_ns <= 0:
            msg = "rebalance history count and ETF input age must be positive"
            raise ValueError(msg)
        if self.timing_window_before_ns <= 0 or self.timing_window_after_ns <= 0:
            msg = "rebalance timing windows must be positive"
            raise ValueError(msg)

    @property
    def base(self) -> _BaseConfig:
        """Return the shared immutable forecast configuration."""
        return _BaseConfig(
            self.model_id,
            self.model_version,
            self.forecast_ttl_ns,
            self.forecast_horizon_ns,
            self.minimum_data_quality_ppm,
        )

    def sha256(self) -> str:
        """Return deterministic effective configuration identity."""
        return self.base.sha256(
            self.minimum_history_count,
            self.maximum_etf_input_age_ns,
            self.timing_window_before_ns,
            self.timing_window_after_ns,
        )


class IndexRebalanceSpecialist:
    """Estimate passive flows without treating tracker behavior as observed."""

    def __init__(self, config: IndexRebalanceSpecialistConfig) -> None:
        """Bind immutable model and calculation policy."""
        self._config = config

    @property
    def configuration_sha256(self) -> str:
        """Expose the immutable effective configuration hash."""
        return self._config.sha256()

    @staticmethod
    def _signed_range(
        lower_magnitude: int, upper_magnitude: int, sign: int
    ) -> SignedIntRange:
        if sign < 0:
            return SignedIntRange(-upper_magnitude, -lower_magnitude)
        return SignedIntRange(lower_magnitude, upper_magnitude)

    def evaluate(
        self,
        bundle: RebalanceInputBundle,
    ) -> SpecialistResult[RebalanceEstimate]:
        """Calculate flow, timing, auction demand, impact, and reversal estimates."""
        reasons = _common_reasons(
            bundle.context,
            self._config.minimum_data_quality_ppm,
        )
        if len(bundle.history) < self._config.minimum_history_count:
            reasons.add(SpecialistReasonCode.INSUFFICIENT_HISTORY)
        source_index = {item.source_id: item for item in bundle.context.provenance}
        if (
            bundle.context.as_of_wall_clock_utc_ns
            - source_index[bundle.etf.source_id].received_wall_clock_utc_ns
            > self._config.maximum_etf_input_age_ns
        ):
            reasons.add(SpecialistReasonCode.STALE_INPUT)
        if bundle.assumption is PassiveFlowAssumption.UNKNOWN_BOUNDED_TRACKING:
            reasons.add(SpecialistReasonCode.ASSUMPTION_LIMITED)
        critical = reasons - {SpecialistReasonCode.ASSUMPTION_LIMITED}
        if critical:
            return _abstention(
                SpecialistKind.INDEX_REBALANCE,
                bundle.context,
                bundle.sha256,
                self._config.base,
                reasons,
            )
        announcement = bundle.announcement
        weight_delta = announcement.new_weight_ppm - announcement.old_weight_ppm
        sign = -1 if weight_delta < 0 else 1
        base_flow = abs(
            _trunc_div(
                weight_delta * announcement.estimated_tracking_assets_currency_nanos,
                PPM,
            )
        )
        uncertainty = max(
            announcement.tracking_assets_uncertainty_ppm,
            PPM - _effective_quality(bundle.context),
            _trunc_div(PPM, len(bundle.history) + 1),
        )
        if bundle.assumption is PassiveFlowAssumption.UNKNOWN_BOUNDED_TRACKING:
            uncertainty = max(uncertainty, 250_000)
        lower_flow = _trunc_div(
            _trunc_div(base_flow * (PPM - uncertainty), PPM)
            * announcement.passive_participation_lower_ppm,
            PPM,
        )
        upper_flow = min(
            MAX_INT64,
            _trunc_div(
                _trunc_div(base_flow * (PPM + uncertainty), PPM)
                * announcement.passive_participation_upper_ppm,
                PPM,
            ),
        )
        flow_range = self._signed_range(lower_flow, upper_flow, sign)
        total_share_lower = lower_flow // bundle.etf.reference_price_currency_nanos
        total_share_upper = upper_flow // bundle.etf.reference_price_currency_nanos
        historical_participation = _median(
            [
                min(
                    PPM,
                    _trunc_div(
                        abs(item.passive_demand_shares) * PPM,
                        item.auction_volume_shares,
                    ),
                )
                for item in bundle.history
            ]
        )
        demand_range = self._signed_range(
            _trunc_div(total_share_lower * historical_participation, PPM),
            _trunc_div(total_share_upper * historical_participation, PPM),
            sign,
        )
        current_participation = min(
            PPM,
            _trunc_div(
                max(abs(demand_range.lower), abs(demand_range.upper)) * PPM,
                bundle.etf.forecast_closing_auction_volume_shares,
            ),
        )
        impact_samples: list[int] = []
        for item in bundle.history:
            sample_participation = max(
                1,
                min(
                    PPM,
                    _trunc_div(
                        abs(item.passive_demand_shares) * PPM,
                        item.auction_volume_shares,
                    ),
                ),
            )
            magnitude = _trunc_div(
                abs(item.realized_impact_ppm) * current_participation,
                sample_participation,
            )
            impact_samples.append(sign * min(PPM, magnitude))
        impact = DistributionPpm(
            _quantile(impact_samples, 10),
            _quantile(impact_samples, 50),
            _quantile(impact_samples, 90),
        )
        reversal = _trunc_div(
            sum(item.reversed_within_window for item in bundle.history) * PPM,
            len(bundle.history),
        )
        estimate = RebalanceEstimate(
            bundle.assumption,
            flow_range,
            announcement.effective_auction_wall_clock_utc_ns
            - self._config.timing_window_before_ns,
            announcement.effective_auction_wall_clock_utc_ns
            + self._config.timing_window_after_ns,
            demand_range,
            impact,
            reversal,
            uncertainty,
        )
        return _published(
            SpecialistKind.INDEX_REBALANCE,
            bundle.context,
            bundle.sha256,
            self._config.base,
            reasons,
            uncertainty,
            estimate,
            impact,
        )

    def verify_replay(
        self,
        bundle: RebalanceInputBundle,
        expected_result_sha256: bytes,
    ) -> SpecialistResult[RebalanceEstimate]:
        """Re-evaluate a rebalance day and require an identical result digest."""
        return _verify_replay(self.evaluate(bundle), expected_result_sha256)


@dataclass(frozen=True, slots=True)
class AuctionSpecialistConfig:
    """Deterministic auction history, freshness, and participation policy."""

    model_id: ModelId
    model_version: ModelVersion
    forecast_ttl_ns: int = 1_000_000_000
    forecast_horizon_ns: int = 60_000_000_000
    minimum_data_quality_ppm: int = 800_000
    minimum_history_count: int = 3
    maximum_input_age_ns: int = 2_000_000_000
    maximum_participation_ppm: int = 50_000

    def __post_init__(self) -> None:
        """Validate common settings, history, freshness, and cap."""
        self.base.validate()
        if self.minimum_history_count <= 0 or self.maximum_input_age_ns <= 0:
            msg = "auction history count and maximum input age must be positive"
            raise ValueError(msg)
        if not 0 < self.maximum_participation_ppm <= PPM:
            msg = "auction maximum participation is outside PPM bounds"
            raise ValueError(msg)

    @property
    def base(self) -> _BaseConfig:
        """Return the shared immutable forecast configuration."""
        return _BaseConfig(
            self.model_id,
            self.model_version,
            self.forecast_ttl_ns,
            self.forecast_horizon_ns,
            self.minimum_data_quality_ppm,
        )

    def sha256(self) -> str:
        """Return deterministic effective configuration identity."""
        return self.base.sha256(
            self.minimum_history_count,
            self.maximum_input_age_ns,
            self.maximum_participation_ppm,
        )


class AuctionSpecialist:
    """Estimate closing-auction outcomes without submitting participation."""

    def __init__(self, config: AuctionSpecialistConfig) -> None:
        """Bind immutable model and conservative cap policy."""
        self._config = config

    @property
    def configuration_sha256(self) -> str:
        """Expose the immutable effective configuration hash."""
        return self._config.sha256()

    def evaluate(
        self,
        bundle: AuctionInputBundle,
    ) -> SpecialistResult[AuctionEstimate]:
        """Calculate clearing price, fill, conservative cap, and uncertainty."""
        reasons = _common_reasons(
            bundle.context,
            self._config.minimum_data_quality_ppm,
        )
        if len(bundle.history) < self._config.minimum_history_count:
            reasons.add(SpecialistReasonCode.INSUFFICIENT_HISTORY)
        source_index = {item.source_id: item for item in bundle.context.provenance}
        current_source_ids = (
            bundle.snapshot.source_id,
            bundle.volume_forecast.source_id,
            bundle.rebalance_state.source_id,
        )
        if any(
            bundle.context.as_of_wall_clock_utc_ns
            - source_index[source_id].received_wall_clock_utc_ns
            > self._config.maximum_input_age_ns
            for source_id in current_source_ids
        ):
            reasons.add(SpecialistReasonCode.STALE_INPUT)
        if bundle.rebalance_state.active:
            reasons.add(SpecialistReasonCode.REBALANCE_ACTIVE)
        if bundle.assumption is AuctionAllocationAssumption.QUEUE_PRIORITY_UNKNOWN:
            reasons.add(SpecialistReasonCode.ASSUMPTION_LIMITED)
        informational = {
            SpecialistReasonCode.REBALANCE_ACTIVE,
            SpecialistReasonCode.ASSUMPTION_LIMITED,
        }
        if reasons - informational:
            return _abstention(
                SpecialistKind.AUCTION,
                bundle.context,
                bundle.sha256,
                self._config.base,
                reasons,
            )
        demand_midpoint = _trunc_div(
            bundle.rebalance_state.demand_shares.lower
            + bundle.rebalance_state.demand_shares.upper,
            2,
        )
        combined_imbalance = bundle.snapshot.signed_imbalance_quantity + (
            demand_midpoint if bundle.rebalance_state.active else 0
        )
        sign = -1 if combined_imbalance < 0 else 1
        current_participation = min(
            PPM,
            _trunc_div(
                abs(combined_imbalance) * PPM,
                bundle.volume_forecast.forecast_quantity,
            ),
        )
        indicative_return = _trunc_div(
            (
                bundle.snapshot.indicative_match_price_currency_nanos
                - bundle.snapshot.reference_price_currency_nanos
            )
            * PPM,
            bundle.snapshot.reference_price_currency_nanos,
        )
        return_samples: list[int] = []
        for item in bundle.history:
            historical_participation = max(
                1,
                min(
                    PPM,
                    _trunc_div(
                        abs(item.signed_imbalance_quantity) * PPM,
                        item.auction_volume_quantity,
                    ),
                ),
            )
            residual = _trunc_div(
                abs(item.clearing_return_ppm) * current_participation,
                historical_participation,
            )
            return_samples.append(
                _clamp(indicative_return + sign * residual, -PPM, PPM)
            )
        returns = DistributionPpm(
            _quantile(return_samples, 10),
            _quantile(return_samples, 50),
            _quantile(return_samples, 90),
        )
        reference = bundle.snapshot.reference_price_currency_nanos
        prices = ClearingPriceDistribution(
            max(1, _trunc_div(reference * (PPM + returns.p10), PPM)),
            max(1, _trunc_div(reference * (PPM + returns.p50), PPM)),
            max(1, _trunc_div(reference * (PPM + returns.p90), PPM)),
        )
        observable_fill = _trunc_div(
            bundle.snapshot.paired_quantity * PPM,
            bundle.snapshot.paired_quantity + abs(combined_imbalance) + 1,
        )
        historical_fill = _median(
            [item.eligible_fill_probability_ppm for item in bundle.history]
        )
        fill_probability = _trunc_div(observable_fill + historical_fill, 2)
        cap = min(
            bundle.snapshot.paired_quantity,
            _trunc_div(
                bundle.volume_forecast.forecast_quantity
                * self._config.maximum_participation_ppm,
                PPM,
            ),
        )
        uncertainty = max(
            PPM - _effective_quality(bundle.context),
            PPM - bundle.volume_forecast.confidence_ppm,
            _trunc_div(PPM, len(bundle.history) + 1),
            bundle.rebalance_state.uncertainty_ppm
            if bundle.rebalance_state.active
            else 0,
            300_000
            if bundle.assumption is AuctionAllocationAssumption.QUEUE_PRIORITY_UNKNOWN
            else 0,
        )
        estimate = AuctionEstimate(
            bundle.assumption,
            prices,
            fill_probability,
            cap,
            uncertainty,
        )
        return _published(
            SpecialistKind.AUCTION,
            bundle.context,
            bundle.sha256,
            self._config.base,
            reasons,
            uncertainty,
            estimate,
            returns,
        )

    def verify_replay(
        self,
        bundle: AuctionInputBundle,
        expected_result_sha256: bytes,
    ) -> SpecialistResult[AuctionEstimate]:
        """Re-evaluate a closing auction and require an identical result digest."""
        return _verify_replay(self.evaluate(bundle), expected_result_sha256)


@dataclass(frozen=True, slots=True)
class HiddenLiquiditySpecialistConfig:
    """Deterministic freshness, evidence, and probability configuration."""

    model_id: ModelId
    model_version: ModelVersion
    forecast_ttl_ns: int = 500_000_000
    forecast_horizon_ns: int = 10_000_000_000
    minimum_data_quality_ppm: int = 800_000
    minimum_evidence_count: int = 3
    maximum_input_age_ns: int = 1_000_000_000

    def __post_init__(self) -> None:
        """Validate common settings, evidence count, and freshness."""
        self.base.validate()
        if self.minimum_evidence_count <= 0 or self.maximum_input_age_ns <= 0:
            msg = "hidden-liquidity evidence count and age must be positive"
            raise ValueError(msg)

    @property
    def base(self) -> _BaseConfig:
        """Return the shared immutable forecast configuration."""
        return _BaseConfig(
            self.model_id,
            self.model_version,
            self.forecast_ttl_ns,
            self.forecast_horizon_ns,
            self.minimum_data_quality_ppm,
        )

    def sha256(self) -> str:
        """Return deterministic effective configuration identity."""
        return self.base.sha256(
            self.minimum_evidence_count,
            self.maximum_input_age_ns,
        )


class HiddenLiquiditySpecialist:
    """Infer replenishment patterns without claiming hidden size is observed."""

    def __init__(self, config: HiddenLiquiditySpecialistConfig) -> None:
        """Bind immutable model and evidence policy."""
        self._config = config

    @property
    def configuration_sha256(self) -> str:
        """Expose the immutable effective configuration hash."""
        return self._config.sha256()

    def evaluate(
        self,
        bundle: HiddenLiquidityInputBundle,
    ) -> SpecialistResult[HiddenLiquidityEstimate]:
        """Estimate iceberg probability, latent-size range, and persistence."""
        reasons = _common_reasons(
            bundle.context,
            self._config.minimum_data_quality_ppm,
        )
        evidence_count = (
            len(bundle.executions)
            + len(bundle.replenishments)
            + len(bundle.partial_fill_sequences)
        )
        if evidence_count < self._config.minimum_evidence_count:
            reasons.add(SpecialistReasonCode.INSUFFICIENT_LIQUIDITY_EVIDENCE)
        relevant_source_ids = {
            bundle.venue_behavior.source_id,
            *(item.source_id for item in bundle.displayed_depth),
            *(item.source_id for item in bundle.executions),
            *(item.source_id for item in bundle.replenishments),
            *(item.source_id for item in bundle.partial_fill_sequences),
            *(item.source_id for item in bundle.price_impacts),
        }
        source_index = {item.source_id: item for item in bundle.context.provenance}
        if any(
            bundle.context.as_of_wall_clock_utc_ns
            - source_index[source_id].received_wall_clock_utc_ns
            > self._config.maximum_input_age_ns
            for source_id in relevant_source_ids
        ):
            reasons.add(SpecialistReasonCode.STALE_INPUT)
        reasons.add(SpecialistReasonCode.ASSUMPTION_LIMITED)
        if reasons - {SpecialistReasonCode.ASSUMPTION_LIMITED}:
            return _abstention(
                SpecialistKind.HIDDEN_LIQUIDITY,
                bundle.context,
                bundle.sha256,
                self._config.base,
                reasons,
            )
        displayed = sum(item.displayed_quantity for item in bundle.displayed_depth)
        executed = sum(item.executed_quantity for item in bundle.executions)
        replenished = sum(item.replenished_quantity for item in bundle.replenishments)
        replenishment_ratio = _trunc_div(
            replenished * PPM,
            displayed + replenished + 1,
        )
        sequence_excess = sum(
            max(
                0,
                item.cumulative_executed_quantity - item.initial_displayed_quantity,
            )
            for item in bundle.partial_fill_sequences
        )
        sequence_total = sum(
            item.cumulative_executed_quantity for item in bundle.partial_fill_sequences
        )
        sequence_ratio = _trunc_div(sequence_excess * PPM, sequence_total + 1)
        repetition_score = min(
            PPM,
            _trunc_div(
                len(bundle.replenishments) * PPM,
                self._config.minimum_evidence_count,
            ),
        )
        raw_probability = _trunc_div(
            replenishment_ratio * 45 + sequence_ratio * 30 + repetition_score * 25,
            100,
        )
        behavior = bundle.venue_behavior
        probability = _clamp(
            raw_probability
            - _trunc_div(behavior.baseline_replenishment_probability_ppm, 2)
            - behavior.false_positive_probability_ppm,
            0,
            PPM,
        )
        inferred_lower = max(replenished, executed - displayed, sequence_excess)
        inferred_upper = min(
            MAX_INT64,
            inferred_lower
            + _trunc_div(
                behavior.typical_displayed_quantity
                * behavior.maximum_latent_multiple_ppm,
                PPM,
            ),
        )
        impact_magnitude = (
            0
            if not bundle.price_impacts
            else min(
                PPM,
                _median([abs(item.signed_impact_ppm) for item in bundle.price_impacts]),
            )
        )
        persistence = _trunc_div(probability * (PPM - impact_magnitude), PPM)
        evidence_score = min(
            PPM,
            _trunc_div(
                evidence_count * PPM,
                self._config.minimum_evidence_count * 2,
            ),
        )
        confidence = _trunc_div(
            _trunc_div(
                _effective_quality(bundle.context) * evidence_score,
                PPM,
            )
            * (PPM - behavior.false_positive_probability_ppm),
            PPM,
        )
        uncertainty = PPM - confidence
        estimate = HiddenLiquidityEstimate(
            bundle.assumption,
            probability,
            QuantityRange(inferred_lower, inferred_upper),
            persistence,
            confidence,
        )
        direction = 1 if bundle.candidate_side is BookSide.BID else -1
        expected_return = direction * min(50_000, _trunc_div(persistence, 20))
        width = max(1, _trunc_div(uncertainty, 20))
        distribution = DistributionPpm(
            _clamp(expected_return - width, -PPM, PPM),
            expected_return,
            _clamp(expected_return + width, -PPM, PPM),
        )
        return _published(
            SpecialistKind.HIDDEN_LIQUIDITY,
            bundle.context,
            bundle.sha256,
            self._config.base,
            reasons,
            uncertainty,
            estimate,
            distribution,
        )

    def verify_replay(
        self,
        bundle: HiddenLiquidityInputBundle,
        expected_result_sha256: bytes,
    ) -> SpecialistResult[HiddenLiquidityEstimate]:
        """Re-evaluate observation sequences and require an identical digest."""
        return _verify_replay(self.evaluate(bundle), expected_result_sha256)


def _verify_replay[T](
    result: SpecialistResult[T],
    expected_result_sha256: bytes,
) -> SpecialistResult[T]:
    if len(expected_result_sha256) != SHA256_BYTES:
        msg = "expected specialist replay digest must contain 32 bytes"
        raise MarketSpecialistEvaluationError(msg)
    if result.sha256 != expected_result_sha256:
        msg = "market specialist replay output digest mismatch"
        raise MarketSpecialistEvaluationError(msg)
    return result
