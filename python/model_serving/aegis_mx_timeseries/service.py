"""Bounded worker, publication, cache, health, and lifecycle service core."""

from __future__ import annotations

import hashlib
import json
from collections import OrderedDict, deque
from dataclasses import dataclass
from typing import Protocol

from aegis_mx_intelligence import (
    ConfigurationVersion,
    ForecastId,
    ForecastTargetCode,
    ForecastUnitCode,
    GlobalEventId,
    Identifier128,
    ModelForecastContractInput,
    ModelId,
    ModelVersion,
)
from aegis_mx_intelligence.contracts import build_model_forecast_contract

from aegis_mx_timeseries.adapters import (
    AdapterForecast,
    AdapterResult,
    BaselineAdapter,
    ForecastModelAdapter,
)
from aegis_mx_timeseries.domain import (
    AdapterDevice,
    AdapterDeviceError,
    AdapterUnavailableError,
    BaselineKind,
    ForecastContext,
    ForecastTarget,
    ModelHealth,
    WorkerStatus,
    derived_identifier,
    validate_checkpoint_sha256,
)

VERSION = "0.2.0"
MAX_RESULT_CAPACITY = 4_096


class MonotonicClock(Protocol):
    """Injectable monotonic source; tests never sample a system clock."""

    def __call__(self) -> int:
        """Return session-local monotonic nanoseconds."""


@dataclass(frozen=True, slots=True)
class WorkerConfig:
    """Bounded worker capacity and freshness configuration."""

    forecast_ttl_ns: int
    maximum_forecast_age_ns: int
    queue_capacity: int
    result_capacity: int
    maximum_batch: int


@dataclass(frozen=True, slots=True)
class ServiceConfig:
    """Immutable effective configuration for one service generation."""

    configuration_version: ConfigurationVersion
    forecast_ttl_ns: int
    maximum_forecast_age_ns: int
    checkpoint_sha256: str | None
    primary_device: AdapterDevice
    fallback_baseline: BaselineKind | None
    queue_capacity: int
    result_capacity: int
    maximum_batch: int

    def __post_init__(self) -> None:
        """Validate all capacities, lifetimes, and checkpoint identity."""
        validate_checkpoint_sha256(self.checkpoint_sha256)
        if (
            self.forecast_ttl_ns <= 0
            or self.maximum_forecast_age_ns <= 0
            or self.maximum_forecast_age_ns > self.forecast_ttl_ns
            or not 0 < self.queue_capacity <= MAX_RESULT_CAPACITY
            or not 0 < self.result_capacity <= MAX_RESULT_CAPACITY
            or not 0 < self.maximum_batch <= self.queue_capacity
        ):
            msg = "service capacity or forecast-age configuration is invalid"
            raise ValueError(msg)

    def sha256(self) -> str:
        """Return a canonical hash without secrets, host state, or wall time."""
        payload = {
            "checkpoint_sha256": self.checkpoint_sha256,
            "configuration_version": self.configuration_version.hex(),
            "fallback_baseline": (
                self.fallback_baseline.name
                if self.fallback_baseline is not None
                else None
            ),
            "forecast_ttl_ns": self.forecast_ttl_ns,
            "maximum_batch": self.maximum_batch,
            "maximum_forecast_age_ns": self.maximum_forecast_age_ns,
            "primary_device": self.primary_device.name,
            "queue_capacity": self.queue_capacity,
            "result_capacity": self.result_capacity,
        }
        encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode(
            "ascii"
        )
        return hashlib.sha256(encoded).hexdigest()


@dataclass(frozen=True, slots=True)
class ForecastRequest:
    """One immutable request and monotonic completion deadline."""

    request_id: GlobalEventId
    context: ForecastContext
    deadline_process_monotonic_time_ns: int


@dataclass(frozen=True, slots=True)
class ForecastCacheKey:
    """Full key preventing target or horizon collisions."""

    context_sha256: bytes
    model_id: ModelId
    model_version: ModelVersion
    target: ForecastTarget
    horizon_ns: int
    checkpoint_sha256: str | None


