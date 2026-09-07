"""Deterministic tests for the off-hot-path time-series forecast service."""

from __future__ import annotations

import base64
import http.client
import json
import signal
import threading
import time
from dataclasses import replace
from http import HTTPStatus
from http.server import ThreadingHTTPServer
from typing import TYPE_CHECKING, cast

import aegis_mx_timeseries.adapters as adapters_module
import aegis_mx_timeseries.domain as domain_module
import aegis_mx_timeseries.evaluation as evaluation_module
import aegis_mx_timeseries.runtime as runtime_module
import aegis_mx_timeseries.service as service_module
import pytest
from aegis_mx_intelligence import (
    ConfigurationVersion,
    FeatureSnapshotId,
    GlobalEventId,
    Identifier128,
    InstrumentId,
    ModelId,
    ModelVersion,
    SessionId,
)
from aegis_mx_timeseries import (
    REQUIRED_EVALUATION_TARGETS,
    AdapterDevice,
    AdapterDeviceError,
    AdapterForecast,
    AdapterResult,
    AdapterUnavailableError,
    BaselineAdapter,
    BaselineKind,
    ContextBuilder,
    ContextBuildRequest,
    EvaluationSeries,
    ForecastCache,
    ForecastModelAdapter,
    ForecastRequest,
    ForecastTarget,
    KnownFutureCovariate,
    ModelHealth,
    ReferenceTimesFmAdapter,
    ServiceConfig,
    SessionState,
    TimeseriesForecastService,
    TimeSeriesPoint,
    TimesFmAdapter,
    WorkerStatus,
    cli,
    compare_baselines_walk_forward,
    decode_context,
    encode_context,
)
from aegis_mx_timeseries.runtime import (
    MAX_HTTP_BODY_BYTES,
    RoleApplication,
    ServiceRole,
    SystemMonotonicClock,
    build_reference_application,
    make_handler,
    serve,
    status_payload,
)

if TYPE_CHECKING:
    from collections.abc import Callable

    from aegis_mx_timeseries import ForecastContext


class ManualClock:
    """Explicit monotonic clock controlled by each test."""

    def __init__(self, now_ns: int = 1_000) -> None:
        self.now_ns = now_ns

    def __call__(self) -> int:
        return self.now_ns


def _identifier(seed: int) -> Identifier128:
    return Identifier128(seed, seed + 1)


def _history(
    target: ForecastTarget = ForecastTarget.RETURN,
    count: int = 32,
) -> tuple[TimeSeriesPoint, ...]:
    nonnegative = target in {
        ForecastTarget.REALIZED_VOLATILITY,
        ForecastTarget.VOLUME,
        ForecastTarget.SPREAD,
    }
    points = []
    for index in range(count):
        value = (index + 10) * 100 if nonnegative else ((index % 7) - 3) * 100
        points.append(
            TimeSeriesPoint(
                exchange_event_time_ns=1_800_000_000_000_000_000 + (index * 1_000),
                available_wall_clock_utc_ns=1_800_000_100_000_000_000 + (index * 1_000),
                value=value,
            )
        )
    return tuple(points)


def _request(
    target: ForecastTarget = ForecastTarget.RETURN,
    *,
    horizons: tuple[int, ...] = (1_000, 2_000),
    history: tuple[TimeSeriesPoint, ...] | None = None,
    covariates: tuple[KnownFutureCovariate, ...] | None = None,
) -> ContextBuildRequest:
    resolved_history = history if history is not None else _history(target)
    as_of = resolved_history[-1].exchange_event_time_ns
    point_in_time = resolved_history[-1].available_wall_clock_utc_ns
    resolved_covariates = covariates
    if resolved_covariates is None:
        resolved_covariates = tuple(
            KnownFutureCovariate(
                horizon_ns=horizon,
                exchange_event_time_ns=as_of + horizon,
                known_wall_clock_utc_ns=point_in_time,
                time_of_day_second=34_200 + index,
                session_state=SessionState.OPEN,
                scheduled_event_flags=index,
            )
            for index, horizon in enumerate(horizons)
        )
    return ContextBuildRequest(
        instrument_id=InstrumentId(_identifier(1)),
        session_id=SessionId(_identifier(3)),
        feature_snapshot_id=FeatureSnapshotId(_identifier(5)),
        configuration_version=ConfigurationVersion(_identifier(7)),
        feature_definition_version=ConfigurationVersion(_identifier(9)),
        source_first_global_event_id=GlobalEventId(_identifier(11)),
        source_last_global_event_id=GlobalEventId(_identifier(13)),
        target=target,
        as_of_exchange_event_time_ns=as_of,
        point_in_time_wall_clock_utc_ns=point_in_time,
        built_process_monotonic_time_ns=900,
        source_payload_sha256=bytes(range(32)),
        horizons_ns=horizons,
        quantiles_ppm=(100_000, 500_000, 900_000),
        history=resolved_history,
        future_covariates=resolved_covariates,
    )


@pytest.fixture
def context() -> ForecastContext:
    return ContextBuilder().build(_request())


def test_context_is_deterministic_and_round_trips(context: ForecastContext) -> None:
    second = ContextBuilder().build(_request())
    assert context == second
    assert len(context.context_sha256) == 32
    assert decode_context(encode_context(context)) == context


@pytest.mark.parametrize(
    ("changes", "message"),
    [
        ({"history": _history(count=3)}, "history"),
        ({"horizons_ns": ()}, "horizon"),
        ({"horizons_ns": (2_000, 1_000)}, "increasing"),
        ({"quantiles_ppm": (100_000, 100_000, 900_000)}, "quantile"),
        ({"source_payload_sha256": b"short"}, "SHA-256"),
        ({"built_process_monotonic_time_ns": 0}, "monotonic"),
    ],
)
def test_context_rejects_malformed_shape(
    changes: dict[str, object], message: str
) -> None:
    with pytest.raises(ValueError, match=message):
        ContextBuilder().build(replace(_request(), **changes))  # type: ignore[arg-type]


def test_context_rejects_nonmonotonic_or_future_history() -> None:
    duplicate = (*_history()[:-1], _history()[-2])
    with pytest.raises(ValueError, match="strictly increasing"):
        ContextBuilder().build(_request(history=duplicate))

    future = list(_history())
    future[-1] = replace(
        future[-1],
        available_wall_clock_utc_ns=future[-1].available_wall_clock_utc_ns + 1,
    )
    request = _request(history=tuple(future))
    request = replace(
        request,
        point_in_time_wall_clock_utc_ns=future[-2].available_wall_clock_utc_ns,
    )
    with pytest.raises(ValueError, match="point-in-time"):
        ContextBuilder().build(request)


