"""Replaceable TimesFM boundary and deterministic comparator adapters."""

from __future__ import annotations

import math
from abc import ABC, abstractmethod
from dataclasses import dataclass, replace
from itertools import pairwise
from typing import TYPE_CHECKING, Final, Protocol

from aegis_mx_timeseries.domain import (
    PPM,
    AdapterDevice,
    AdapterDeviceError,
    AdapterUnavailableError,
    BaselineKind,
    ForecastContext,
    ForecastTarget,
    target_bounds,
    validate_checkpoint_sha256,
)

if TYPE_CHECKING:
    from aegis_mx_intelligence import ModelId, ModelVersion

MIN_BASELINE_HISTORY: Final = 4
MIN_SEASONAL_PERIOD: Final = 2
MAX_SEASONAL_PERIOD: Final = 256


def _trunc_div(numerator: int, denominator: int) -> int:
    if denominator <= 0:
        msg = "denominator must be positive"
        raise ValueError(msg)
    magnitude = abs(numerator) // denominator
    return -magnitude if numerator < 0 else magnitude


def _clamp_target(target: ForecastTarget, value: int) -> int:
    minimum, maximum = target_bounds(target)
    return max(minimum, min(maximum, value))


def _recent_differences(context: ForecastContext) -> tuple[int, ...]:
    values = tuple(point.value for point in context.history[-9:])
    return tuple(following - current for current, following in pairwise(values))