@dataclass(frozen=True, slots=True)
class PublishedForecast:
    """Validated canonical publication retained by the off-path cache."""

    cache_key: ForecastCacheKey
    forecast_id: ForecastId
    target: ForecastTarget
    horizon_ns: int
    point_value: int
    quantile_values: tuple[int, int, int]
    producer_name: str
    model_id: ModelId
    model_version: ModelVersion
    device: AdapterDevice
    checkpoint_sha256: str | None
    created_process_monotonic_time_ns: int
    valid_until_process_monotonic_time_ns: int
    contract_bytes: bytes


@dataclass(frozen=True, slots=True)
class WorkerResult:
    """Completed or rejected request result."""

    status: WorkerStatus
    forecasts: tuple[PublishedForecast, ...] = ()
    used_fallback: bool = False
    reason: str = ""


@dataclass(frozen=True, slots=True)
class ServiceStatus:
    """Machine-readable service contract snapshot."""

    healthy: bool
    ready: bool
    model_health: ModelHealth
    version: str
    configuration_sha256: str
    live_trading_capable: bool = False


@dataclass(slots=True)
class ServiceMetrics:
    """Fixed-cardinality counters; series values never become labels."""

    forecast_requests_total: int = 0
    queue_rejections_total: int = 0
    deadline_misses_total: int = 0
    adapter_failures_total: int = 0
    gpu_failures_total: int = 0
    cpu_fallbacks_total: int = 0
    baseline_fallbacks_total: int = 0
    cache_hits_total: int = 0
    cache_stale_total: int = 0
    published_forecasts_total: int = 0


class ForecastCache:
    """Bounded off-path cache enforcing expiration and publication age."""

    def __init__(self, capacity: int, maximum_forecast_age_ns: int) -> None:
        """Create an empty fixed-capacity logical cache."""
        if capacity <= 0 or maximum_forecast_age_ns <= 0:
            msg = "forecast cache bounds must be positive"
            raise ValueError(msg)
        self._capacity = capacity
        self._maximum_age_ns = maximum_forecast_age_ns
        self._entries: OrderedDict[ForecastCacheKey, PublishedForecast] = OrderedDict()
        self._stale_count = 0

    def put(self, forecast: PublishedForecast) -> None:
        """Insert or replace one immutable entry with deterministic eviction."""
        if (
            forecast.created_process_monotonic_time_ns <= 0
            or forecast.valid_until_process_monotonic_time_ns
            <= forecast.created_process_monotonic_time_ns
            or not forecast.contract_bytes
        ):
            msg = "published forecast is malformed"
            raise ValueError(msg)
        self._entries.pop(forecast.cache_key, None)
        while len(self._entries) >= self._capacity:
            self._entries.popitem(last=False)
        self._entries[forecast.cache_key] = forecast

    def get(self, key: ForecastCacheKey, now_ns: int) -> PublishedForecast | None:
        """Return only fresh forecasts and remove stale or regressed entries."""
        forecast = self._entries.get(key)
        if forecast is None:
            return None
        if (
            now_ns < forecast.created_process_monotonic_time_ns
            or now_ns > forecast.valid_until_process_monotonic_time_ns
            or now_ns - forecast.created_process_monotonic_time_ns
            > self._maximum_age_ns
        ):
            del self._entries[key]
            self._stale_count += 1
            return None
        return forecast

    def __len__(self) -> int:
        """Return the bounded number of current entries."""
        return len(self._entries)

    @property
    def stale_count(self) -> int:
        """Return the number of stale or time-regressed entries removed."""
        return self._stale_count


_TARGET_CODES = {
    ForecastTarget.RETURN: ForecastTargetCode.RETURN,
    ForecastTarget.REALIZED_VOLATILITY: ForecastTargetCode.REALIZED_VOLATILITY,
    ForecastTarget.VOLUME: ForecastTargetCode.VOLUME,
    ForecastTarget.SPREAD: ForecastTargetCode.SPREAD,
    ForecastTarget.MARKET_FACTOR: ForecastTargetCode.MARKET_FACTOR,
    ForecastTarget.SECTOR_FACTOR: ForecastTargetCode.SECTOR_FACTOR,
}