def test_context_rejects_missing_or_invalid_covariates() -> None:
    with pytest.raises(ValueError, match="covariate"):
        ContextBuilder().build(_request(covariates=()))

    wrong_time = replace(
        _request().future_covariates[0],
        exchange_event_time_ns=_request().as_of_exchange_event_time_ns + 9_999,
    )
    with pytest.raises(ValueError, match="future timestamp"):
        ContextBuilder().build(
            _request(covariates=(wrong_time, _request().future_covariates[1]))
        )

    late = replace(
        _request().future_covariates[0],
        known_wall_clock_utc_ns=_request().point_in_time_wall_clock_utc_ns + 1,
    )
    with pytest.raises(ValueError, match="point-in-time"):
        ContextBuilder().build(
            _request(covariates=(late, _request().future_covariates[1]))
        )


@pytest.mark.parametrize(
    "target",
    list(ForecastTarget),
)
def test_reference_adapter_supports_every_target_and_multiple_horizons(
    target: ForecastTarget,
) -> None:
    context = ContextBuilder().build(_request(target))
    adapter = ReferenceTimesFmAdapter(
        ModelId(_identifier(101)), ModelVersion(_identifier(103))
    )
    (result,) = adapter.forecast_batch(
        (context,), AdapterDevice.CPU, checkpoint_sha256=None
    )
    assert result.target is target
    assert result.context_sha256 == context.context_sha256
    assert [forecast.horizon_ns for forecast in result.forecasts] == [1_000, 2_000]
    assert all(
        forecast.quantile_values[0]
        <= forecast.quantile_values[1]
        <= forecast.quantile_values[2]
        for forecast in result.forecasts
    )
    assert (
        all(forecast.point_value >= 0 for forecast in result.forecasts)
        if target
        in {
            ForecastTarget.REALIZED_VOLATILITY,
            ForecastTarget.VOLUME,
            ForecastTarget.SPREAD,
        }
        else True
    )


class RecordingBackend:
    """Minimal replaceable TimesFM backend used only as a test double."""

    def __init__(self) -> None:
        self.calls: list[tuple[AdapterDevice, str | None, int]] = []

    def forecast_batch(
        self,
        contexts: tuple[ForecastContext, ...],
        device: AdapterDevice,
        checkpoint_sha256: str | None,
    ) -> tuple[AdapterResult, ...]:
        self.calls.append((device, checkpoint_sha256, len(contexts)))
        return ReferenceTimesFmAdapter(
            ModelId(_identifier(105)), ModelVersion(_identifier(107))
        ).forecast_batch(contexts, AdapterDevice.CPU, checkpoint_sha256)


def test_timesfm_adapter_is_replaceable_zero_shot_and_checkpoint_aware(
    context: ForecastContext,
) -> None:
    backend = RecordingBackend()
    adapter = TimesFmAdapter(
        backend,
        ModelId(_identifier(109)),
        ModelVersion(_identifier(111)),
    )
    adapter.forecast_batch((context,), AdapterDevice.CPU, None)
    checkpoint = "ab" * 32
    adapter.forecast_batch((context,), AdapterDevice.GPU, checkpoint)
    assert backend.calls == [
        (AdapterDevice.CPU, None, 1),
        (AdapterDevice.GPU, checkpoint, 1),
    ]


@pytest.mark.parametrize("kind", list(BaselineKind))
def test_interpretable_baseline_suite_is_deterministic(
    context: ForecastContext, kind: BaselineKind
) -> None:
    adapter = BaselineAdapter(
        kind,
        ModelId(_identifier(201 + kind.value)),
        ModelVersion(_identifier(221 + kind.value)),
        seasonal_period=4,
    )
    first = adapter.forecast_batch((context,), AdapterDevice.CPU, None)
    second = adapter.forecast_batch((context,), AdapterDevice.CPU, None)
    assert first == second
    assert len(first[0].forecasts) == 2


class ControllableAdapter(ForecastModelAdapter):
    """Adapter double supporting GPU and complete failure injection."""

    def __init__(self, *, fail_gpu: bool = False, fail_all: bool = False) -> None:
        super().__init__(ModelId(_identifier(301)), ModelVersion(_identifier(303)))
        self.fail_gpu = fail_gpu
        self.fail_all = fail_all
        self.devices: list[AdapterDevice] = []
        self.batch_sizes: list[int] = []

    @property
    def adapter_name(self) -> str:
        return "controllable"

    def forecast_batch(
        self,
        contexts: tuple[ForecastContext, ...],
        device: AdapterDevice,
        checkpoint_sha256: str | None,
    ) -> tuple[AdapterResult, ...]:
        del checkpoint_sha256
        self.devices.append(device)
        self.batch_sizes.append(len(contexts))
        if self.fail_all:
            raise AdapterUnavailableError("injected adapter failure")
        if self.fail_gpu and device is AdapterDevice.GPU:
            raise AdapterDeviceError("injected GPU failure")
        return ReferenceTimesFmAdapter(
            self.model_id, self.model_version
        ).forecast_batch(contexts, AdapterDevice.CPU, None)


def _service(
    clock: ManualClock,
    adapter: ForecastModelAdapter,
    *,
    fallback: BaselineKind | None = BaselineKind.LAST_VALUE,
    queue_capacity: int = 8,
    maximum_batch: int = 4,
) -> TimeseriesForecastService:
    return TimeseriesForecastService(
        ServiceConfig(
            configuration_version=ConfigurationVersion(_identifier(7)),
            forecast_ttl_ns=1_000,
            maximum_forecast_age_ns=500,
            checkpoint_sha256=None,
            primary_device=AdapterDevice.GPU,
            fallback_baseline=fallback,
            queue_capacity=queue_capacity,
            result_capacity=queue_capacity,
            maximum_batch=min(maximum_batch, queue_capacity),
        ),
        adapter,
        clock,
    )


def _forecast_request(
    context: ForecastContext, sequence: int = 1, deadline_ns: int = 2_000
) -> ForecastRequest:
    return ForecastRequest(
        request_id=GlobalEventId(_identifier(500 + sequence)),
        context=context,
        deadline_process_monotonic_time_ns=deadline_ns,
    )


