"""Deterministic, leakage-safe macroeconomic release specialist."""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from enum import IntEnum
from typing import Final

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
from aegis_mx_intelligence.macro_types import (
    MAX_ABSOLUTE_RESPONSE_PPM,
    MAX_INT64,
    MIN_INT64,
    PPM,
    SHA256_BYTES,
    CrossAssetGroup,
    CrossAssetKind,
    CrossAssetResponseFeature,
    CrossAssetUnit,
    ExpectedReleaseField,
    MacroDecision,
    MacroDirectionDistribution,
    MacroEventPhase,
    MacroFieldSurprise,
    MacroInputBundle,
    MacroMetricIdentity,
    MacroMetricRole,
    MacroMetricUnit,
    MacroReasonCode,
    MacroRevisionSurprise,
    MacroSourceEvidence,
    MacroSourceKind,
    MacroSourceQuality,
    MacroSpecialistResult,
    ReleaseCompletenessState,
)
from aegis_mx_intelligence.news_types import derived_identifier


class MacroEvaluationError(ValueError):
    """Fail-closed macro evaluation, replay, or lineage error."""


@dataclass(frozen=True, slots=True)
class MacroSpecialistConfig:
    """Immutable integer configuration for deterministic macro evaluation."""

    model_id: ModelId
    model_version: ModelVersion
    forecast_ttl_ns: int = 2_000_000_000
    forecast_horizon_ns: int = 300_000_000_000
    minimum_history_count: int = 5
    minimum_source_quality_ppm: int = 600_000
    yield_basis_point_scale_ppm: int = 1_000
    epsilon_percent_ppm: int = 1
    epsilon_rate_basis_points: int = 1
    epsilon_index_milli: int = 1
    epsilon_currency_millions: int = 1
    epsilon_quantity_thousands: int = 1
    epsilon_ratio_ppm: int = 1
    required_cross_asset_groups: tuple[CrossAssetGroup, ...] = (
        CrossAssetGroup.EQUITY_INDEX,
        CrossAssetGroup.RATES,
        CrossAssetGroup.FX,
        CrossAssetGroup.VOLATILITY,
        CrossAssetGroup.SECTOR,
    )

    def __post_init__(self) -> None:
        """Validate durations, thresholds, epsilons, and group uniqueness."""
        if self.forecast_ttl_ns <= 0 or self.forecast_horizon_ns <= 0:
            msg = "macro forecast TTL and horizon must be positive"
            raise ValueError(msg)
        if self.minimum_history_count <= 0:
            msg = "macro minimum_history_count must be positive"
            raise ValueError(msg)
        if not 0 <= self.minimum_source_quality_ppm <= PPM:
            msg = "macro minimum source quality is outside [0, 1,000,000] PPM"
            raise ValueError(msg)
        if self.yield_basis_point_scale_ppm <= 0:
            msg = "yield basis-point normalization scale must be positive"
            raise ValueError(msg)
        epsilons = (
            self.epsilon_percent_ppm,
            self.epsilon_rate_basis_points,
            self.epsilon_index_milli,
            self.epsilon_currency_millions,
            self.epsilon_quantity_thousands,
            self.epsilon_ratio_ppm,
        )
        if any(value <= 0 for value in epsilons):
            msg = "macro surprise epsilon values must be positive"
            raise ValueError(msg)
        if not self.required_cross_asset_groups or len(
            set(self.required_cross_asset_groups)
        ) != len(self.required_cross_asset_groups):
            msg = "required macro cross-asset groups must be nonempty and unique"
            raise ValueError(msg)

    def epsilon(self, unit: MacroMetricUnit) -> int:
        """Return the explicit surprise epsilon in the field's own unit."""
        return {
            MacroMetricUnit.PERCENT_PPM: self.epsilon_percent_ppm,
            MacroMetricUnit.RATE_BASIS_POINTS: self.epsilon_rate_basis_points,
            MacroMetricUnit.INDEX_MILLI: self.epsilon_index_milli,
            MacroMetricUnit.CURRENCY_MILLIONS: self.epsilon_currency_millions,
            MacroMetricUnit.QUANTITY_THOUSANDS: self.epsilon_quantity_thousands,
            MacroMetricUnit.RATIO_PPM: self.epsilon_ratio_ppm,
        }[unit]

    def sha256(self) -> str:
        """Return a deterministic, secret-free effective configuration hash."""
        values = (
            self.model_id.hex(),
            self.model_version.hex(),
            self.forecast_ttl_ns,
            self.forecast_horizon_ns,
            self.minimum_history_count,
            self.minimum_source_quality_ppm,
            self.yield_basis_point_scale_ppm,
            self.epsilon_percent_ppm,
            self.epsilon_rate_basis_points,
            self.epsilon_index_milli,
            self.epsilon_currency_millions,
            self.epsilon_quantity_thousands,
            self.epsilon_ratio_ppm,
            *(int(item) for item in self.required_cross_asset_groups),
        )
        encoded = "|".join(str(value) for value in values).encode("ascii")
        return hashlib.sha256(encoded).hexdigest()