_UNIT_CODES = {
    ForecastTarget.RETURN: ForecastUnitCode.RETURN_PPM,
    ForecastTarget.REALIZED_VOLATILITY: ForecastUnitCode.VOLATILITY_PPM,
    ForecastTarget.VOLUME: ForecastUnitCode.VOLUME_UNITS,
    ForecastTarget.SPREAD: ForecastUnitCode.SPREAD_TICKS,
    ForecastTarget.MARKET_FACTOR: ForecastUnitCode.FACTOR_PPM,
    ForecastTarget.SECTOR_FACTOR: ForecastUnitCode.FACTOR_PPM,
}


def _directional_probabilities(point: int) -> tuple[int, int, int]:
    if point > 0:
        return (300_000, 100_000, 600_000)
    if point < 0:
        return (600_000, 100_000, 300_000)
    return (450_000, 100_000, 450_000)


class ForecastPublisher:
    """Map adapter output into the v1.4 contract retaining v1.3 target fields."""

    def __init__(self, forecast_ttl_ns: int) -> None:
        """Create a publisher with one immutable monotonic lifetime."""
        self._forecast_ttl_ns = forecast_ttl_ns

    def publish(
        self,
        context: ForecastContext,
        result: AdapterResult,
        forecast: AdapterForecast,
        now_ns: int,
    ) -> PublishedForecast:
        """Validate and serialize one target/horizon forecast."""
        result.validate_for(context)
        forecast_identifier = derived_identifier(
            "forecast",
            context.context_sha256,
            result.model_id.hex(),
            result.model_version.hex(),
            int(context.target),
            forecast.horizon_ns,
            result.checkpoint_sha256 or "zero-shot",
        )
        record_identifier = derived_identifier(
            "forecast-record", forecast_identifier.hex()
        )
        probability_down, probability_flat, probability_up = (
            _directional_probabilities(forecast.point_value)
            if context.target is ForecastTarget.RETURN
            else (0, 1_000_000, 0)
        )
        is_return = context.target is ForecastTarget.RETURN
        volatility = (
            forecast.point_value
            if context.target is ForecastTarget.REALIZED_VOLATILITY
            else min(
                10_000_000,
                max(0, forecast.quantile_values[2] - forecast.quantile_values[0]),
            )
        )
        valid_until = now_ns + self._forecast_ttl_ns
        contract = build_model_forecast_contract(
            ModelForecastContractInput(
                record_id=GlobalEventId(record_identifier),
                forecast_id=ForecastId(forecast_identifier),
                session_id=context.session_id,
                model_id=result.model_id,
                model_version=result.model_version,
                instrument_id=context.instrument_id,
                feature_snapshot_id=context.feature_snapshot_id,
                configuration_version=context.configuration_version,
                expected_return_ppm=forecast.point_value if is_return else 0,
                return_p10_ppm=forecast.quantile_values[0] if is_return else 0,
                return_p50_ppm=forecast.quantile_values[1] if is_return else 0,
                return_p90_ppm=forecast.quantile_values[2] if is_return else 0,
                probability_down_ppm=probability_down,
                probability_flat_ppm=probability_flat,
                probability_up_ppm=probability_up,
                volatility_ppm=volatility,
                confidence_ppm=forecast.confidence_ppm,
                calibration_score_ppm=forecast.calibration_score_ppm,
                data_quality_score_ppm=forecast.data_quality_score_ppm,
                ood_score_ppm=forecast.ood_score_ppm,
                horizon_ns=forecast.horizon_ns,
                as_of_exchange_event_time_ns=context.as_of_exchange_event_time_ns,
                production_process_monotonic_time_ns=now_ns,
                expiration_process_monotonic_time_ns=valid_until,
                forecast_target=_TARGET_CODES[context.target],
                forecast_unit=_UNIT_CODES[context.target],
                target_value=forecast.point_value,
                target_p10=forecast.quantile_values[0],
                target_p50=forecast.quantile_values[1],
                target_p90=forecast.quantile_values[2],
            )
        )
        key = ForecastCacheKey(
            context.context_sha256,
            result.model_id,
            result.model_version,
            context.target,
            forecast.horizon_ns,
            result.checkpoint_sha256,
        )
        return PublishedForecast(
            cache_key=key,
            forecast_id=ForecastId(forecast_identifier),
            target=context.target,
            horizon_ns=forecast.horizon_ns,
            point_value=forecast.point_value,
            quantile_values=forecast.quantile_values,
            producer_name=result.producer_name,
            model_id=result.model_id,
            model_version=result.model_version,
            device=result.device,
            checkpoint_sha256=result.checkpoint_sha256,
            created_process_monotonic_time_ns=now_ns,
            valid_until_process_monotonic_time_ns=valid_until,
            contract_bytes=contract,
        )