def test_worker_batches_and_publishes_common_forecasts(
    context: ForecastContext,
) -> None:
    clock = ManualClock()
    adapter = ControllableAdapter()
    service = _service(clock, adapter, maximum_batch=2)
    assert service.submit(_forecast_request(context, 1)) is WorkerStatus.ACCEPTED
    assert service.submit(_forecast_request(context, 2)) is WorkerStatus.ACCEPTED
    assert service.run_worker_once() == 2
    assert adapter.batch_sizes == [2]
    first = service.poll(_forecast_request(context, 1).request_id)
    assert first.status is WorkerStatus.COMPLETED
    assert len(first.forecasts) == 2
    assert all(item.contract_bytes.startswith(b"") for item in first.forecasts)
    assert all(item.target is ForecastTarget.RETURN for item in first.forecasts)


def test_worker_gpu_failure_retries_cpu_with_same_adapter(
    context: ForecastContext,
) -> None:
    clock = ManualClock()
    adapter = ControllableAdapter(fail_gpu=True)
    service = _service(clock, adapter, fallback=None)
    request = _forecast_request(context)
    assert service.submit(request) is WorkerStatus.ACCEPTED
    assert service.run_worker_once() == 1
    result = service.poll(request.request_id)
    assert result.status is WorkerStatus.COMPLETED
    assert result.used_fallback is False
    assert adapter.devices == [AdapterDevice.GPU, AdapterDevice.CPU]
    assert service.status().model_health is ModelHealth.DEGRADED


def test_worker_uses_explicit_deterministic_fallback(
    context: ForecastContext,
) -> None:
    clock = ManualClock()
    service = _service(clock, ControllableAdapter(fail_all=True))
    request = _forecast_request(context)
    assert service.submit(request) is WorkerStatus.ACCEPTED
    service.run_worker_once()
    result = service.poll(request.request_id)
    assert result.status is WorkerStatus.FALLBACK_COMPLETED
    assert result.used_fallback is True
    assert all(item.producer_name == "last_value" for item in result.forecasts)


def test_worker_rejects_deadline_and_capacity(context: ForecastContext) -> None:
    clock = ManualClock(2_001)
    service = _service(clock, ControllableAdapter(), queue_capacity=1)
    assert (
        service.submit(_forecast_request(context, deadline_ns=2_000))
        is WorkerStatus.DEADLINE_MISSED
    )
    clock.now_ns = 1_000
    assert service.submit(_forecast_request(context, 1)) is WorkerStatus.ACCEPTED
    assert service.submit(_forecast_request(context, 2)) is WorkerStatus.QUEUE_FULL


def test_worker_discards_completion_after_deadline(context: ForecastContext) -> None:
    clock = ManualClock()

    class LateAdapter(ControllableAdapter):
        def forecast_batch(
            self,
            contexts: tuple[ForecastContext, ...],
            device: AdapterDevice,
            checkpoint_sha256: str | None,
        ) -> tuple[AdapterResult, ...]:
            result = super().forecast_batch(contexts, device, checkpoint_sha256)
            clock.now_ns = 2_001
            return result

    service = _service(clock, LateAdapter())
    request = _forecast_request(context, deadline_ns=2_000)
    service.submit(request)
    service.run_worker_once()
    assert service.poll(request.request_id).status is WorkerStatus.DEADLINE_MISSED


def test_cache_rejects_stale_forecasts(context: ForecastContext) -> None:
    clock = ManualClock()
    service = _service(clock, ControllableAdapter())
    request = _forecast_request(context)
    service.submit(request)
    service.run_worker_once()
    completed = service.poll(request.request_id)
    cache = ForecastCache(capacity=4, maximum_forecast_age_ns=500)
    for forecast in completed.forecasts:
        cache.put(forecast)
        assert cache.get(forecast.cache_key, 1_500) == forecast
        assert cache.get(forecast.cache_key, 1_501) is None


def test_service_contract_health_metrics_and_shutdown(context: ForecastContext) -> None:
    clock = ManualClock()
    service = _service(clock, ControllableAdapter())
    status = service.status()
    assert status.healthy is True
    assert status.ready is True
    assert len(status.configuration_sha256) == 64
    assert status.live_trading_capable is False
    assert "forecast_requests_total 0" in service.prometheus_metrics()
    assert service.shutdown(1_100) is WorkerStatus.STOPPED
    assert service.submit(_forecast_request(context)) is WorkerStatus.STOPPED
    assert service.status().ready is False


def test_walk_forward_evaluates_required_targets_separately() -> None:
    series = {
        target: EvaluationSeries(target, _history(target, 48), seasonal_period=4)
        for target in REQUIRED_EVALUATION_TARGETS
    }
    report = compare_baselines_walk_forward(
        series,
        minimum_history=16,
        horizon_steps=1,
        seed=20_260_828,
    )
    assert {item.target for item in report.targets} == set(REQUIRED_EVALUATION_TARGETS)
    assert all(item.sample_count > 0 for item in report.targets)
    assert all(item.metrics for item in report.targets)
    assert report.point_in_time is True
    assert report.economic_value_claim is False
    assert len(report.dataset_sha256) == 64


def test_walk_forward_rejects_missing_target_and_future_leakage() -> None:
    incomplete = {
        target: EvaluationSeries(target, _history(target, 48), seasonal_period=4)
        for target in REQUIRED_EVALUATION_TARGETS[:-1]
    }
    with pytest.raises(ValueError, match="required evaluation target"):
        compare_baselines_walk_forward(incomplete, 16, 1, 20_260_828)

    points = list(_history(ForecastTarget.RETURN, 48))
    points[20] = replace(
        points[20],
        available_wall_clock_utc_ns=points[-1].available_wall_clock_utc_ns + 1,
    )
    leaking = {
        target: EvaluationSeries(
            target,
            tuple(points) if target is ForecastTarget.RETURN else _history(target, 48),
            seasonal_period=4,
        )
        for target in REQUIRED_EVALUATION_TARGETS
    }
    with pytest.raises(ValueError, match="point-in-time"):
        compare_baselines_walk_forward(leaking, 16, 1, 20_260_828)