def _trunc_div(numerator: int, denominator: int) -> int:
    if denominator <= 0:
        msg = "deterministic macro division denominator must be positive"
        raise MacroEvaluationError(msg)
    sign = -1 if numerator < 0 else 1
    return sign * (abs(numerator) // denominator)


def _scaled_ratio_ppm(numerator: int, denominator: int) -> int:
    result = _trunc_div(numerator * PPM, denominator)
    if not MIN_INT64 <= result <= MAX_INT64:
        msg = "standardized macro ratio exceeds signed 64-bit output"
        raise MacroEvaluationError(msg)
    return result


def _clamp(value: int, lower: int, upper: int) -> int:
    return min(upper, max(lower, value))


def _median(values: list[int]) -> int:
    if not values:
        msg = "macro median requires at least one value"
        raise MacroEvaluationError(msg)
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[middle]
    return _trunc_div(ordered[middle - 1] + ordered[middle], 2)


def _ordered_unique[T: IntEnum](values: list[T]) -> tuple[T, ...]:
    return tuple(sorted(set(values), key=int))


def _same_field_family(left: MacroMetricIdentity, right: MacroMetricIdentity) -> bool:
    return (
        left.event_type is right.event_type
        and left.field_id == right.field_id
        and left.role is right.role
        and left.reference_period == right.reference_period
        and left.seasonal_adjustment is right.seasonal_adjustment
        and left.annualization is right.annualization
    )


def _cross_asset_group(kind: CrossAssetKind) -> CrossAssetGroup:
    return {
        CrossAssetKind.EQUITY_INDEX_FUTURE: CrossAssetGroup.EQUITY_INDEX,
        CrossAssetKind.TREASURY_FUTURE: CrossAssetGroup.RATES,
        CrossAssetKind.TREASURY_YIELD: CrossAssetGroup.RATES,
        CrossAssetKind.FX_PROXY: CrossAssetGroup.FX,
        CrossAssetKind.VOLATILITY_INSTRUMENT: CrossAssetGroup.VOLATILITY,
        CrossAssetKind.SECTOR_ETF: CrossAssetGroup.SECTOR,
    }[kind]


def _source_quality_score(source: MacroSourceEvidence) -> int:
    return {
        MacroSourceQuality.UNVERIFIED: 0,
        MacroSourceQuality.PUBLIC_SECONDARY: 500_000,
        MacroSourceQuality.LICENSED_AGGREGATOR: 800_000,
        MacroSourceQuality.OFFICIAL_PRIMARY: PPM,
        MacroSourceQuality.SYNTHETIC_REPLAY: 900_000,
    }[source.quality]


@dataclass(frozen=True, slots=True)
class _MacroAnalysis:
    phase: MacroEventPhase
    headline: MacroFieldSurprise
    core: MacroFieldSurprise | None
    subcomponents: tuple[MacroFieldSurprise, ...]
    revisions: tuple[MacroRevisionSurprise, ...]
    percentile: int | None
    features: tuple[CrossAssetResponseFeature, ...]
    source_quality: int


class MacroeconomicReleaseSpecialist:
    """Pure off-hot-path specialist with strict abstention semantics."""

    _CRITICAL_REASONS: Final = frozenset(
        {
            MacroReasonCode.NO_RELEASE,
            MacroReasonCode.PARTIAL_RELEASE,
            MacroReasonCode.MISSING_REQUIRED_FIELD,
            MacroReasonCode.MISSING_CONSENSUS,
            MacroReasonCode.INCOMPATIBLE_FIELD,
            MacroReasonCode.CONFLICTING_PROVIDER_VALUES,
            MacroReasonCode.LOW_SOURCE_QUALITY,
            MacroReasonCode.UNAUTHENTICATED_SOURCE,
            MacroReasonCode.MISSING_CROSS_ASSET_GROUP,
            MacroReasonCode.INSUFFICIENT_HISTORY,
            MacroReasonCode.NO_COMPARABLE_SURPRISE,
            MacroReasonCode.INVALID_MARKET_FEATURES,
        }
    )

    def __init__(self, config: MacroSpecialistConfig) -> None:
        """Bind one immutable model and configuration version."""
        self._config = config

    @property
    def configuration_sha256(self) -> str:
        """Expose the immutable effective configuration hash."""
        return self._config.sha256()

    @staticmethod
    def _phase(bundle: MacroInputBundle) -> MacroEventPhase:
        if bundle.release is None:
            if bundle.as_of_wall_clock_utc_ns <= (
                bundle.calendar.scheduled_release_wall_clock_utc_ns
            ):
                return MacroEventPhase.SCHEDULED
            return MacroEventPhase.DELAYED
        if bundle.release.completeness is ReleaseCompletenessState.PARTIAL:
            return MacroEventPhase.PARTIAL_RELEASE
        if bundle.release.revision > 1:
            return MacroEventPhase.CORRECTED
        return MacroEventPhase.RELEASE_COMPLETE

    def _field_surprise(
        self,
        bundle: MacroInputBundle,
        field: ExpectedReleaseField,
        source_index: dict[str, MacroSourceEvidence],
    ) -> MacroFieldSurprise:
        if bundle.release is None:
            return MacroFieldSurprise(
                field.identity,
                None,
                None,
                None,
                None,
                MacroReasonCode.NO_RELEASE,
            )
        official = [
            item
            for item in bundle.release.observations
            if item.identity == field.identity
            and source_index[item.source_id].kind is MacroSourceKind.OFFICIAL_RELEASE
        ]
        if not official:
            incompatible = any(
                _same_field_family(field.identity, item.identity)
                and source_index[item.source_id].kind
                is MacroSourceKind.OFFICIAL_RELEASE
                for item in bundle.release.observations
            )
            return MacroFieldSurprise(
                field.identity,
                None,
                None,
                None,
                None,
                MacroReasonCode.INCOMPATIBLE_FIELD
                if incompatible
                else (
                    MacroReasonCode.MISSING_REQUIRED_FIELD
                    if field.required
                    else MacroReasonCode.FIELD_NOT_RELEASED
                ),
            )
        actual = official[0].value
        matching_values = {
            item.value
            for item in bundle.release.observations
            if item.identity == field.identity
        }
        if len(matching_values) > 1:
            return MacroFieldSurprise(
                field.identity,
                actual,
                None,
                None,
                None,
                MacroReasonCode.CONFLICTING_PROVIDER_VALUES,
            )
        estimate = next(
            (
                item
                for item in bundle.consensus_snapshot.estimates
                if item.identity == field.identity
            ),
            None,
        )
        if estimate is None:
            incompatible = any(
                _same_field_family(field.identity, item.identity)
                for item in bundle.consensus_snapshot.estimates
            )
            return MacroFieldSurprise(
                field.identity,
                actual,
                None,
                None,
                None,
                MacroReasonCode.INCOMPATIBLE_FIELD
                if incompatible
                else MacroReasonCode.MISSING_CONSENSUS,
            )
        denominator = max(
            estimate.dispersion, self._config.epsilon(field.identity.unit)
        )
        return MacroFieldSurprise(
            field.identity,
            actual,
            estimate.consensus_value,
            estimate.dispersion,
            _scaled_ratio_ppm(actual - estimate.consensus_value, denominator),
            None,
        )

    def _surprises(
        self,
        bundle: MacroInputBundle,
        source_index: dict[str, MacroSourceEvidence],
    ) -> tuple[
        MacroFieldSurprise,
        MacroFieldSurprise | None,
        tuple[MacroFieldSurprise, ...],
    ]:
        values = tuple(
            self._field_surprise(bundle, field, source_index)
            for field in bundle.calendar.expected_fields
        )
        headline = next(
            item for item in values if item.identity.role is MacroMetricRole.HEADLINE
        )
        core = next(
            (item for item in values if item.identity.role is MacroMetricRole.CORE),
            None,
        )
        subcomponents = tuple(
            item
            for item in values
            if item.identity.role is MacroMetricRole.SUBCOMPONENT
        )
        return headline, core, subcomponents

    def _revision_surprises(
        self, bundle: MacroInputBundle
    ) -> tuple[MacroRevisionSurprise, ...]:
        if bundle.release is None:
            return ()
        output = []
        for prior in bundle.release.prior_values:
            if prior.revised_value_in_release is None:
                continue
            delta = prior.revised_value_in_release - prior.value_known_before_release
            output.append(
                MacroRevisionSurprise(
                    prior.identity,
                    prior.value_known_before_release,
                    prior.revised_value_in_release,
                    delta,
                    _scaled_ratio_ppm(
                        delta,
                        max(
                            abs(prior.value_known_before_release),
                            self._config.epsilon(prior.identity.unit),
                        ),
                    ),
                )
            )
        return tuple(output)

    def _surprise_percentile(
        self,
        bundle: MacroInputBundle,
        headline: MacroFieldSurprise,
    ) -> int | None:
        if (
            headline.surprise_ppm is None
            or len(bundle.historical_surprises) < self._config.minimum_history_count
        ):
            return None
        less_or_equal = sum(
            item.headline_surprise_ppm <= headline.surprise_ppm
            for item in bundle.historical_surprises
        )
        return _trunc_div(less_or_equal * PPM, len(bundle.historical_surprises))

    def _cross_asset_features(
        self, bundle: MacroInputBundle
    ) -> tuple[CrossAssetResponseFeature, ...]:
        if bundle.market_snapshot is None:
            return ()
        output = []
        for item in bundle.market_snapshot.observations:
            magnitude = (
                abs(item.response_value)
                if item.unit is CrossAssetUnit.RETURN_PPM
                else abs(item.response_value) * self._config.yield_basis_point_scale_ppm
            )
            output.append(
                CrossAssetResponseFeature(
                    item.instrument_id,
                    item.kind,
                    _cross_asset_group(item.kind),
                    item.unit,
                    item.response_value,
                    min(MAX_ABSOLUTE_RESPONSE_PPM, magnitude),
                )
            )
        return tuple(
            sorted(
                output,
                key=lambda item: (
                    int(item.group),
                    int(item.kind),
                    item.instrument_id.high,
                    item.instrument_id.low,
                ),
            )
        )

    def _source_quality(self, bundle: MacroInputBundle) -> int:
        return min(_source_quality_score(item) for item in bundle.sources)

    @staticmethod
    def _phase_reasons(
        bundle: MacroInputBundle, phase: MacroEventPhase
    ) -> list[MacroReasonCode]:
        reasons: list[MacroReasonCode] = []
        if phase is MacroEventPhase.SCHEDULED:
            reasons.append(MacroReasonCode.NO_RELEASE)
        elif phase is MacroEventPhase.DELAYED:
            reasons.extend(
                [MacroReasonCode.NO_RELEASE, MacroReasonCode.DELAYED_RELEASE]
            )
        elif phase is MacroEventPhase.PARTIAL_RELEASE:
            reasons.append(MacroReasonCode.PARTIAL_RELEASE)
        elif phase is MacroEventPhase.CORRECTED:
            reasons.append(MacroReasonCode.CORRECTED_RELEASE)
        if bundle.release is not None and (
            bundle.release.official_publication_wall_clock_utc_ns
            > bundle.calendar.scheduled_release_wall_clock_utc_ns
        ):
            reasons.append(MacroReasonCode.DELAYED_RELEASE)
        return reasons

    def _quality_reasons(
        self, bundle: MacroInputBundle, analysis: _MacroAnalysis
    ) -> list[MacroReasonCode]:
        reasons: list[MacroReasonCode] = []
        if bundle.release is not None and analysis.percentile is None:
            reasons.append(MacroReasonCode.INSUFFICIENT_HISTORY)
        if analysis.source_quality < self._config.minimum_source_quality_ppm:
            reasons.append(MacroReasonCode.LOW_SOURCE_QUALITY)
        if any(not item.authenticated for item in bundle.sources):
            reasons.append(MacroReasonCode.UNAUTHENTICATED_SOURCE)
        if bundle.release is not None:
            if bundle.market_snapshot is None:
                reasons.append(MacroReasonCode.MISSING_CROSS_ASSET_GROUP)
            elif not bundle.market_snapshot.valid:
                reasons.append(MacroReasonCode.INVALID_MARKET_FEATURES)
            available_groups = {item.group for item in analysis.features}
            if any(
                item not in available_groups
                for item in self._config.required_cross_asset_groups
            ):
                reasons.append(MacroReasonCode.MISSING_CROSS_ASSET_GROUP)
        return reasons

    @staticmethod
    def _surprise_reasons(
        bundle: MacroInputBundle, analysis: _MacroAnalysis
    ) -> list[MacroReasonCode]:
        surprises = (
            analysis.headline,
            *((analysis.core,) if analysis.core is not None else ()),
            *analysis.subcomponents,
        )
        reasons = [item.reason for item in surprises if item.reason is not None]
        if bundle.release is not None and not any(
            item.surprise_ppm is not None for item in surprises
        ):
            reasons.append(MacroReasonCode.NO_COMPARABLE_SURPRISE)
        return reasons

    def _reasons(
        self, bundle: MacroInputBundle, analysis: _MacroAnalysis
    ) -> tuple[MacroReasonCode, ...]:
        reasons = self._phase_reasons(bundle, analysis.phase)
        reasons.extend(self._surprise_reasons(bundle, analysis))
        reasons.extend(self._quality_reasons(bundle, analysis))
        return _ordered_unique(reasons)

    @staticmethod
    def _direction_distribution(signal_ppm: int) -> MacroDirectionDistribution:
        flat = 300_000
        directional_mass = PPM - flat
        edge = _trunc_div(
            _clamp(signal_ppm, -PPM, PPM) * (directional_mass // 2),
            PPM,
        )
        up = (directional_mass // 2) + edge
        down = directional_mass - up
        return MacroDirectionDistribution(down, flat, up)

    def _surprise_signal(
        self,
        bundle: MacroInputBundle,
        surprises: tuple[MacroFieldSurprise, ...],
    ) -> int:
        surprise_index = {item.identity: item for item in surprises}
        numerator = 0
        denominator = 0
        for field in bundle.calendar.expected_fields:
            surprise = surprise_index[field.identity].surprise_ppm
            if surprise is None or field.forecast_weight_ppm == 0:
                continue
            numerator += (
                _clamp(surprise, -PPM, PPM)
                * field.forecast_weight_ppm
                * field.positive_surprise_direction
            )
            denominator += field.forecast_weight_ppm
        if denominator <= 0:
            msg = "published macro result has no weighted surprise signal"
            raise MacroEvaluationError(msg)
        return _trunc_div(numerator, denominator)

    @staticmethod
    def _target_response(bundle: MacroInputBundle) -> int:
        if bundle.market_snapshot is None:
            return 0
        return next(
            (
                item.response_value
                for item in bundle.market_snapshot.observations
                if item.instrument_id == bundle.target_instrument_id
                and item.unit is CrossAssetUnit.RETURN_PPM
            ),
            0,
        )

    def _publish_values(
        self,
        bundle: MacroInputBundle,
        analysis: _MacroAnalysis,
    ) -> tuple[
        ForecastId,
        int,
        int,
        MacroDirectionDistribution,
        int,
        bytes,
    ]:
        if bundle.market_snapshot is None:
            msg = "published macro result requires market feature provenance"
            raise MacroEvaluationError(msg)
        expiration = (
            bundle.production_process_monotonic_time_ns + self._config.forecast_ttl_ns
        )
        if expiration > (1 << 64) - 1:
            msg = "macro forecast expiration overflows uint64"
            raise MacroEvaluationError(msg)
        surprises = (
            analysis.headline,
            *((analysis.core,) if analysis.core is not None else ()),
            *analysis.subcomponents,
        )
        surprise_signal = self._surprise_signal(bundle, surprises)
        target_response = _clamp(self._target_response(bundle), -PPM, PPM)
        expected_return = _clamp(
            _trunc_div((surprise_signal * 3) + target_response, 8),
            -MAX_ABSOLUTE_RESPONSE_PPM,
            MAX_ABSOLUTE_RESPONSE_PPM,
        )
        expected_volatility = _median(
            [item.normalized_magnitude_ppm for item in analysis.features]
        )
        expected_volatility = max(1, expected_volatility)
        direction = self._direction_distribution(expected_return * 2)
        forecast_id = ForecastId(
            derived_identifier(
                "macro-forecast",
                bundle.sha256,
                self._config.model_version.hex(),
            )
        )
        record_id = GlobalEventId(
            derived_identifier("macro-forecast-record", forecast_id.hex())
        )
        confidence = min(750_000, analysis.source_quality)
        if analysis.percentile is None:
            msg = "published macro result requires a surprise percentile"
            raise MacroEvaluationError(msg)
        ood_score = min(PPM, abs(analysis.percentile - (PPM // 2)) * 2)
        lower = _clamp(
            expected_return - expected_volatility,
            -MAX_ABSOLUTE_RESPONSE_PPM,
            MAX_ABSOLUTE_RESPONSE_PPM,
        )
        upper = _clamp(
            expected_return + expected_volatility,
            -MAX_ABSOLUTE_RESPONSE_PPM,
            MAX_ABSOLUTE_RESPONSE_PPM,
        )
        forecast = ModelForecastContractInput(
            record_id=record_id,
            forecast_id=forecast_id,
            session_id=bundle.session_id,
            model_id=self._config.model_id,
            model_version=self._config.model_version,
            instrument_id=bundle.target_instrument_id,
            feature_snapshot_id=bundle.market_snapshot.feature_snapshot_id,
            configuration_version=bundle.configuration_version,
            expected_return_ppm=expected_return,
            return_p10_ppm=lower,
            return_p50_ppm=expected_return,
            return_p90_ppm=upper,
            probability_down_ppm=direction.down_ppm,
            probability_flat_ppm=direction.flat_ppm,
            probability_up_ppm=direction.up_ppm,
            volatility_ppm=expected_volatility,
            confidence_ppm=confidence,
            calibration_score_ppm=0,
            data_quality_score_ppm=analysis.source_quality,
            ood_score_ppm=ood_score,
            horizon_ns=self._config.forecast_horizon_ns,
            as_of_exchange_event_time_ns=(
                bundle.market_snapshot.as_of_exchange_event_time_ns
            ),
            production_process_monotonic_time_ns=(
                bundle.production_process_monotonic_time_ns
            ),
            expiration_process_monotonic_time_ns=expiration,
            forecast_target=ForecastTargetCode.RETURN,
            forecast_unit=ForecastUnitCode.RETURN_PPM,
            target_value=expected_return,
            target_p10=lower,
            target_p50=expected_return,
            target_p90=upper,
        )
        return (
            forecast_id,
            expected_return,
            expected_volatility,
            direction,
            expiration,
            build_model_forecast_contract(forecast),
        )

    def evaluate(self, bundle: MacroInputBundle) -> MacroSpecialistResult:
        """Evaluate a point-in-time bundle and publish or strictly abstain."""
        phase = self._phase(bundle)
        source_index = {item.source_id: item for item in bundle.sources}
        headline, core, subcomponents = self._surprises(bundle, source_index)
        analysis = _MacroAnalysis(
            phase,
            headline,
            core,
            subcomponents,
            self._revision_surprises(bundle),
            self._surprise_percentile(bundle, headline),
            self._cross_asset_features(bundle),
            self._source_quality(bundle),
        )
        reasons = self._reasons(bundle, analysis)
        publish = not any(item in self._CRITICAL_REASONS for item in reasons)
        forecast_values: (
            tuple[
                ForecastId,
                int,
                int,
                MacroDirectionDistribution,
                int,
                bytes,
            ]
            | None
        ) = None
        if publish:
            forecast_values = self._publish_values(bundle, analysis)
        release = bundle.release
        delay = (
            None
            if release is None
            else release.official_publication_wall_clock_utc_ns
            - bundle.calendar.scheduled_release_wall_clock_utc_ns
        )
        return MacroSpecialistResult(
            macro_event_id=bundle.calendar.calendar_event_id,
            model_id=self._config.model_id,
            model_version=self._config.model_version,
            input_sha256=bundle.sha256,
            consensus_snapshot_sha256=bundle.consensus_snapshot.sha256,
            release_sha256=None if release is None else release.sha256,
            revision=0 if release is None else release.revision,
            phase=phase,
            decision=MacroDecision.PUBLISH if publish else MacroDecision.ABSTAIN,
            scheduled_release_wall_clock_utc_ns=(
                bundle.calendar.scheduled_release_wall_clock_utc_ns
            ),
            official_publication_wall_clock_utc_ns=(
                None
                if release is None
                else release.official_publication_wall_clock_utc_ns
            ),
            receipt_wall_clock_utc_ns=(
                None if release is None else release.receipt_wall_clock_utc_ns
            ),
            release_delay_ns=delay,
            headline_surprise=analysis.headline,
            core_surprise=analysis.core,
            subcomponent_surprises=analysis.subcomponents,
            revision_surprises=analysis.revisions,
            surprise_percentile_ppm=analysis.percentile,
            cross_asset_response_features=analysis.features,
            source_quality_score_ppm=analysis.source_quality,
            reason_codes=reasons,
            forecast_id=None if forecast_values is None else forecast_values[0],
            expected_return_ppm=(
                None if forecast_values is None else forecast_values[1]
            ),
            expected_volatility_ppm=(
                None if forecast_values is None else forecast_values[2]
            ),
            direction=None if forecast_values is None else forecast_values[3],
            production_process_monotonic_time_ns=(
                bundle.production_process_monotonic_time_ns
            ),
            valid_until_process_monotonic_time_ns=(
                None if forecast_values is None else forecast_values[4]
            ),
            forecast_contract_bytes=(
                None if forecast_values is None else forecast_values[5]
            ),
        )

    def verify_replay(
        self,
        bundle: MacroInputBundle,
        expected_result_sha256: bytes,
    ) -> MacroSpecialistResult:
        """Re-evaluate and require byte-stable detailed output."""
        if len(expected_result_sha256) != SHA256_BYTES:
            msg = "expected macro replay digest must contain 32 bytes"
            raise MacroEvaluationError(msg)
        result = self.evaluate(bundle)
        if result.sha256 != expected_result_sha256:
            msg = "macro replay output digest mismatch"
            raise MacroEvaluationError(msg)
        return result


class FrozenMacroEventProcessor:
    """Single-event processor enforcing consensus and correction lineage."""

    def __init__(
        self,
        specialist: MacroeconomicReleaseSpecialist,
        initial_bundle: MacroInputBundle,
    ) -> None:
        """Freeze the calendar event and exact pre-release consensus digest."""
        self._specialist = specialist
        self._event_id = initial_bundle.calendar.calendar_event_id
        self._consensus_sha256 = initial_bundle.consensus_snapshot.sha256
        self._latest_revision = 0
        self._latest_release_sha256: bytes | None = None

    @property
    def consensus_snapshot_sha256(self) -> bytes:
        """Return the frozen pre-release consensus identity."""
        return self._consensus_sha256

    def process(self, bundle: MacroInputBundle) -> MacroSpecialistResult:
        """Reject consensus mutation, stale revisions, and broken correction links."""
        if bundle.calendar.calendar_event_id != self._event_id:
            msg = "macro processor received a different calendar event"
            raise MacroEvaluationError(msg)
        if bundle.consensus_snapshot.sha256 != self._consensus_sha256:
            msg = "frozen macro consensus changed after processor initialization"
            raise MacroEvaluationError(msg)
        release = bundle.release
        if release is not None:
            if release.revision <= self._latest_revision:
                msg = "macro processor received a stale release revision"
                raise MacroEvaluationError(msg)
            if (
                release.revision > 1
                and release.correction_of_release_sha256 != self._latest_release_sha256
            ):
                msg = "macro correction does not link to the accepted release"
                raise MacroEvaluationError(msg)
        result = self._specialist.evaluate(bundle)
        if release is not None:
            self._latest_revision = release.revision
            self._latest_release_sha256 = release.sha256
        return result