class ForecastWorker:
    """Bounded batch worker with deadline, device, fallback, and cache policy."""

    def __init__(
        self,
        config: ServiceConfig,
        adapter: ForecastModelAdapter,
        clock: MonotonicClock,
        metrics: ServiceMetrics,
    ) -> None:
        """Create one worker with preconfigured bounded queues and fallback."""
        self._config = config
        self._adapter = adapter
        self._clock = clock
        self._metrics = metrics
        self._requests: deque[ForecastRequest] = deque()
        self._results: OrderedDict[Identifier128, WorkerResult] = OrderedDict()
        self._cache = ForecastCache(
            config.result_capacity * config.maximum_batch,
            config.maximum_forecast_age_ns,
        )
        self._publisher = ForecastPublisher(config.forecast_ttl_ns)
        self._health = ModelHealth.HEALTHY
        self._stopped = False
        fallback_id = derived_identifier("fallback-model", config.sha256())
        fallback_version = derived_identifier("fallback-version", config.sha256())
        self._fallback = (
            BaselineAdapter(
                config.fallback_baseline,
                ModelId(fallback_id),
                ModelVersion(fallback_version),
                seasonal_period=8,
            )
            if config.fallback_baseline is not None
            else None
        )

    @property
    def health(self) -> ModelHealth:
        """Return the current fail-closed model health."""
        return self._health

    @property
    def stopped(self) -> bool:
        """Return whether admission is permanently stopped."""
        return self._stopped

    def submit(self, request: ForecastRequest) -> WorkerStatus:
        """Admit without waiting or reject with a stable reason."""
        self._metrics.forecast_requests_total += 1
        if self._stopped:
            return WorkerStatus.STOPPED
        now_ns = self._clock()
        if (
            request.deadline_process_monotonic_time_ns <= 0
            or now_ns <= 0
            or now_ns > request.deadline_process_monotonic_time_ns
        ):
            self._metrics.deadline_misses_total += 1
            return WorkerStatus.DEADLINE_MISSED
        if (
            request.context.configuration_version != self._config.configuration_version
            or request.context.built_process_monotonic_time_ns > now_ns
            or any(item.request_id == request.request_id for item in self._requests)
            or request.request_id in self._results
        ):
            return WorkerStatus.INVALID_REQUEST
        if len(self._requests) >= self._config.queue_capacity:
            self._metrics.queue_rejections_total += 1
            return WorkerStatus.QUEUE_FULL
        self._requests.append(request)
        return WorkerStatus.ACCEPTED

    def _store(self, request_id: Identifier128, result: WorkerResult) -> None:
        self._results.pop(request_id, None)
        while len(self._results) >= self._config.result_capacity:
            self._results.popitem(last=False)
        self._results[request_id] = result

    def _primary_cache_key(
        self, context: ForecastContext, horizon_ns: int
    ) -> ForecastCacheKey:
        return ForecastCacheKey(
            context.context_sha256,
            self._adapter.model_id,
            self._adapter.model_version,
            context.target,
            horizon_ns,
            self._config.checkpoint_sha256,
        )

    def _cached(self, request: ForecastRequest, now_ns: int) -> WorkerResult | None:
        stale_before = self._cache.stale_count
        forecasts = tuple(
            self._cache.get(self._primary_cache_key(request.context, horizon), now_ns)
            for horizon in request.context.horizons_ns
        )
        self._metrics.cache_stale_total += self._cache.stale_count - stale_before
        if all(item is not None for item in forecasts):
            self._metrics.cache_hits_total += len(forecasts)
            return WorkerResult(
                WorkerStatus.COMPLETED,
                tuple(item for item in forecasts if item is not None),
            )
        return None

    def _invoke(
        self, contexts: tuple[ForecastContext, ...]
    ) -> tuple[tuple[AdapterResult, ...], bool]:
        try:
            return (
                self._adapter.forecast_batch(
                    contexts,
                    self._config.primary_device,
                    self._config.checkpoint_sha256,
                ),
                False,
            )
        except AdapterDeviceError:
            self._health = ModelHealth.DEGRADED
            if self._config.primary_device is not AdapterDevice.GPU:
                return self._invoke_fallback(contexts)
            self._metrics.gpu_failures_total += 1
            try:
                self._metrics.cpu_fallbacks_total += 1
                return (
                    self._adapter.forecast_batch(
                        contexts, AdapterDevice.CPU, self._config.checkpoint_sha256
                    ),
                    False,
                )
            except (AdapterDeviceError, AdapterUnavailableError, ValueError):
                return self._invoke_fallback(contexts)
        except (AdapterUnavailableError, ValueError):
            self._health = ModelHealth.DEGRADED
            return self._invoke_fallback(contexts)

    def _invoke_fallback(
        self, contexts: tuple[ForecastContext, ...]
    ) -> tuple[tuple[AdapterResult, ...], bool]:
        self._metrics.adapter_failures_total += 1
        self._health = ModelHealth.DEGRADED
        if self._fallback is None:
            msg = "primary inference and fallback failed"
            raise AdapterUnavailableError(msg) from None
        self._metrics.baseline_fallbacks_total += 1
        return (self._fallback.forecast_batch(contexts, AdapterDevice.CPU, None), True)

    def _active_requests(
        self, batch: tuple[ForecastRequest, ...], now_ns: int
    ) -> list[ForecastRequest]:
        active = []
        for request in batch:
            if now_ns > request.deadline_process_monotonic_time_ns:
                self._metrics.deadline_misses_total += 1
                self._store(
                    request.request_id,
                    WorkerResult(WorkerStatus.DEADLINE_MISSED, reason="deadline"),
                )
                continue
            cached = self._cached(request, now_ns)
            if cached is not None:
                self._store(request.request_id, cached)
                continue
            active.append(request)
        return active

    @staticmethod
    def _validate_result_batch(
        active: list[ForecastRequest], adapter_results: tuple[AdapterResult, ...]
    ) -> None:
        if len(adapter_results) != len(active):
            msg = "adapter batch result count mismatch"
            raise AdapterUnavailableError(msg)
        for request, result in zip(active, adapter_results, strict=True):
            result.validate_for(request.context)

    def _record_model_failure(
        self, active: list[ForecastRequest], error: Exception
    ) -> None:
        self._metrics.adapter_failures_total += 1
        self._health = ModelHealth.FAILED
        for request in active:
            self._store(
                request.request_id,
                WorkerResult(WorkerStatus.MODEL_FAILURE, reason=type(error).__name__),
            )

    def _publish_batch(
        self,
        active: list[ForecastRequest],
        adapter_results: tuple[AdapterResult, ...],
        *,
        used_fallback: bool,
    ) -> None:
        completed_ns = self._clock()
        for request, result in zip(active, adapter_results, strict=True):
            if completed_ns > request.deadline_process_monotonic_time_ns:
                self._metrics.deadline_misses_total += 1
                self._store(
                    request.request_id,
                    WorkerResult(
                        WorkerStatus.DEADLINE_MISSED, reason="late_completion"
                    ),
                )
                continue
            forecasts = tuple(
                self._publisher.publish(request.context, result, forecast, completed_ns)
                for forecast in result.forecasts
            )
            for forecast in forecasts:
                self._cache.put(forecast)
            self._metrics.published_forecasts_total += len(forecasts)
            self._store(
                request.request_id,
                WorkerResult(
                    WorkerStatus.FALLBACK_COMPLETED
                    if used_fallback
                    else WorkerStatus.COMPLETED,
                    forecasts,
                    used_fallback,
                ),
            )

    def run_once(self) -> int:
        """Process at most one configured batch and never wait for work."""
        if self._stopped or not self._requests:
            return 0
        batch = tuple(
            self._requests.popleft()
            for _ in range(min(self._config.maximum_batch, len(self._requests)))
        )
        active = self._active_requests(batch, self._clock())
        if not active:
            return len(batch)
        try:
            adapter_results, used_fallback = self._invoke(
                tuple(request.context for request in active)
            )
            self._validate_result_batch(active, adapter_results)
        except (AdapterDeviceError, AdapterUnavailableError, ValueError) as error:
            self._record_model_failure(active, error)
            return len(batch)
        self._publish_batch(active, adapter_results, used_fallback=used_fallback)
        return len(batch)

    def poll(self, request_id: Identifier128) -> WorkerResult:
        """Return and remove one completed result without waiting."""
        return self._results.pop(
            request_id, WorkerResult(WorkerStatus.NO_RESULT, reason="not_ready")
        )

    def shutdown(self, deadline_ns: int) -> WorkerStatus:
        """Stop admission and mark queued work stopped within the supplied bound."""
        if deadline_ns < self._clock():
            return WorkerStatus.DEADLINE_MISSED
        self._stopped = True
        while self._requests:
            request = self._requests.popleft()
            self._store(
                request.request_id,
                WorkerResult(WorkerStatus.STOPPED, reason="shutdown"),
            )
        self._health = ModelHealth.DISABLED
        return WorkerStatus.STOPPED