def test_adapter_output_validation_rejects_malformed_result(
    context: ForecastContext,
) -> None:
    malformed = AdapterResult(
        context_sha256=context.context_sha256,
        target=context.target,
        forecasts=tuple(
            AdapterForecast(
                horizon_ns=horizon,
                point_value=0,
                quantile_values=(1, 0, 2),
                confidence_ppm=1_000_000,
                calibration_score_ppm=1_000_000,
                data_quality_score_ppm=1_000_000,
                ood_score_ppm=0,
            )
            for horizon in context.horizons_ns
        ),
        producer_name="malformed",
        model_id=ModelId(_identifier(701)),
        model_version=ModelVersion(_identifier(703)),
        device=AdapterDevice.CPU,
        checkpoint_sha256=None,
    )
    with pytest.raises(ValueError, match="quantile"):
        malformed.validate_for(context)


def test_role_application_exposes_common_operational_contract() -> None:
    clock = ManualClock()
    application = build_reference_application(ServiceRole.API, clock)
    assert application.role is ServiceRole.API
    assert application.status().ready is True

    health = application.handle("GET", "/healthz")
    assert health.status is HTTPStatus.OK
    assert json.loads(health.body) == {"healthy": True, "model_health": "healthy"}
    assert application.handle("GET", "/readyz").status is HTTPStatus.OK
    version = json.loads(application.handle("GET", "/version").body)
    assert version["role"] == "timeseries-forecast-api"
    assert version["live_trading_capable"] is False
    configuration = json.loads(application.handle("GET", "/configuration").body)
    assert len(configuration["configuration_sha256"]) == 64
    metrics = application.handle("GET", "/metrics")
    assert metrics.content_type == "text/plain; version=0.0.4"
    assert b"forecast_requests_total" in metrics.body
    assert application.handle("GET", "/missing").status is HTTPStatus.NOT_FOUND
    assert (
        application.handle("DELETE", "/healthz").status is HTTPStatus.METHOD_NOT_ALLOWED
    )
    assert (
        application.handle(
            "POST", "/v1/forecast", b"x" * (MAX_HTTP_BODY_BYTES + 1)
        ).status
        is HTTPStatus.REQUEST_ENTITY_TOO_LARGE
    )
    startup = status_payload(application)
    assert startup["event"] == "service_start"
    assert startup["live_trading_capable"] is False
    assert SystemMonotonicClock()() > 0


def test_context_builder_role_validates_and_canonicalizes_context(
    context: ForecastContext,
) -> None:
    application = RoleApplication(
        ServiceRole.CONTEXT_BUILDER,
        _service(ManualClock(), ControllableAdapter()),
        ManualClock(),
    )
    response = application.handle("POST", "/v1/context", encode_context(context))
    assert response.status is HTTPStatus.OK
    payload = json.loads(response.body)
    assert payload["context_sha256"] == context.context_sha256.hex()
    assert (
        decode_context(base64.b64decode(payload["canonical_context_base64"])) == context
    )
    assert (
        application.handle("POST", "/v1/context", b"not-json").status
        is HTTPStatus.BAD_REQUEST
    )
    assert (
        application.handle("POST", "/v1/forecast", encode_context(context)).status
        is HTTPStatus.NOT_FOUND
    )
    with pytest.raises(ValueError, match="deadline"):
        RoleApplication(
            ServiceRole.CONTEXT_BUILDER,
            _service(ManualClock(), ControllableAdapter()),
            ManualClock(),
            request_deadline_ns=0,
        )


@pytest.mark.parametrize("role", [ServiceRole.API, ServiceRole.WORKER])
def test_api_and_worker_publish_canonical_forecasts(
    context: ForecastContext,
    role: ServiceRole,
) -> None:
    clock = ManualClock()
    application = RoleApplication(
        role,
        _service(clock, ControllableAdapter()),
        clock,
    )
    response = application.handle("POST", "/v1/forecast", encode_context(context))
    assert response.status is HTTPStatus.OK
    payload = json.loads(response.body)
    assert payload["status"] == "completed"
    assert payload["used_fallback"] is False
    assert len(payload["forecasts"]) == len(context.horizons_ns)
    assert all(
        base64.b64decode(item["contract_base64"])[4:8] == b"AMCR"
        for item in payload["forecasts"]
    )
    assert (
        application.handle("POST", "/v1/forecast", b"invalid").status
        is HTTPStatus.BAD_REQUEST
    )


def test_role_application_reports_failed_model_and_stopped_admission(
    context: ForecastContext,
) -> None:
    clock = ManualClock()
    failed_service = _service(clock, ControllableAdapter(fail_all=True), fallback=None)
    failed_request = _forecast_request(context)
    assert failed_service.submit(failed_request) is WorkerStatus.ACCEPTED
    failed_service.run_worker_once()
    application = RoleApplication(ServiceRole.WORKER, failed_service, clock)
    assert (
        application.handle("GET", "/healthz").status is HTTPStatus.SERVICE_UNAVAILABLE
    )
    assert application.handle("GET", "/readyz").status is HTTPStatus.SERVICE_UNAVAILABLE

    stopped = RoleApplication(
        ServiceRole.API,
        _service(clock, ControllableAdapter()),
        clock,
    )
    assert stopped.shutdown(2_000) is WorkerStatus.STOPPED
    response = stopped.handle("POST", "/v1/forecast", encode_context(context))
    assert response.status is HTTPStatus.SERVICE_UNAVAILABLE
    assert json.loads(response.body)["status"] == "stopped"


def test_http_handler_frames_requests_and_structured_logs(
    context: ForecastContext,
    capsys: pytest.CaptureFixture[str],
) -> None:
    clock = ManualClock()
    application = RoleApplication(
        ServiceRole.CONTEXT_BUILDER,
        _service(clock, ControllableAdapter()),
        clock,
    )
    server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(application))
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        connection = http.client.HTTPConnection("127.0.0.1", server.server_port)
        connection.request("GET", "/version?credential=credential-canary")
        get_response = connection.getresponse()
        assert get_response.status == HTTPStatus.OK
        assert get_response.getheader("X-Content-Type-Options") == "nosniff"
        get_response.read()

        connection.request("POST", "/v1/context", body=encode_context(context))
        post_response = connection.getresponse()
        assert post_response.status == HTTPStatus.OK
        post_response.read()

        connection.putrequest("POST", "/v1/context")
        connection.putheader("Content-Length", "invalid")
        connection.endheaders()
        invalid_length = connection.getresponse()
        assert invalid_length.status == HTTPStatus.BAD_REQUEST
        invalid_length.read()

        connection.putrequest("POST", "/v1/context")
        connection.putheader("Content-Length", str(MAX_HTTP_BODY_BYTES + 1))
        connection.endheaders()
        oversized = connection.getresponse()
        assert oversized.status == HTTPStatus.REQUEST_ENTITY_TOO_LARGE
        oversized.read()
        connection.close()
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)
    logged = capsys.readouterr().err
    assert '"event":"http_request"' in logged
    assert "canonical_context" not in logged
    assert "credential-canary" not in logged


