"""Deterministic point-in-time walk-forward comparator evaluation."""

from __future__ import annotations

import hashlib
import json
import math
from dataclasses import dataclass
from typing import Final

from aegis_mx_timeseries.adapters import baseline_point
from aegis_mx_timeseries.domain import BaselineKind, ForecastTarget, TimeSeriesPoint

MINIMUM_EVALUATION_HISTORY: Final = 4
MINIMUM_SEASONAL_PERIOD: Final = 2
MAXIMUM_SEASONAL_PERIOD: Final = 256
REQUIRED_EVALUATION_TARGETS: Final = (
    ForecastTarget.RETURN,
    ForecastTarget.REALIZED_VOLATILITY,
    ForecastTarget.VOLUME,
    ForecastTarget.SPREAD,
)

_BASELINES_BY_TARGET: Final = {
    ForecastTarget.RETURN: (
        BaselineKind.LAST_VALUE,
        BaselineKind.SEASONAL_NAIVE,
        BaselineKind.ARIMA_COMPATIBLE,
        BaselineKind.GRADIENT_BOOSTING,
        BaselineKind.COMPACT_TEMPORAL,
    ),
    ForecastTarget.REALIZED_VOLATILITY: (
        BaselineKind.LAST_VALUE,
        BaselineKind.SEASONAL_NAIVE,
        BaselineKind.GARCH_COMPATIBLE,
        BaselineKind.GRADIENT_BOOSTING,
        BaselineKind.COMPACT_TEMPORAL,
    ),
    ForecastTarget.VOLUME: (
        BaselineKind.LAST_VALUE,
        BaselineKind.SEASONAL_NAIVE,
        BaselineKind.ARIMA_COMPATIBLE,
        BaselineKind.GRADIENT_BOOSTING,
        BaselineKind.COMPACT_TEMPORAL,
    ),
    ForecastTarget.SPREAD: (
        BaselineKind.LAST_VALUE,
        BaselineKind.SEASONAL_NAIVE,
        BaselineKind.ARIMA_COMPATIBLE,
        BaselineKind.GRADIENT_BOOSTING,
        BaselineKind.COMPACT_TEMPORAL,
    ),
}


@dataclass(frozen=True, slots=True)
class EvaluationSeries:
    """One target's event-ordered point-in-time evaluation series."""

    target: ForecastTarget
    points: tuple[TimeSeriesPoint, ...]
    seasonal_period: int


@dataclass(frozen=True, slots=True)
class BaselineMetric:
    """Integer error metrics for one target and comparator."""

    baseline: BaselineKind
    mean_absolute_error: int
    root_mean_squared_error: int


@dataclass(frozen=True, slots=True)
class TargetEvaluation:
    """Separate report for one semantically distinct target."""

    target: ForecastTarget
    sample_count: int
    metrics: tuple[BaselineMetric, ...]


@dataclass(frozen=True, slots=True)
class WalkForwardReport:
    """Reproducible infrastructure report without an economic-value claim."""

    targets: tuple[TargetEvaluation, ...]
    dataset_sha256: str
    seed: int
    point_in_time: bool = True
    economic_value_claim: bool = False


def _validate_series(series: EvaluationSeries, minimum_history: int) -> None:
    if series.target not in REQUIRED_EVALUATION_TARGETS:
        msg = "walk-forward evaluation target is unsupported"
        raise ValueError(msg)
    if not MINIMUM_SEASONAL_PERIOD <= series.seasonal_period <= MAXIMUM_SEASONAL_PERIOD:
        msg = "evaluation seasonal period is outside the supported bound"
        raise ValueError(msg)
    if len(series.points) <= minimum_history:
        msg = "evaluation series has insufficient history"
        raise ValueError(msg)
    previous_event = 0
    previous_availability = 0
    for point in series.points:
        if point.exchange_event_time_ns <= previous_event:
            msg = "evaluation event timestamps must be strictly increasing"
            raise ValueError(msg)
        if (
            point.available_wall_clock_utc_ns <= 0
            or point.available_wall_clock_utc_ns < previous_availability
        ):
            msg = "evaluation series violates point-in-time availability ordering"
            raise ValueError(msg)
        previous_event = point.exchange_event_time_ns
        previous_availability = point.available_wall_clock_utc_ns


def _dataset_digest(series_by_target: dict[ForecastTarget, EvaluationSeries]) -> str:
    payload = {
        target.name: [
            [
                point.exchange_event_time_ns,
                point.available_wall_clock_utc_ns,
                point.value,
            ]
            for point in series_by_target[target].points
        ]
        for target in sorted(series_by_target, key=int)
    }
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("ascii")
    return hashlib.sha256(encoded).hexdigest()


def compare_baselines_walk_forward(
    series_by_target: dict[ForecastTarget, EvaluationSeries],
    minimum_history: int,
    horizon_steps: int,
    seed: int,
) -> WalkForwardReport:
    """Evaluate required targets separately using only prefix-available data."""
    if set(series_by_target) != set(REQUIRED_EVALUATION_TARGETS):
        msg = "every required evaluation target must be supplied exactly once"
        raise ValueError(msg)
    if minimum_history < MINIMUM_EVALUATION_HISTORY or horizon_steps <= 0 or seed <= 0:
        msg = "walk-forward configuration is invalid"
        raise ValueError(msg)

    reports = []
    for target in REQUIRED_EVALUATION_TARGETS:
        series = series_by_target[target]
        if series.target is not target:
            msg = "evaluation series key and target do not agree"
            raise ValueError(msg)
        _validate_series(series, minimum_history)
        sample_count = len(series.points) - minimum_history - horizon_steps + 1
        if sample_count <= 0:
            msg = "evaluation series is too short for the requested horizon"
            raise ValueError(msg)
        metrics = []
        for baseline in _BASELINES_BY_TARGET[target]:
            absolute_error = 0
            squared_error = 0
            for split in range(minimum_history, len(series.points) - horizon_steps + 1):
                prefix = series.points[:split]
                cutoff = prefix[-1].available_wall_clock_utc_ns
                if any(point.available_wall_clock_utc_ns > cutoff for point in prefix):
                    msg = "walk-forward split contains point-in-time leakage"
                    raise ValueError(msg)
                values = tuple(point.value for point in prefix)
                prediction = baseline_point(
                    baseline, target, values, series.seasonal_period
                )
                outcome = series.points[split + horizon_steps - 1].value
                error = prediction - outcome
                absolute_error += abs(error)
                squared_error += error * error
            metrics.append(
                BaselineMetric(
                    baseline=baseline,
                    mean_absolute_error=absolute_error // sample_count,
                    root_mean_squared_error=math.isqrt(squared_error // sample_count),
                )
            )
        reports.append(TargetEvaluation(target, sample_count, tuple(metrics)))
    return WalkForwardReport(
        targets=tuple(reports),
        dataset_sha256=_dataset_digest(series_by_target),
        seed=seed,
    )