class TimeseriesForecastService:
    """Service facade exposing lifecycle, metrics, cache, and bounded worker."""

    def __init__(
        self,
        config: ServiceConfig,
        adapter: ForecastModelAdapter,
        clock: MonotonicClock,
    ) -> None:
        """Build one facade over the configured worker generation."""
        self._config = config
        self._metrics = ServiceMetrics()
        self._worker = ForecastWorker(config, adapter, clock, self._metrics)

    def submit(self, request: ForecastRequest) -> WorkerStatus:
        """Submit without waiting."""
        return self._worker.submit(request)

    def run_worker_once(self) -> int:
        """Process one bounded worker batch."""
        return self._worker.run_once()

    def poll(self, request_id: Identifier128) -> WorkerResult:
        """Poll one request without waiting."""
        return self._worker.poll(request_id)

    def status(self) -> ServiceStatus:
        """Return the common health/readiness/build/configuration contract."""
        health = self._worker.health
        healthy = health not in {ModelHealth.UNKNOWN, ModelHealth.FAILED}
        ready = healthy and not self._worker.stopped
        return ServiceStatus(
            healthy=healthy,
            ready=ready,
            model_health=health,
            version=VERSION,
            configuration_sha256=self._config.sha256(),
        )

    def prometheus_metrics(self) -> str:
        """Render fixed-cardinality Prometheus text without target labels."""
        values = (
            ("forecast_requests_total", self._metrics.forecast_requests_total),
            ("forecast_queue_rejections_total", self._metrics.queue_rejections_total),
            ("forecast_deadline_misses_total", self._metrics.deadline_misses_total),
            ("forecast_adapter_failures_total", self._metrics.adapter_failures_total),
            ("forecast_gpu_failures_total", self._metrics.gpu_failures_total),
            ("forecast_cpu_fallbacks_total", self._metrics.cpu_fallbacks_total),
            (
                "forecast_baseline_fallbacks_total",
                self._metrics.baseline_fallbacks_total,
            ),
            ("forecast_cache_hits_total", self._metrics.cache_hits_total),
            ("forecast_cache_stale_total", self._metrics.cache_stale_total),
            ("forecast_published_total", self._metrics.published_forecasts_total),
        )
        return "".join(f"{name} {value}\n" for name, value in values)

    def shutdown(self, deadline_ns: int) -> WorkerStatus:
        """Stop admission and fail queued work closed."""
        return self._worker.shutdown(deadline_ns)