def test_serve_validates_address_and_cli_dispatch(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    application = build_reference_application(ServiceRole.API, ManualClock())
    with pytest.raises(ValueError, match="host or port"):
        serve(application, "", 8080)
    with pytest.raises(ValueError, match="host or port"):
        serve(application, "127.0.0.1", 0)

    dispatch: list[tuple[ServiceRole, str, int]] = []

    def fake_serve(
        app: RoleApplication,
        host: str,
        port: int,
        *,
        request_security: object,
        allow_insecure_loopback: bool,
    ) -> None:
        assert request_security is None
        assert allow_insecure_loopback is True
        dispatch.append((app.role, host, port))

    monkeypatch.setattr(cli, "serve", fake_serve)
    assert (
        cli.main(
            ServiceRole.WORKER,
            [
                "--host",
                "localhost",
                "--port",
                "9090",
                "--allow-insecure-loopback",
            ],
        )
        == 0
    )
    assert dispatch == [(ServiceRole.WORKER, "localhost", 9090)]

    called: list[ServiceRole] = []

    def fake_main(role: ServiceRole, arguments: object = None) -> int:
        del arguments
        called.append(role)
        return 0

    monkeypatch.setattr(cli, "main", fake_main)
    assert cli.api_main() == 0
    assert cli.worker_main() == 0
    assert cli.context_builder_main() == 0
    assert called == [ServiceRole.API, ServiceRole.WORKER, ServiceRole.CONTEXT_BUILDER]


def test_context_validation_covers_all_fail_closed_semantics(
    context: ForecastContext,
) -> None:
    with pytest.raises(ValueError, match="minimum history"):
        ContextBuilder(3)
    with pytest.raises(ValueError, match="minimum history"):
        ContextBuilder(domain_module.MAX_HISTORY_POINTS + 1)
    with pytest.raises(ValueError, match="unknown forecast target"):
        domain_module.target_bounds(cast("ForecastTarget", 0))

    after_as_of = list(_history())
    after_as_of[-1] = replace(
        after_as_of[-1],
        exchange_event_time_ns=after_as_of[-1].exchange_event_time_ns + 1,
    )
    with pytest.raises(ValueError, match="after the as-of"):
        ContextBuilder().build(
            replace(
                _request(),
                history=tuple(after_as_of),
                as_of_exchange_event_time_ns=_request().as_of_exchange_event_time_ns,
            )
        )

    out_of_range = list(_history())
    out_of_range[-1] = replace(out_of_range[-1], value=10_000_001)
    with pytest.raises(ValueError, match="target unit range"):
        ContextBuilder().build(_request(history=tuple(out_of_range)))
    with pytest.raises(ValueError, match="final history"):
        ContextBuilder().build(
            replace(
                _request(),
                as_of_exchange_event_time_ns=_request().as_of_exchange_event_time_ns
                + 1,
            )
        )

    first, second = _request().future_covariates
    invalid_covariates = (
        (replace(first, horizon_ns=9_999), "horizon does not match"),
        (replace(first, time_of_day_second=86_400), "time-of-day"),
        (replace(first, session_state=SessionState.UNKNOWN), "session state"),
        (replace(first, scheduled_event_flags=1 << 32), "uint32"),
    )
    for invalid, message in invalid_covariates:
        with pytest.raises(ValueError, match=message):
            ContextBuilder().build(_request(covariates=(invalid, second)))

    oversized = replace(context, history=context.history * 1_000)
    with pytest.raises(ValueError, match="exceeds"):
        encode_context(oversized)


def test_context_decoder_rejects_malformed_untrusted_json(
    context: ForecastContext,
) -> None:
    encoded = encode_context(context)
    with pytest.raises(ValueError, match="size"):
        decode_context(b"")
    with pytest.raises(ValueError, match="size"):
        decode_context(b"x" * (domain_module.MAX_CONTEXT_BYTES + 1))
    with pytest.raises(ValueError, match="ASCII JSON"):
        decode_context(b"\xff")
    with pytest.raises(ValueError, match="root"):
        decode_context(b"[]")

    payload = json.loads(encoded)

    def encoded_change(field: str, value: object) -> bytes:
        changed = dict(payload)
        changed[field] = value
        return json.dumps(changed, sort_keys=True, separators=(",", ":")).encode(
            "ascii"
        )

    missing = dict(payload)
    del missing["target"]
    with pytest.raises(ValueError, match="fields"):
        decode_context(json.dumps(missing).encode("ascii"))
    for field, value in (
        ("target", "not_a_target"),
        ("history", "not-an-array"),
        ("history", ["not-an-object"]),
        (
            "history",
            [
                {
                    "exchange_event_time_ns": True,
                    "available_wall_clock_utc_ns": 1,
                    "value": 1,
                }
            ],
        ),
        ("future_covariates", "not-an-array"),
        ("source_payload_sha256", "not-hex"),
    ):
        with pytest.raises(ValueError, match="malformed"):
            decode_context(encoded_change(field, value))
    with pytest.raises(ValueError, match="array"):
        decode_context(encoded_change("quantiles_ppm", "not-an-array"))
    with pytest.raises(ValueError, match="three"):
        decode_context(encoded_change("quantiles_ppm", [1, 2]))
    with pytest.raises(ValueError, match="array"):
        decode_context(encoded_change("horizons_ns", "not-an-array"))

    for identifier, message in (
        ("short", "32-character"),
        ("g" * 32, "hexadecimal"),
        ("A" * 32, "lowercase"),
    ):
        with pytest.raises(ValueError, match=message):
            decode_context(encoded_change("instrument_id", identifier))
    with pytest.raises(ValueError, match="does not match"):
        decode_context(encoded_change("context_sha256", "00" * 32))


def test_checkpoint_and_identifier_helpers_reject_mutable_names() -> None:
    identifier = domain_module.derived_identifier("domain", b"bytes", "text", 1)
    assert identifier.high != 0 or identifier.low != 0
    domain_module.validate_checkpoint_sha256(None)
    domain_module.validate_checkpoint_sha256("01" * 32)
    with pytest.raises(ValueError, match="64 lowercase"):
        domain_module.validate_checkpoint_sha256("A" * 64)
    with pytest.raises(ValueError, match="hexadecimal"):
        domain_module.validate_checkpoint_sha256("g" * 64)


def test_adapter_validation_and_comparator_failures_are_explicit(
    context: ForecastContext,
) -> None:
    adapter = ReferenceTimesFmAdapter(
        ModelId(_identifier(801)), ModelVersion(_identifier(803))
    )
    (valid,) = adapter.forecast_batch((context,), AdapterDevice.CPU, None)
    first = valid.forecasts[0]
    invalid_results = (
        (replace(valid, context_sha256=b"x" * 32), "detached"),
        (replace(valid, producer_name=""), "every requested"),
        (
            replace(
                valid,
                forecasts=(
                    replace(first, horizon_ns=9_999),
                    *valid.forecasts[1:],
                ),
            ),
            "horizon",
        ),
        (
            replace(
                valid,
                forecasts=(
                    replace(first, point_value=10_000_001),
                    *valid.forecasts[1:],
                ),
            ),
            "target range",
        ),
        (
            replace(
                valid,
                forecasts=(
                    replace(first, confidence_ppm=1_000_001),
                    *valid.forecasts[1:],
                ),
            ),
            "score",
        ),
    )
    for invalid, message in invalid_results:
        with pytest.raises(ValueError, match=message):
            invalid.validate_for(context)

    with pytest.raises(ValueError, match="denominator"):
        adapters_module._trunc_div(1, 0)
    one_point = replace(context, history=context.history[-1:])
    assert adapters_module._residual_scale(one_point) == 1
    with pytest.raises(AdapterUnavailableError, match="insufficient"):
        adapters_module.baseline_point(
            BaselineKind.LAST_VALUE, ForecastTarget.RETURN, (1, 2, 3), 4
        )
    with pytest.raises(ValueError, match="unknown baseline"):
        adapters_module.baseline_point(
            cast("BaselineKind", 0), ForecastTarget.RETURN, (1, 2, 3, 4), 4
        )
    with pytest.raises(ValueError, match="seasonal period"):
        BaselineAdapter(
            BaselineKind.LAST_VALUE,
            ModelId(_identifier(805)),
            ModelVersion(_identifier(807)),
            seasonal_period=1,
        )
    baseline = BaselineAdapter(
        BaselineKind.LAST_VALUE,
        ModelId(_identifier(809)),
        ModelVersion(_identifier(811)),
        seasonal_period=4,
    )
    with pytest.raises(AdapterDeviceError, match="CPU"):
        baseline.forecast_batch((context,), AdapterDevice.GPU, None)
    with pytest.raises(AdapterUnavailableError, match="checkpoints"):
        baseline.forecast_batch((context,), AdapterDevice.CPU, "01" * 32)


def test_timesfm_adapter_rejects_backend_count_and_covers_session_covariates(
    context: ForecastContext,
) -> None:
    class EmptyBackend:
        def forecast_batch(
            self,
            contexts: tuple[ForecastContext, ...],
            device: AdapterDevice,
            checkpoint_sha256: str | None,
        ) -> tuple[AdapterResult, ...]:
            del contexts, device, checkpoint_sha256
            return ()

    wrapper = TimesFmAdapter(
        EmptyBackend(), ModelId(_identifier(813)), ModelVersion(_identifier(815))
    )
    with pytest.raises(AdapterUnavailableError, match="result count"):
        wrapper.forecast_batch((context,), AdapterDevice.CPU, None)

    reference = ReferenceTimesFmAdapter(
        ModelId(_identifier(817)), ModelVersion(_identifier(819))
    )
    for state in (
        SessionState.PRE_OPEN,
        SessionState.HALTED,
        SessionState.AUCTION,
        SessionState.CLOSED,
    ):
        covariates = tuple(
            replace(covariate, session_state=state)
            for covariate in context.future_covariates
        )
        changed = ContextBuilder().build(_request(covariates=covariates))
        assert reference.forecast_batch((changed,), AdapterDevice.CPU, None)

    assert adapters_module.baseline_point(
        BaselineKind.GRADIENT_BOOSTING,
        ForecastTarget.RETURN,
        (10, 20, 5, 15),
        4,
    )
    assert adapters_module.baseline_point(
        BaselineKind.GRADIENT_BOOSTING,
        ForecastTarget.RETURN,
        (100, 200, 300, 50),
        4,
    )


def test_walk_forward_rejects_every_invalid_evaluation_shape(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    valid = {
        target: EvaluationSeries(target, _history(target, 48), seasonal_period=4)
        for target in REQUIRED_EVALUATION_TARGETS
    }
    with pytest.raises(ValueError, match="unsupported"):
        evaluation_module._validate_series(
            EvaluationSeries(ForecastTarget.MARKET_FACTOR, _history(count=48), 4), 16
        )
    with pytest.raises(ValueError, match="seasonal"):
        evaluation_module._validate_series(
            EvaluationSeries(ForecastTarget.RETURN, _history(count=48), 1), 16
        )
    with pytest.raises(ValueError, match="insufficient"):
        evaluation_module._validate_series(
            EvaluationSeries(ForecastTarget.RETURN, _history(count=16), 4), 16
        )
    duplicate = (*_history(count=47), _history(count=47)[-1])
    with pytest.raises(ValueError, match="strictly increasing"):
        evaluation_module._validate_series(
            EvaluationSeries(ForecastTarget.RETURN, duplicate, 4), 16
        )
    bad_availability = list(_history(count=48))
    bad_availability[0] = replace(bad_availability[0], available_wall_clock_utc_ns=0)
    with pytest.raises(ValueError, match="availability"):
        evaluation_module._validate_series(
            EvaluationSeries(ForecastTarget.RETURN, tuple(bad_availability), 4), 16
        )
    regressed = list(_history(count=48))
    regressed[2] = replace(
        regressed[2],
        available_wall_clock_utc_ns=regressed[1].available_wall_clock_utc_ns - 1,
    )
    with pytest.raises(ValueError, match="availability"):
        evaluation_module._validate_series(
            EvaluationSeries(ForecastTarget.RETURN, tuple(regressed), 4), 16
        )

    for minimum_history, horizon_steps, seed in (
        (3, 1, 1),
        (16, 0, 1),
        (16, 1, 0),
    ):
        with pytest.raises(ValueError, match="configuration"):
            compare_baselines_walk_forward(valid, minimum_history, horizon_steps, seed)
    mismatched = dict(valid)
    mismatched[ForecastTarget.RETURN] = EvaluationSeries(
        ForecastTarget.VOLUME, _history(ForecastTarget.VOLUME, 48), 4
    )
    with pytest.raises(ValueError, match="key and target"):
        compare_baselines_walk_forward(mismatched, 16, 1, 1)
    with pytest.raises(ValueError, match="too short"):
        compare_baselines_walk_forward(valid, 47, 2, 1)

    leaking = dict(valid)
    leak_points = list(_history(count=48))
    leak_points[1] = replace(
        leak_points[1],
        available_wall_clock_utc_ns=leak_points[-1].available_wall_clock_utc_ns + 1,
    )
    leaking[ForecastTarget.RETURN] = EvaluationSeries(
        ForecastTarget.RETURN, tuple(leak_points), 4
    )
    monkeypatch.setattr(evaluation_module, "_validate_series", lambda *_: None)
    with pytest.raises(ValueError, match="leakage"):
        compare_baselines_walk_forward(leaking, 16, 1, 1)


def test_service_configuration_and_cache_validate_every_bound(
    context: ForecastContext,
) -> None:
    valid_config = ServiceConfig(
        configuration_version=context.configuration_version,
        forecast_ttl_ns=1_000,
        maximum_forecast_age_ns=500,
        checkpoint_sha256=None,
        primary_device=AdapterDevice.CPU,
        fallback_baseline=None,
        queue_capacity=2,
        result_capacity=2,
        maximum_batch=2,
    )
    invalid_configs = (
        {"forecast_ttl_ns": 0},
        {"maximum_forecast_age_ns": 0},
        {"maximum_forecast_age_ns": 1_001},
        {"queue_capacity": 0},
        {"result_capacity": 0},
        {"maximum_batch": 0},
    )
    for changes in invalid_configs:
        with pytest.raises(ValueError, match="capacity"):
            replace(valid_config, **changes)  # type: ignore[arg-type]

    with pytest.raises(ValueError, match="bounds"):
        ForecastCache(0, 1)
    with pytest.raises(ValueError, match="bounds"):
        ForecastCache(1, 0)

    clock = ManualClock()
    service = _service(clock, ControllableAdapter())
    request = _forecast_request(context)
    service.submit(request)
    service.run_worker_once()
    forecasts = service.poll(request.request_id).forecasts
    cache = ForecastCache(1, 500)
    for invalid in (
        replace(forecasts[0], created_process_monotonic_time_ns=0),
        replace(
            forecasts[0],
            valid_until_process_monotonic_time_ns=forecasts[
                0
            ].created_process_monotonic_time_ns,
        ),
        replace(forecasts[0], contract_bytes=b""),
    ):
        with pytest.raises(ValueError, match="malformed"):
            cache.put(invalid)
    assert cache.get(forecasts[0].cache_key, 1_000) is None
    cache.put(forecasts[0])
    assert len(cache) == 1
    assert cache.get(forecasts[0].cache_key, 999) is None
    assert cache.stale_count == 1
    cache.put(forecasts[0])
    cache.put(forecasts[1])
    assert len(cache) == 1
    assert cache.get(forecasts[0].cache_key, 1_100) is None
    assert cache.get(forecasts[1].cache_key, 1_100) == forecasts[1]


def test_worker_duplicate_cache_stale_deadline_and_shutdown_paths(
    context: ForecastContext,
) -> None:
    clock = ManualClock()
    adapter = ControllableAdapter()
    service = _service(clock, adapter)
    request = _forecast_request(context)
    assert service.submit(request) is WorkerStatus.ACCEPTED
    assert service.submit(request) is WorkerStatus.INVALID_REQUEST
    service.run_worker_once()
    assert service.submit(request) is WorkerStatus.INVALID_REQUEST
    assert service.poll(request.request_id).status is WorkerStatus.COMPLETED

    cached_request = _forecast_request(context, sequence=2)
    assert service.submit(cached_request) is WorkerStatus.ACCEPTED
    service.run_worker_once()
    assert service.poll(cached_request.request_id).status is WorkerStatus.COMPLETED
    assert adapter.batch_sizes == [1]

    clock.now_ns = 1_501
    stale_request = _forecast_request(context, sequence=3, deadline_ns=3_000)
    assert service.submit(stale_request) is WorkerStatus.ACCEPTED
    service.run_worker_once()
    assert service.poll(stale_request.request_id).status is WorkerStatus.COMPLETED
    assert "forecast_cache_stale_total 2" in service.prometheus_metrics()

    deadline_service = _service(ManualClock(), ControllableAdapter())
    deadline_clock = cast("ManualClock", deadline_service._worker._clock)
    deadline_request = _forecast_request(context, deadline_ns=1_100)
    assert deadline_service.submit(deadline_request) is WorkerStatus.ACCEPTED
    deadline_clock.now_ns = 1_101
    assert deadline_service.run_worker_once() == 1
    assert (
        deadline_service.poll(deadline_request.request_id).status
        is WorkerStatus.DEADLINE_MISSED
    )
    assert deadline_service.run_worker_once() == 0

    shutdown_clock = ManualClock()
    shutdown_service = _service(shutdown_clock, ControllableAdapter())
    queued = _forecast_request(context)
    assert shutdown_service.submit(queued) is WorkerStatus.ACCEPTED
    assert shutdown_service.shutdown(999) is WorkerStatus.DEADLINE_MISSED
    assert shutdown_service.shutdown(1_000) is WorkerStatus.STOPPED
    assert shutdown_service.poll(queued.request_id).status is WorkerStatus.STOPPED
    assert shutdown_service.run_worker_once() == 0
    assert shutdown_service.poll(_identifier(999)).status is WorkerStatus.NO_RESULT


def test_worker_failures_cover_cpu_retry_fallback_and_batch_validation(
    context: ForecastContext,
) -> None:
    class DeviceFailureAdapter(ControllableAdapter):
        def forecast_batch(
            self,
            contexts: tuple[ForecastContext, ...],
            device: AdapterDevice,
            checkpoint_sha256: str | None,
        ) -> tuple[AdapterResult, ...]:
            del contexts, checkpoint_sha256
            self.devices.append(device)
            raise AdapterDeviceError("device failed")

    gpu_adapter = DeviceFailureAdapter()
    gpu_service = _service(ManualClock(), gpu_adapter)
    gpu_request = _forecast_request(context)
    gpu_service.submit(gpu_request)
    gpu_service.run_worker_once()
    assert (
        gpu_service.poll(gpu_request.request_id).status
        is WorkerStatus.FALLBACK_COMPLETED
    )
    assert gpu_adapter.devices == [AdapterDevice.GPU, AdapterDevice.CPU]

    cpu_config = ServiceConfig(
        configuration_version=context.configuration_version,
        forecast_ttl_ns=1_000,
        maximum_forecast_age_ns=500,
        checkpoint_sha256=None,
        primary_device=AdapterDevice.CPU,
        fallback_baseline=BaselineKind.LAST_VALUE,
        queue_capacity=2,
        result_capacity=2,
        maximum_batch=2,
    )
    cpu_service = TimeseriesForecastService(
        cpu_config, DeviceFailureAdapter(), ManualClock()
    )
    cpu_request = _forecast_request(context)
    cpu_service.submit(cpu_request)
    cpu_service.run_worker_once()
    assert (
        cpu_service.poll(cpu_request.request_id).status
        is WorkerStatus.FALLBACK_COMPLETED
    )

    class EmptyAdapter(ControllableAdapter):
        def forecast_batch(
            self,
            contexts: tuple[ForecastContext, ...],
            device: AdapterDevice,
            checkpoint_sha256: str | None,
        ) -> tuple[AdapterResult, ...]:
            del contexts, device, checkpoint_sha256
            return ()

    empty_service = _service(ManualClock(), EmptyAdapter(), fallback=None)
    empty_request = _forecast_request(context)
    empty_service.submit(empty_request)
    empty_service.run_worker_once()
    assert (
        empty_service.poll(empty_request.request_id).status
        is WorkerStatus.MODEL_FAILURE
    )


def test_worker_rejects_context_mismatch_and_evicts_bounded_results(
    context: ForecastContext,
) -> None:
    clock = ManualClock()
    service = _service(clock, ControllableAdapter())
    wrong_configuration = replace(
        context, configuration_version=ConfigurationVersion(_identifier(901))
    )
    assert (
        service.submit(
            ForecastRequest(GlobalEventId(_identifier(903)), wrong_configuration, 2_000)
        )
        is WorkerStatus.INVALID_REQUEST
    )
    future_built = replace(context, built_process_monotonic_time_ns=1_001)
    assert (
        service.submit(
            ForecastRequest(GlobalEventId(_identifier(905)), future_built, 2_000)
        )
        is WorkerStatus.INVALID_REQUEST
    )

    bounded_config = ServiceConfig(
        configuration_version=context.configuration_version,
        forecast_ttl_ns=1_000,
        maximum_forecast_age_ns=500,
        checkpoint_sha256=None,
        primary_device=AdapterDevice.GPU,
        fallback_baseline=None,
        queue_capacity=2,
        result_capacity=1,
        maximum_batch=2,
    )
    bounded = TimeseriesForecastService(bounded_config, ControllableAdapter(), clock)
    first = _forecast_request(context, 1)
    second = _forecast_request(context, 2)
    bounded.submit(first)
    bounded.submit(second)
    bounded.run_worker_once()
    assert bounded.poll(first.request_id).status is WorkerStatus.NO_RESULT
    assert bounded.poll(second.request_id).status is WorkerStatus.COMPLETED


def test_publisher_covers_direction_and_every_target() -> None:
    assert service_module._directional_probabilities(1)[2] == 600_000
    assert service_module._directional_probabilities(-1)[0] == 600_000
    assert service_module._directional_probabilities(0)[1] == 100_000
    for target in ForecastTarget:
        context = ContextBuilder().build(_request(target))
        clock = ManualClock()
        service = _service(clock, ControllableAdapter())
        request = _forecast_request(context, sequence=int(target))
        assert service.submit(request) is WorkerStatus.ACCEPTED
        service.run_worker_once()
        result = service.poll(request.request_id)
        assert result.status is WorkerStatus.COMPLETED
        assert all(item.target is target for item in result.forecasts)


def test_runtime_returns_model_failure_and_serve_restores_signals(
    context: ForecastContext,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    clock = ManualClock()
    failed = RoleApplication(
        ServiceRole.API,
        _service(clock, ControllableAdapter(fail_all=True), fallback=None),
        clock,
    )
    response = failed.handle("POST", "/v1/forecast", encode_context(context))
    assert response.status is HTTPStatus.SERVICE_UNAVAILABLE
    assert json.loads(response.body)["status"] == "model_failure"
    assert build_reference_application(ServiceRole.API).status().ready is True

    application = build_reference_application(ServiceRole.API, clock)
    handlers: dict[int, object] = {}

    class FakeServer:
        def __init__(self, address: tuple[str, int], handler: object) -> None:
            self.address = address
            self.handler = handler
            self.shutdown_called = False
            self.closed = False

        def shutdown(self) -> None:
            self.shutdown_called = True

        def serve_forever(self, poll_interval: float) -> None:
            assert poll_interval == 0.1
            cast("Callable[[int, object], None]", handlers[signal.SIGTERM])(
                signal.SIGTERM, None
            )

        def server_close(self) -> None:
            self.closed = True

    created: list[FakeServer] = []

    def fake_server(address: tuple[str, int], handler: object) -> FakeServer:
        server = FakeServer(address, handler)
        created.append(server)
        return server

    def fake_signal(signum: int, handler: object) -> object:
        previous = handlers.get(signum, object())
        handlers[signum] = handler
        return previous

    class InlineThread:
        def __init__(self, *, target: Callable[[], None], daemon: bool) -> None:
            self._target = target
            assert daemon is True

        def start(self) -> None:
            self._target()

    monkeypatch.setattr(runtime_module, "ThreadingHTTPServer", fake_server)
    monkeypatch.setattr(signal, "signal", fake_signal)
    monkeypatch.setattr(threading, "Thread", InlineThread)
    monkeypatch.setattr(time, "monotonic_ns", lambda: 1_500)
    runtime_module.serve(application, "127.0.0.1", 8080, allow_insecure_loopback=True)
    assert created[0].address == ("127.0.0.1", 8080)
    assert created[0].shutdown_called is True
    assert created[0].closed is True
    assert application.status().ready is False