def _residual_scale(context: ForecastContext) -> int:
    differences = _recent_differences(context)
    if not differences:
        return 1
    return max(1, sum(abs(value) for value in differences) // len(differences))


def _quantiles(
    context: ForecastContext, point: int, horizon_index: int
) -> tuple[int, int, int]:
    width = _residual_scale(context) * max(1, horizon_index + 1)
    return (
        _clamp_target(context.target, point - width),
        point,
        _clamp_target(context.target, point + width),
    )


@dataclass(frozen=True, slots=True)
class AdapterForecast:
    """One horizon's integer point and quantile forecast."""

    horizon_ns: int
    point_value: int
    quantile_values: tuple[int, int, int]
    confidence_ppm: int
    calibration_score_ppm: int
    data_quality_score_ppm: int
    ood_score_ppm: int


@dataclass(frozen=True, slots=True)
class AdapterResult:
    """One context's complete model result and producing identity."""

    context_sha256: bytes
    target: ForecastTarget
    forecasts: tuple[AdapterForecast, ...]
    producer_name: str
    model_id: ModelId
    model_version: ModelVersion
    device: AdapterDevice
    checkpoint_sha256: str | None

    def validate_for(self, context: ForecastContext) -> None:
        """Fail closed on malformed, detached, or unit-invalid adapter output."""
        validate_checkpoint_sha256(self.checkpoint_sha256)
        if (
            self.context_sha256 != context.context_sha256
            or self.target is not context.target
        ):
            msg = "adapter output is detached from its forecast context"
            raise ValueError(msg)
        if not self.producer_name or len(self.forecasts) != len(context.horizons_ns):
            msg = "adapter output does not cover every requested horizon"
            raise ValueError(msg)
        minimum, maximum = target_bounds(context.target)
        for expected_horizon, forecast in zip(
            context.horizons_ns, self.forecasts, strict=True
        ):
            if forecast.horizon_ns != expected_horizon:
                msg = "adapter output horizon does not match the context"
                raise ValueError(msg)
            if not (
                forecast.quantile_values[0]
                <= forecast.quantile_values[1]
                <= forecast.quantile_values[2]
            ):
                msg = "adapter quantiles must be monotonic"
                raise ValueError(msg)
            values = (forecast.point_value, *forecast.quantile_values)
            if any(not minimum <= value <= maximum for value in values):
                msg = "adapter forecast is outside the target range"
                raise ValueError(msg)
            scores = (
                forecast.confidence_ppm,
                forecast.calibration_score_ppm,
                forecast.data_quality_score_ppm,
                forecast.ood_score_ppm,
            )
            if any(not 0 <= score <= PPM for score in scores):
                msg = "adapter score is outside PPM"
                raise ValueError(msg)


class ForecastModelAdapter(ABC):
    """Replaceable, bounded, off-hot-path time-series model interface."""

    def __init__(self, model_id: ModelId, model_version: ModelVersion) -> None:
        """Bind an immutable model identity to the adapter instance."""
        self._model_id = model_id
        self._model_version = model_version

    @property
    def model_id(self) -> ModelId:
        """Return the immutable model identity."""
        return self._model_id

    @property
    def model_version(self) -> ModelVersion:
        """Return the immutable model version."""
        return self._model_version

    @property
    @abstractmethod
    def adapter_name(self) -> str:
        """Return a bounded stable name used only for provenance and metrics."""

    @abstractmethod
    def forecast_batch(
        self,
        contexts: tuple[ForecastContext, ...],
        device: AdapterDevice,
        checkpoint_sha256: str | None,
    ) -> tuple[AdapterResult, ...]:
        """Forecast a bounded batch in input order or raise a typed failure."""


class TimesFmBackend(Protocol):
    """Injection protocol implemented by an approved TimesFM runtime package."""

    def forecast_batch(
        self,
        contexts: tuple[ForecastContext, ...],
        device: AdapterDevice,
        checkpoint_sha256: str | None,
    ) -> tuple[AdapterResult, ...]:
        """Return one result per context without changing input order."""


class TimesFmAdapter(ForecastModelAdapter):
    """Identity-enforcing wrapper around a replaceable TimesFM-compatible backend."""

    def __init__(
        self,
        backend: TimesFmBackend,
        model_id: ModelId,
        model_version: ModelVersion,
    ) -> None:
        """Wrap one injected backend with the supplied canonical identity."""
        super().__init__(model_id, model_version)
        self._backend = backend

    @property
    def adapter_name(self) -> str:
        """Return the stable external-adapter name."""
        return "timesfm"

    def forecast_batch(
        self,
        contexts: tuple[ForecastContext, ...],
        device: AdapterDevice,
        checkpoint_sha256: str | None,
    ) -> tuple[AdapterResult, ...]:
        """Run the backend and overwrite all identity fields authoritatively."""
        validate_checkpoint_sha256(checkpoint_sha256)
        results = self._backend.forecast_batch(contexts, device, checkpoint_sha256)
        if len(results) != len(contexts):
            msg = "TimesFM backend result count does not match the batch"
            raise AdapterUnavailableError(msg)
        normalized = tuple(
            replace(
                result,
                producer_name=self.adapter_name,
                model_id=self.model_id,
                model_version=self.model_version,
                device=device,
                checkpoint_sha256=checkpoint_sha256,
            )
            for result in results
        )
        for context, result in zip(contexts, normalized, strict=True):
            result.validate_for(context)
        return normalized


class ReferenceTimesFmAdapter(ForecastModelAdapter):
    """Deterministic infrastructure adapter; not an external TimesFM model."""

    @property
    def adapter_name(self) -> str:
        """Return the explicit non-TimesFM reference name."""
        return "timesfm_reference"

    def forecast_batch(
        self,
        contexts: tuple[ForecastContext, ...],
        device: AdapterDevice,
        checkpoint_sha256: str | None,
    ) -> tuple[AdapterResult, ...]:
        """Produce deterministic infrastructure-only forecasts."""
        validate_checkpoint_sha256(checkpoint_sha256)
        results = []
        for context in contexts:
            differences = _recent_differences(context)
            trend = _trunc_div(sum(differences), len(differences))
            forecasts = []
            for index, (horizon, covariate) in enumerate(
                zip(context.horizons_ns, context.future_covariates, strict=True)
            ):
                session_adjustment = {
                    1: -trend // 4,
                    2: 0,
                    3: -trend,
                    4: trend // 2,
                    5: -trend // 2,
                }[int(covariate.session_state)]
                event_adjustment = covariate.scheduled_event_flags.bit_count() * max(
                    1, _residual_scale(context) // 10
                )
                point = (
                    context.history[-1].value
                    + (trend * (index + 1))
                    + session_adjustment
                    + event_adjustment
                )
                point = _clamp_target(context.target, point)
                forecasts.append(
                    AdapterForecast(
                        horizon_ns=horizon,
                        point_value=point,
                        quantile_values=_quantiles(context, point, index),
                        confidence_ppm=max(100_000, 800_000 - (index * 50_000)),
                        calibration_score_ppm=750_000,
                        data_quality_score_ppm=1_000_000,
                        ood_score_ppm=0,
                    )
                )
            result = AdapterResult(
                context_sha256=context.context_sha256,
                target=context.target,
                forecasts=tuple(forecasts),
                producer_name=self.adapter_name,
                model_id=self.model_id,
                model_version=self.model_version,
                device=device,
                checkpoint_sha256=checkpoint_sha256,
            )
            result.validate_for(context)
            results.append(result)
        return tuple(results)


_BASELINE_NAMES = {
    BaselineKind.LAST_VALUE: "last_value",
    BaselineKind.SEASONAL_NAIVE: "seasonal_naive",
    BaselineKind.ARIMA_COMPATIBLE: "arima_compatible",
    BaselineKind.GARCH_COMPATIBLE: "garch_compatible",
    BaselineKind.GRADIENT_BOOSTING: "gradient_boosting",
    BaselineKind.COMPACT_TEMPORAL: "compact_temporal",
}


def baseline_point(
    kind: BaselineKind,
    target: ForecastTarget,
    values: tuple[int, ...],
    seasonal_period: int,
) -> int:
    """Evaluate one deterministic interpretable baseline point."""
    if len(values) < max(MIN_BASELINE_HISTORY, seasonal_period):
        msg = "baseline has insufficient history"
        raise AdapterUnavailableError(msg)
    if kind is BaselineKind.LAST_VALUE:
        point = values[-1]
    elif kind is BaselineKind.SEASONAL_NAIVE:
        point = values[-seasonal_period]
    elif kind is BaselineKind.ARIMA_COMPATIBLE:
        differences = tuple(
            following - current for current, following in pairwise(values[-9:])
        )
        point = values[-1] + _trunc_div(sum(differences), len(differences))
    elif kind is BaselineKind.GARCH_COMPATIBLE:
        differences = tuple(
            following - current for current, following in pairwise(values[-9:])
        )
        variance = sum(value * value for value in differences) // len(differences)
        point = math.isqrt(variance)
    elif kind is BaselineKind.GRADIENT_BOOSTING:
        recent_change = values[-1] - values[-2]
        long_change = values[-1] - values[-4]
        point = values[-1]
        if recent_change > 0:
            point += abs(recent_change) // 2
        if long_change < 0:
            point -= abs(long_change) // 3
    elif kind is BaselineKind.COMPACT_TEMPORAL:
        point = _trunc_div(
            values[-4] + (2 * values[-3]) + (3 * values[-2]) + (4 * values[-1]),
            10,
        )
    else:
        msg = "unknown baseline kind"
        raise ValueError(msg)
    return _clamp_target(target, point)


class BaselineAdapter(ForecastModelAdapter):
    """One deterministic comparator behind the common adapter interface."""

    def __init__(
        self,
        kind: BaselineKind,
        model_id: ModelId,
        model_version: ModelVersion,
        *,
        seasonal_period: int = 8,
    ) -> None:
        """Bind one baseline and validate its fixed seasonal period."""
        if not MIN_SEASONAL_PERIOD <= seasonal_period <= MAX_SEASONAL_PERIOD:
            msg = "seasonal period is outside the supported bound"
            raise ValueError(msg)
        super().__init__(model_id, model_version)
        self._kind = kind
        self._seasonal_period = seasonal_period

    @property
    def adapter_name(self) -> str:
        """Return the stable comparator name."""
        return _BASELINE_NAMES[self._kind]

    def forecast_batch(
        self,
        contexts: tuple[ForecastContext, ...],
        device: AdapterDevice,
        checkpoint_sha256: str | None,
    ) -> tuple[AdapterResult, ...]:
        """Forecast every context using deterministic integer arithmetic."""
        if device is not AdapterDevice.CPU:
            msg = "baseline adapters support CPU only"
            raise AdapterDeviceError(msg)
        if checkpoint_sha256 is not None:
            msg = "baseline adapters do not accept checkpoints"
            raise AdapterUnavailableError(msg)
        results = []
        for context in contexts:
            values = tuple(point.value for point in context.history)
            point = baseline_point(
                self._kind, context.target, values, self._seasonal_period
            )
            forecasts = tuple(
                AdapterForecast(
                    horizon_ns=horizon,
                    point_value=point,
                    quantile_values=_quantiles(context, point, index),
                    confidence_ppm=500_000,
                    calibration_score_ppm=500_000,
                    data_quality_score_ppm=1_000_000,
                    ood_score_ppm=0,
                )
                for index, horizon in enumerate(context.horizons_ns)
            )
            result = AdapterResult(
                context_sha256=context.context_sha256,
                target=context.target,
                forecasts=forecasts,
                producer_name=self.adapter_name,
                model_id=self.model_id,
                model_version=self.model_version,
                device=AdapterDevice.CPU,
                checkpoint_sha256=None,
            )
            result.validate_for(context)
            results.append(result)
        return tuple(results)
