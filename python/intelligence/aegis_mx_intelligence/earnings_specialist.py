"""Deterministic, leakage-safe earnings-event specialist outside the hot path."""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from enum import IntEnum
from typing import Final, Protocol

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
from aegis_mx_intelligence.earnings_types import (
    MAX_ABSOLUTE_RETURN_PPM,
    MAX_INT64,
    MAX_VOLATILITY_PPM,
    MIN_INT64,
    PPM,
    SHA256_BYTES,
    AccountingBasis,
    AccountingQualityFlag,
    DirectionDistribution,
    EarningsInputBundle,
    EarningsMetricIdentity,
    EarningsMetricKind,
    EarningsMetricUnit,
    EarningsPhase,
    EarningsReasonCode,
    EarningsSourceKind,
    EarningsSpecialistResult,
    ExpectedGapRange,
    GuidanceChange,
    GuidanceRange,
    MetricSurprise,
    NormalizedEarningsMetric,
)
from aegis_mx_intelligence.news_types import derived_identifier

MAX_HOOKS: Final = 16
MAX_TRANSITIONS: Final = 32


class EarningsEvaluationError(ValueError):
    """Fail-closed specialist evaluation or lifecycle error."""


@dataclass(frozen=True, slots=True)
class EarningsSpecialistConfig:
    """Immutable integer configuration for one versioned specialist."""

    model_id: ModelId
    model_version: ModelVersion
    forecast_ttl_ns: int = 5_000_000_000
    default_discovery_duration_ns: int = 1_800_000_000_000
    minimum_discovery_duration_ns: int = 60_000_000_000
    maximum_discovery_duration_ns: int = 14_400_000_000_000
    minimum_history_count: int = 3
    gaap_divergence_threshold_ppm: int = 150_000
    adjustment_materiality_threshold_ppm: int = 100_000
    segment_mismatch_threshold_ppm: int = 50_000
    epsilon_currency_nanos: int = 1_000_000
    epsilon_currency_nanos_per_share: int = 1_000
    epsilon_ppm: int = 1
    epsilon_quantity_units: int = 1

    def __post_init__(self) -> None:
        """Validate bounded lifetimes, thresholds, history, and epsilon values."""
        durations = (
            self.forecast_ttl_ns,
            self.default_discovery_duration_ns,
            self.minimum_discovery_duration_ns,
            self.maximum_discovery_duration_ns,
        )
        if any(value <= 0 for value in durations):
            msg = "earnings forecast and discovery durations must be positive"
            raise ValueError(msg)
        if not (
            self.minimum_discovery_duration_ns
            <= self.default_discovery_duration_ns
            <= self.maximum_discovery_duration_ns
        ):
            msg = "default price-discovery duration is outside configured bounds"
            raise ValueError(msg)
        if self.minimum_history_count <= 0:
            msg = "minimum_history_count must be positive"
            raise ValueError(msg)
        thresholds = (
            self.gaap_divergence_threshold_ppm,
            self.adjustment_materiality_threshold_ppm,
            self.segment_mismatch_threshold_ppm,
        )
        if any(not 0 <= value <= PPM for value in thresholds):
            msg = "earnings quality thresholds must be within [0, 1,000,000] PPM"
            raise ValueError(msg)
        epsilons = (
            self.epsilon_currency_nanos,
            self.epsilon_currency_nanos_per_share,
            self.epsilon_ppm,
            self.epsilon_quantity_units,
        )
        if any(value <= 0 for value in epsilons):
            msg = "earnings surprise epsilon values must be positive"
            raise ValueError(msg)

    def epsilon(self, unit: EarningsMetricUnit) -> int:
        """Return the explicit epsilon in the supplied metric unit."""
        return {
            EarningsMetricUnit.CURRENCY_NANOS: self.epsilon_currency_nanos,
            EarningsMetricUnit.CURRENCY_NANOS_PER_SHARE: (
                self.epsilon_currency_nanos_per_share
            ),
            EarningsMetricUnit.PPM: self.epsilon_ppm,
            EarningsMetricUnit.QUANTITY_UNITS: self.epsilon_quantity_units,
        }[unit]

    def sha256(self) -> str:
        """Return a deterministic secret-free configuration hash."""
        values = (
            self.model_id.hex(),
            self.model_version.hex(),
            self.forecast_ttl_ns,
            self.default_discovery_duration_ns,
            self.minimum_discovery_duration_ns,
            self.maximum_discovery_duration_ns,
            self.minimum_history_count,
            self.gaap_divergence_threshold_ppm,
            self.adjustment_materiality_threshold_ppm,
            self.segment_mismatch_threshold_ppm,
            self.epsilon_currency_nanos,
            self.epsilon_currency_nanos_per_share,
            self.epsilon_ppm,
            self.epsilon_quantity_units,
        )
        encoded = "|".join(str(value) for value in values).encode("ascii")
        return hashlib.sha256(encoded).hexdigest()


def _trunc_div(numerator: int, denominator: int) -> int:
    if denominator <= 0:
        msg = "deterministic division denominator must be positive"
        raise EarningsEvaluationError(msg)
    sign = -1 if numerator < 0 else 1
    return sign * (abs(numerator) // denominator)


def _scaled_ratio_ppm(numerator: int, denominator: int) -> int:
    result = _trunc_div(numerator * PPM, denominator)
    if not MIN_INT64 <= result <= MAX_INT64:
        msg = "standardized earnings ratio exceeds signed 64-bit output"
        raise EarningsEvaluationError(msg)
    return result


def _midpoint(value: GuidanceRange) -> int:
    return value.lower_value + _trunc_div(
        value.upper_value - value.lower_value,
        2,
    )


def _clamp(value: int, lower: int, upper: int) -> int:
    return min(upper, max(lower, value))


def _median(values: list[int]) -> int:
    if not values:
        msg = "median requires at least one value"
        raise EarningsEvaluationError(msg)
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2 == 1:
        return ordered[middle]
    return _trunc_div(ordered[middle - 1] + ordered[middle], 2)


def _ordered_unique[T: IntEnum](values: list[T]) -> tuple[T, ...]:
    return tuple(sorted(set(values), key=int))


def _same_metric_family(
    left: EarningsMetricIdentity, right: EarningsMetricIdentity
) -> bool:
    return (
        left.kind is right.kind
        and left.fiscal_period == right.fiscal_period
        and left.segment_name == right.segment_name
    )


class EarningsSpecialist:
    """Pure specialist that publishes detailed output and one common forecast."""

    def __init__(self, config: EarningsSpecialistConfig) -> None:
        """Bind an immutable model/configuration version."""
        self._config = config

    @property
    def configuration_sha256(self) -> str:
        """Expose the immutable effective configuration hash."""
        return self._config.sha256()

    def _surprises(self, bundle: EarningsInputBundle) -> tuple[MetricSurprise, ...]:
        estimates = {item.identity: item for item in bundle.consensus}
        output = []
        for actual in bundle.actuals:
            estimate = estimates.get(actual.identity)
            if estimate is not None:
                denominator = max(
                    estimate.dispersion,
                    self._config.epsilon(actual.identity.unit),
                )
                output.append(
                    MetricSurprise(
                        actual.identity,
                        actual.value,
                        estimate.consensus_value,
                        estimate.dispersion,
                        _scaled_ratio_ppm(
                            actual.value - estimate.consensus_value,
                            denominator,
                        ),
                        None,
                    )
                )
                continue
            incompatible = any(
                _same_metric_family(actual.identity, candidate.identity)
                for candidate in bundle.consensus
            )
            output.append(
                MetricSurprise(
                    actual.identity,
                    actual.value,
                    None,
                    None,
                    None,
                    EarningsReasonCode.INCOMPATIBLE_METRIC
                    if incompatible
                    else EarningsReasonCode.MISSING_CONSENSUS,
                )
            )
        return tuple(output)

    def _guidance_changes(
        self, bundle: EarningsInputBundle
    ) -> tuple[GuidanceChange, ...]:
        prior = {item.identity: item for item in bundle.prior_guidance}
        output = []
        for current in bundle.current_guidance:
            current_midpoint = _midpoint(current)
            prior_value = prior.get(current.identity)
            if prior_value is None:
                output.append(
                    GuidanceChange(
                        current.identity,
                        current_midpoint,
                        None,
                        None,
                        None,
                        EarningsReasonCode.MISSING_PRIOR_GUIDANCE,
                    )
                )
                continue
            prior_midpoint = _midpoint(prior_value)
            delta = current_midpoint - prior_midpoint
            output.append(
                GuidanceChange(
                    current.identity,
                    current_midpoint,
                    prior_midpoint,
                    delta,
                    _scaled_ratio_ppm(
                        delta,
                        max(
                            abs(prior_midpoint),
                            self._config.epsilon(current.identity.unit),
                        ),
                    ),
                    None,
                )
            )
        return tuple(output)

    def _has_gaap_divergence(self, bundle: EarningsInputBundle) -> bool:
        actual_index = {item.identity: item for item in bundle.actuals}
        for identity, gaap in actual_index.items():
            if identity.basis is not AccountingBasis.GAAP:
                continue
            non_gaap_identity = EarningsMetricIdentity(
                identity.kind,
                AccountingBasis.NON_GAAP,
                identity.unit,
                identity.fiscal_period,
                identity.segment_name,
            )
            non_gaap = actual_index.get(non_gaap_identity)
            if non_gaap is None:
                continue
            divergence = abs(
                _scaled_ratio_ppm(
                    non_gaap.value - gaap.value,
                    max(abs(gaap.value), self._config.epsilon(identity.unit)),
                )
            )
            if divergence >= self._config.gaap_divergence_threshold_ppm:
                return True
        return False

    @staticmethod
    def _consolidated_revenue(
        bundle: EarningsInputBundle,
    ) -> NormalizedEarningsMetric | None:
        return next(
            (
                item
                for item in bundle.actuals
                if item.identity.kind is EarningsMetricKind.REVENUE
                and item.identity.basis is AccountingBasis.GAAP
            ),
            None,
        )

    def _adjustments_are_material(
        self,
        bundle: EarningsInputBundle,
        consolidated_revenue: NormalizedEarningsMetric | None,
    ) -> bool:
        if consolidated_revenue is None or not bundle.one_time_adjustments:
            return False
        adjustment_total = sum(
            abs(item.amount_currency_nanos) for item in bundle.one_time_adjustments
        )
        ratio = abs(
            _scaled_ratio_ppm(
                adjustment_total,
                max(
                    abs(consolidated_revenue.value),
                    self._config.epsilon(EarningsMetricUnit.CURRENCY_NANOS),
                ),
            )
        )
        return ratio >= self._config.adjustment_materiality_threshold_ppm

    def _segments_mismatch(
        self,
        bundle: EarningsInputBundle,
        consolidated_revenue: NormalizedEarningsMetric | None,
    ) -> bool:
        if consolidated_revenue is None:
            return False
        segment_revenue = [
            item
            for item in bundle.actuals
            if item.identity.kind is EarningsMetricKind.SEGMENT_REVENUE
            and item.identity.basis is AccountingBasis.GAAP
        ]
        if not segment_revenue:
            return False
        segment_total = sum(item.value for item in segment_revenue)
        mismatch = abs(
            _scaled_ratio_ppm(
                segment_total - consolidated_revenue.value,
                max(
                    abs(consolidated_revenue.value),
                    self._config.epsilon(EarningsMetricUnit.CURRENCY_NANOS),
                ),
            )
        )
        return mismatch >= self._config.segment_mismatch_threshold_ppm

    def _quality_flags(
        self, bundle: EarningsInputBundle
    ) -> tuple[AccountingQualityFlag, ...]:
        flags: list[AccountingQualityFlag] = []
        consolidated_revenue = self._consolidated_revenue(bundle)
        if bundle.revision > 1:
            flags.append(AccountingQualityFlag.CORRECTED_RELEASE)
        if self._has_gaap_divergence(bundle):
            flags.append(AccountingQualityFlag.GAAP_NON_GAAP_DIVERGENCE)
        if self._adjustments_are_material(bundle, consolidated_revenue):
            flags.append(AccountingQualityFlag.MATERIAL_ONE_TIME_ADJUSTMENTS)
        if self._segments_mismatch(bundle, consolidated_revenue):
            flags.append(AccountingQualityFlag.SEGMENT_TOTAL_MISMATCH)

        if not any(
            item.identity.kind is EarningsMetricKind.FREE_CASH_FLOW
            for item in bundle.actuals
        ):
            flags.append(AccountingQualityFlag.MISSING_FREE_CASH_FLOW)
        return _ordered_unique(flags)

    def _reason_codes(
        self,
        bundle: EarningsInputBundle,
        surprises: tuple[MetricSurprise, ...],
        guidance: tuple[GuidanceChange, ...],
        flags: tuple[AccountingQualityFlag, ...],
    ) -> tuple[EarningsReasonCode, ...]:
        reasons = [item.reason for item in surprises if item.reason] + [
            item.reason for item in guidance if item.reason
        ]
        source_kinds = {item.kind for item in bundle.sources}
        if EarningsSourceKind.REGULATORY_FILING not in source_kinds:
            reasons.append(EarningsReasonCode.MISSING_FILING)
        if EarningsSourceKind.TRANSCRIPT not in source_kinds:
            reasons.append(EarningsReasonCode.MISSING_TRANSCRIPT)
        if bundle.option_implied_move is None:
            reasons.append(EarningsReasonCode.MISSING_OPTION_IMPLIED_MOVE)
        if len(bundle.historical_reactions) < self._config.minimum_history_count:
            reasons.append(EarningsReasonCode.INSUFFICIENT_HISTORY)
        if not bundle.market_features.is_post_release:
            reasons.append(EarningsReasonCode.MISSING_POST_RELEASE_FEATURES)
        if bundle.revision > 1:
            reasons.append(EarningsReasonCode.CORRECTED_RELEASE)
        flag_reasons = {
            AccountingQualityFlag.GAAP_NON_GAAP_DIVERGENCE: (
                EarningsReasonCode.GAAP_NON_GAAP_DIVERGENCE
            ),
            AccountingQualityFlag.MATERIAL_ONE_TIME_ADJUSTMENTS: (
                EarningsReasonCode.MATERIAL_ONE_TIME_ADJUSTMENTS
            ),
            AccountingQualityFlag.SEGMENT_TOTAL_MISMATCH: (
                EarningsReasonCode.SEGMENT_TOTAL_MISMATCH
            ),
            AccountingQualityFlag.MISSING_FREE_CASH_FLOW: (
                EarningsReasonCode.MISSING_FREE_CASH_FLOW
            ),
        }
        reasons.extend(flag_reasons[item] for item in flags if item in flag_reasons)
        if not any(item.surprise_ppm is not None for item in surprises):
            reasons.append(EarningsReasonCode.NO_COMPARABLE_METRICS)
        return _ordered_unique(reasons)

    @staticmethod
    def _strongest(values: list[int]) -> int:
        return max(values, key=lambda value: (abs(value), value)) if values else 0

    def _prior_change_signal(self, bundle: EarningsInputBundle) -> int:
        prior = {item.identity: item for item in bundle.prior_reported_values}
        changes = []
        for actual in bundle.actuals:
            prior_value = prior.get(actual.identity)
            if prior_value is None:
                continue
            changes.append(
                _scaled_ratio_ppm(
                    actual.value - prior_value.value,
                    max(
                        abs(prior_value.value),
                        self._config.epsilon(actual.identity.unit),
                    ),
                )
            )
        return self._strongest(changes)

    def _materiality(
        self,
        bundle: EarningsInputBundle,
        surprises: tuple[MetricSurprise, ...],
        guidance: tuple[GuidanceChange, ...],
    ) -> int:
        surprise = abs(
            self._strongest(
                [
                    item.surprise_ppm
                    for item in surprises
                    if item.surprise_ppm is not None
                ]
            )
        )
        guidance_change = abs(
            self._strongest(
                [item.change_ppm for item in guidance if item.change_ppm is not None]
            )
        )
        surprise_component = _trunc_div(min(surprise, 3 * PPM), 3)
        guidance_component = min(guidance_change, PPM)
        option_component = (
            0
            if bundle.option_implied_move is None
            else min(PPM, bundle.option_implied_move.absolute_move_ppm * 5)
        )
        return _trunc_div(
            (surprise_component * 50)
            + (guidance_component * 30)
            + (option_component * 20),
            100,
        )

    def _direction_signal(
        self,
        bundle: EarningsInputBundle,
        surprises: tuple[MetricSurprise, ...],
        guidance: tuple[GuidanceChange, ...],
    ) -> int:
        surprise = self._strongest(
            [item.surprise_ppm for item in surprises if item.surprise_ppm is not None]
        )
        guidance_change = self._strongest(
            [item.change_ppm for item in guidance if item.change_ppm is not None]
        )
        prior_change = self._prior_change_signal(bundle)
        post_release = (
            bundle.market_features.return_since_release_ppm
            if bundle.market_features.is_post_release
            else 0
        )
        weighted = (
            _clamp(surprise, -PPM, PPM) * 50
            + _clamp(guidance_change, -PPM, PPM) * 25
            + _clamp(prior_change, -PPM, PPM) * 10
            + _clamp(post_release, -PPM, PPM) * 15
        )
        return _clamp(_trunc_div(weighted, 100), -PPM, PPM)

    @staticmethod
    def _direction_distribution(
        signal_ppm: int, uncertainty_ppm: int
    ) -> DirectionDistribution:
        flat = 250_000 + _trunc_div(uncertainty_ppm, 4)
        directional_mass = PPM - flat
        edge = _trunc_div(signal_ppm * (directional_mass // 2), PPM)
        up = (directional_mass // 2) + edge
        down = directional_mass - up
        return DirectionDistribution(down, flat, up)

    def _expected_volatility(
        self, bundle: EarningsInputBundle, materiality_ppm: int
    ) -> int:
        values = [
            item.realized_volatility_ppm
            for item in bundle.historical_reactions
            if item.realized_volatility_ppm > 0
        ]
        if bundle.option_implied_move is not None:
            values.append(bundle.option_implied_move.absolute_move_ppm)
        if bundle.market_features.realized_volatility_ppm > 0:
            values.append(bundle.market_features.realized_volatility_ppm)
        baseline = _median(values) if values else max(1, materiality_ppm // 2)
        adjusted = _trunc_div(baseline * (500_000 + materiality_ppm), PPM)
        return _clamp(adjusted, 0, MAX_VOLATILITY_PPM)

    def _gap_range(
        self,
        bundle: EarningsInputBundle,
        expected_return_ppm: int,
        expected_volatility_ppm: int,
    ) -> ExpectedGapRange:
        candidates = [abs(item.gap_return_ppm) for item in bundle.historical_reactions]
        if bundle.option_implied_move is not None:
            candidates.append(bundle.option_implied_move.absolute_move_ppm)
        candidates.append(max(1, expected_volatility_ppm // 2))
        magnitude = _median(candidates)
        return ExpectedGapRange(
            _clamp(
                expected_return_ppm - magnitude,
                -MAX_ABSOLUTE_RETURN_PPM,
                MAX_ABSOLUTE_RETURN_PPM,
            ),
            expected_return_ppm,
            _clamp(
                expected_return_ppm + magnitude,
                -MAX_ABSOLUTE_RETURN_PPM,
                MAX_ABSOLUTE_RETURN_PPM,
            ),
        )

    def _discovery_duration(
        self, bundle: EarningsInputBundle, materiality_ppm: int
    ) -> int:
        durations = [
            item.price_discovery_duration_ns for item in bundle.historical_reactions
        ]
        baseline = (
            _median(durations)
            if len(durations) >= self._config.minimum_history_count
            else self._config.default_discovery_duration_ns
        )
        adjusted = _trunc_div(baseline * (500_000 + materiality_ppm), PPM)
        return _clamp(
            adjusted,
            self._config.minimum_discovery_duration_ns,
            self._config.maximum_discovery_duration_ns,
        )

    def evaluate(self, bundle: EarningsInputBundle) -> EarningsSpecialistResult:
        """Evaluate one immutable bundle and publish canonical forecast bytes."""
        if not bundle.market_features.valid:
            msg = "invalid market features fail the earnings specialist closed"
            raise EarningsEvaluationError(msg)
        expiration = (
            bundle.production_process_monotonic_time_ns + self._config.forecast_ttl_ns
        )
        if expiration > (1 << 64) - 1:
            msg = "earnings forecast expiration overflows uint64"
            raise EarningsEvaluationError(msg)

        surprises = self._surprises(bundle)
        guidance = self._guidance_changes(bundle)
        flags = self._quality_flags(bundle)
        reasons = self._reason_codes(bundle, surprises, guidance, flags)
        uncertainty = min(950_000, 100_000 + (len(reasons) * 60_000))
        materiality = self._materiality(bundle, surprises, guidance)
        signal = self._direction_signal(bundle, surprises, guidance)
        expected_return = _trunc_div(signal, 2)
        direction = self._direction_distribution(signal, uncertainty)
        expected_volatility = self._expected_volatility(bundle, materiality)
        gap = self._gap_range(bundle, expected_return, expected_volatility)
        discovery_duration = self._discovery_duration(bundle, materiality)
        phase = (
            EarningsPhase.PRICE_DISCOVERY
            if bundle.market_features.is_post_release
            else EarningsPhase.RELEASE_PROCESSING
        )

        forecast_id = ForecastId(
            derived_identifier(
                "earnings-forecast",
                bundle.sha256,
                self._config.model_version.hex(),
            )
        )
        record_id = GlobalEventId(
            derived_identifier("earnings-forecast-record", forecast_id.hex())
        )
        forecast = ModelForecastContractInput(
            record_id=record_id,
            forecast_id=forecast_id,
            session_id=bundle.session_id,
            model_id=self._config.model_id,
            model_version=self._config.model_version,
            instrument_id=bundle.instrument_id,
            feature_snapshot_id=bundle.market_features.feature_snapshot_id,
            configuration_version=bundle.configuration_version,
            expected_return_ppm=expected_return,
            return_p10_ppm=gap.lower_return_ppm,
            return_p50_ppm=gap.center_return_ppm,
            return_p90_ppm=gap.upper_return_ppm,
            probability_down_ppm=direction.down_ppm,
            probability_flat_ppm=direction.flat_ppm,
            probability_up_ppm=direction.up_ppm,
            volatility_ppm=expected_volatility,
            confidence_ppm=PPM - uncertainty,
            calibration_score_ppm=0,
            data_quality_score_ppm=PPM - uncertainty,
            ood_score_ppm=uncertainty,
            horizon_ns=discovery_duration,
            as_of_exchange_event_time_ns=(
                bundle.market_features.as_of_exchange_event_time_ns
            ),
            production_process_monotonic_time_ns=(
                bundle.production_process_monotonic_time_ns
            ),
            expiration_process_monotonic_time_ns=expiration,
            forecast_target=ForecastTargetCode.RETURN,
            forecast_unit=ForecastUnitCode.RETURN_PPM,
            target_value=expected_return,
            target_p10=gap.lower_return_ppm,
            target_p50=gap.center_return_ppm,
            target_p90=gap.upper_return_ppm,
        )
        return EarningsSpecialistResult(
            earnings_event_id=bundle.earnings_event_id,
            forecast_id=forecast_id,
            model_id=self._config.model_id,
            model_version=self._config.model_version,
            input_sha256=bundle.sha256,
            revision=bundle.revision,
            phase=phase,
            surprises=surprises,
            guidance_changes=guidance,
            accounting_quality_flags=flags,
            materiality_ppm=materiality,
            direction=direction,
            expected_return_ppm=expected_return,
            expected_volatility_ppm=expected_volatility,
            expected_gap=gap,
            estimated_price_discovery_duration_ns=discovery_duration,
            uncertainty_ppm=uncertainty,
            reason_codes=reasons,
            production_process_monotonic_time_ns=(
                bundle.production_process_monotonic_time_ns
            ),
            valid_until_process_monotonic_time_ns=expiration,
            forecast_contract_bytes=build_model_forecast_contract(forecast),
        )

    def verify_replay(
        self, bundle: EarningsInputBundle, expected_result_sha256: bytes
    ) -> EarningsSpecialistResult:
        """Re-evaluate and require byte-stable detailed output."""
        if len(expected_result_sha256) != SHA256_BYTES:
            msg = "expected replay digest must contain 32 bytes"
            raise EarningsEvaluationError(msg)
        result = self.evaluate(bundle)
        if result.sha256 != expected_result_sha256:
            msg = "earnings replay output digest mismatch"
            raise EarningsEvaluationError(msg)
        return result


@dataclass(frozen=True, slots=True)
class EarningsLifecycleTransition:
    """Proposed and committed phase transition delivered to integration hooks."""

    earnings_event_id: GlobalEventId
    revision: int
    previous_phase: EarningsPhase
    next_phase: EarningsPhase
    transition_process_monotonic_time_ns: int


class EarningsLifecycleHook(Protocol):
    """Bounded integration hook with no order or gateway capability."""

    def approve_transition(self, transition: EarningsLifecycleTransition) -> bool:
        """Approve a proposed transition without blocking or side effects."""


class EarningsEventLifecycle:
    """Fail-closed PRE/RELEASE/DISCOVERY/RECOVERY integration state machine."""

    def __init__(self, hooks: tuple[EarningsLifecycleHook, ...] = ()) -> None:
        """Create a pre-earnings lifecycle with a bounded hook collection."""
        if len(hooks) > MAX_HOOKS:
            msg = "earnings lifecycle hook count exceeds its bound"
            raise ValueError(msg)
        self._hooks = hooks
        self._phase = EarningsPhase.PRE_EARNINGS
        self._event_id: GlobalEventId | None = None
        self._revision = 0
        self._history: list[EarningsLifecycleTransition] = []

    @property
    def phase(self) -> EarningsPhase:
        """Return the current committed phase."""
        return self._phase

    @property
    def history(self) -> tuple[EarningsLifecycleTransition, ...]:
        """Return immutable bounded transition history."""
        return tuple(self._history)

    def _transition(
        self,
        event_id: GlobalEventId,
        revision: int,
        next_phase: EarningsPhase,
        now_ns: int,
    ) -> None:
        if now_ns <= 0:
            msg = "lifecycle transition time must be positive"
            raise EarningsEvaluationError(msg)
        if len(self._history) >= MAX_TRANSITIONS:
            msg = "earnings lifecycle transition history is full"
            raise EarningsEvaluationError(msg)
        transition = EarningsLifecycleTransition(
            event_id,
            revision,
            self._phase,
            next_phase,
            now_ns,
        )
        if not all(hook.approve_transition(transition) for hook in self._hooks):
            msg = "earnings lifecycle hook rejected the transition"
            raise EarningsEvaluationError(msg)
        self._history.append(transition)
        self._phase = next_phase
        self._event_id = event_id
        self._revision = revision

    def accept_release(self, result: EarningsSpecialistResult) -> None:
        """Enter/re-enter release processing for an original or corrected release."""
        first_release = (
            self._phase is EarningsPhase.PRE_EARNINGS and result.revision == 1
        )
        correction = (
            result.revision > self._revision
            and self._event_id == result.earnings_event_id
            and self._phase
            in {
                EarningsPhase.RELEASE_PROCESSING,
                EarningsPhase.PRICE_DISCOVERY,
                EarningsPhase.RECOVERY,
            }
        )
        if not first_release and not correction:
            msg = "release is stale or invalid for the current earnings phase"
            raise EarningsEvaluationError(msg)
        self._transition(
            result.earnings_event_id,
            result.revision,
            EarningsPhase.RELEASE_PROCESSING,
            result.production_process_monotonic_time_ns,
        )

    def begin_price_discovery(self, result: EarningsSpecialistResult) -> None:
        """Enter price discovery only with fresh post-release market features."""
        if (
            self._phase is not EarningsPhase.RELEASE_PROCESSING
            or result.phase is not EarningsPhase.PRICE_DISCOVERY
            or self._event_id != result.earnings_event_id
            or self._revision != result.revision
        ):
            msg = "price discovery requires matching post-release specialist output"
            raise EarningsEvaluationError(msg)
        self._transition(
            result.earnings_event_id,
            result.revision,
            EarningsPhase.PRICE_DISCOVERY,
            result.production_process_monotonic_time_ns,
        )

    def complete_price_discovery(
        self, result: EarningsSpecialistResult, now_monotonic_time_ns: int
    ) -> bool:
        """Enter recovery after the versioned discovery duration; never sleep."""
        if (
            self._phase is not EarningsPhase.PRICE_DISCOVERY
            or self._event_id != result.earnings_event_id
            or self._revision != result.revision
        ):
            msg = "recovery requires matching active price-discovery output"
            raise EarningsEvaluationError(msg)
        elapsed = now_monotonic_time_ns - result.production_process_monotonic_time_ns
        if elapsed < result.estimated_price_discovery_duration_ns:
            return False
        self._transition(
            result.earnings_event_id,
            result.revision,
            EarningsPhase.RECOVERY,
            now_monotonic_time_ns,
        )
        return True

    def reset(self, now_monotonic_time_ns: int) -> None:
        """Return to PRE_EARNINGS only after recovery has completed."""
        if self._phase is not EarningsPhase.RECOVERY or self._event_id is None:
            msg = "earnings lifecycle reset requires RECOVERY"
            raise EarningsEvaluationError(msg)
        event_id = self._event_id
        revision = self._revision
        self._transition(
            event_id,
            revision,
            EarningsPhase.PRE_EARNINGS,
            now_monotonic_time_ns,
        )
        self._event_id = None
        self._revision = 0
