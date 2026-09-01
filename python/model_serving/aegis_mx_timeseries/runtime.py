"""Bounded HTTP runtime for the three off-hot-path service roles."""

from __future__ import annotations

import base64
import json
import signal
import sys
import threading
import time
from dataclasses import asdict, dataclass
from enum import StrEnum
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import TYPE_CHECKING, Final

from aegis_mx_intelligence import (
    ConfigurationVersion,
    GlobalEventId,
    ModelId,
    ModelVersion,
)

from aegis_mx_timeseries.adapters import ReferenceTimesFmAdapter
from aegis_mx_timeseries.domain import (
    AdapterDevice,
    BaselineKind,
    WorkerStatus,
    decode_context,
    derived_identifier,
    encode_context,
)
from aegis_mx_timeseries.service import (
    VERSION,
    ForecastRequest,
    ServiceConfig,
    ServiceStatus,
    TimeseriesForecastService,
)

if TYPE_CHECKING:
    from collections.abc import Callable

MAX_HTTP_BODY_BYTES: Final = 1 << 20
DEFAULT_REQUEST_DEADLINE_NS: Final = 5_000_000_000
DEFAULT_SHUTDOWN_GRACE_NS: Final = 10_000_000_000
MAX_TCP_PORT: Final = 65_535


class ServiceRole(StrEnum):
    """Independently deployable process role."""

    API = "timeseries-forecast-api"
    WORKER = "timeseries-forecast-worker"
    CONTEXT_BUILDER = "timeseries-context-builder"


@dataclass(frozen=True, slots=True)
class HttpResponse:
    """Pure response value used by the network adapter and unit tests."""

    status: HTTPStatus
    content_type: str
    body: bytes


def _json_response(status: HTTPStatus, payload: object) -> HttpResponse:
    return HttpResponse(
        status,
        "application/json",
        json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("ascii"),
    )


class SystemMonotonicClock:
    """Explicit system-clock adapter kept outside the forecasting core."""

    def __call__(self) -> int:
        """Sample the host monotonic clock only at the service boundary."""
        return time.monotonic_ns()


class RoleApplication:
    """Protocol-neutral request router with bounded untrusted input handling."""

    def __init__(
        self,
        role: ServiceRole,
        service: TimeseriesForecastService,
        clock: Callable[[], int],
        request_deadline_ns: int = DEFAULT_REQUEST_DEADLINE_NS,
    ) -> None:
        """Bind a process role to one core service and injected clock."""
        if request_deadline_ns <= 0:
            msg = "request deadline must be positive"
            raise ValueError(msg)
        self._role = role
        self._service = service
        self._clock = clock
        self._request_deadline_ns = request_deadline_ns

    @property
    def role(self) -> ServiceRole:
        """Return the immutable deployed role."""
        return self._role

    def status(self) -> ServiceStatus:
        """Return the common service status without exposing mutable internals."""
        return self._service.status()

    def handle(self, method: str, path: str, body: bytes = b"") -> HttpResponse:
        """Handle one already-framed request without blocking on external services."""
        if len(body) > MAX_HTTP_BODY_BYTES:
            return _json_response(
                HTTPStatus.REQUEST_ENTITY_TOO_LARGE, {"error": "body_too_large"}
            )
        if method == "GET":
            return self._handle_get(path)
        if method == "POST":
            return self._handle_post(path, body)
        return _json_response(
            HTTPStatus.METHOD_NOT_ALLOWED, {"error": "method_not_allowed"}
        )

    def _handle_get(self, path: str) -> HttpResponse:
        status = self._service.status()
        if path == "/healthz":
            return _json_response(
                HTTPStatus.OK if status.healthy else HTTPStatus.SERVICE_UNAVAILABLE,
                {
                    "healthy": status.healthy,
                    "model_health": status.model_health.name.lower(),
                },
            )
        if path == "/readyz":
            return _json_response(
                HTTPStatus.OK if status.ready else HTTPStatus.SERVICE_UNAVAILABLE,
                {"ready": status.ready},
            )
        if path == "/version":
            return _json_response(
                HTTPStatus.OK,
                {
                    "live_trading_capable": False,
                    "role": self._role.value,
                    "version": status.version,
                },
            )
        if path == "/configuration":
            return _json_response(
                HTTPStatus.OK,
                {"configuration_sha256": status.configuration_sha256},
            )
        if path == "/metrics":
            return HttpResponse(
                HTTPStatus.OK,
                "text/plain; version=0.0.4",
                self._service.prometheus_metrics().encode("ascii"),
            )
        return _json_response(HTTPStatus.NOT_FOUND, {"error": "not_found"})

    def _handle_post(self, path: str, body: bytes) -> HttpResponse:
        if path == "/v1/context" and self._role is ServiceRole.CONTEXT_BUILDER:
            return self._validate_context(body)
        if path == "/v1/forecast" and self._role in {
            ServiceRole.API,
            ServiceRole.WORKER,
        }:
            return self._forecast(body)
        return _json_response(HTTPStatus.NOT_FOUND, {"error": "not_found"})

    @staticmethod
    def _validate_context(body: bytes) -> HttpResponse:
        try:
            context = decode_context(body)
        except ValueError:
            return _json_response(HTTPStatus.BAD_REQUEST, {"error": "invalid_context"})
        return _json_response(
            HTTPStatus.OK,
            {
                "canonical_context_base64": base64.b64encode(
                    encode_context(context)
                ).decode("ascii"),
                "context_sha256": context.context_sha256.hex(),
            },
        )

    def _forecast(self, body: bytes) -> HttpResponse:
        try:
            context = decode_context(body)
        except ValueError:
            return _json_response(HTTPStatus.BAD_REQUEST, {"error": "invalid_context"})
        now_ns = self._clock()
        request_identifier = derived_identifier(
            "http-request", context.context_sha256, now_ns
        )
        request = ForecastRequest(
            request_id=GlobalEventId(request_identifier),
            context=context,
            deadline_process_monotonic_time_ns=now_ns + self._request_deadline_ns,
        )
        admission = self._service.submit(request)
        if admission is not WorkerStatus.ACCEPTED:
            return _json_response(
                HTTPStatus.SERVICE_UNAVAILABLE,
                {"status": admission.name.lower()},
            )
        self._service.run_worker_once()
        result = self._service.poll(request.request_id)
        if result.status not in {
            WorkerStatus.COMPLETED,
            WorkerStatus.FALLBACK_COMPLETED,
        }:
            return _json_response(
                HTTPStatus.SERVICE_UNAVAILABLE,
                {"status": result.status.name.lower()},
            )
        return _json_response(
            HTTPStatus.OK,
            {
                "forecasts": [
                    {
                        "contract_base64": base64.b64encode(item.contract_bytes).decode(
                            "ascii"
                        ),
                        "forecast_id": item.forecast_id.hex(),
                        "horizon_ns": item.horizon_ns,
                        "point_value": item.point_value,
                        "target": item.target.name.lower(),
                    }
                    for item in result.forecasts
                ],
                "status": result.status.name.lower(),
                "used_fallback": result.used_fallback,
            },
        )

    def shutdown(self, deadline_ns: int) -> WorkerStatus:
        """Stop admission and delegate bounded queue shutdown to the core."""
        return self._service.shutdown(deadline_ns)


def build_reference_application(
    role: ServiceRole,
    clock: Callable[[], int] | None = None,
) -> RoleApplication:
    """Build the deterministic, non-economic reference deployment."""
    selected_clock = clock if clock is not None else SystemMonotonicClock()
    configuration = ConfigurationVersion(
        derived_identifier("reference-service-configuration", VERSION)
    )
    model_id = ModelId(derived_identifier("reference-timesfm-compatible-model"))
    model_version = ModelVersion(
        derived_identifier("reference-timesfm-compatible-version", VERSION)
    )
    config = ServiceConfig(
        configuration_version=configuration,
        forecast_ttl_ns=30_000_000_000,
        maximum_forecast_age_ns=10_000_000_000,
        checkpoint_sha256=None,
        primary_device=AdapterDevice.CPU,
        fallback_baseline=BaselineKind.SEASONAL_NAIVE,
        queue_capacity=256,
        result_capacity=256,
        maximum_batch=32,
    )
    return RoleApplication(
        role,
        TimeseriesForecastService(
            config,
            ReferenceTimesFmAdapter(model_id, model_version),
            selected_clock,
        ),
        selected_clock,
    )


def make_handler(application: RoleApplication) -> type[BaseHTTPRequestHandler]:
    """Adapt the pure application to the Python standard-library HTTP server."""

    class Handler(BaseHTTPRequestHandler):
        server_version = "AegisMXTimeseries/1"

        def do_GET(self) -> None:
            self._dispatch("GET", b"")

        def do_POST(self) -> None:
            raw_length = self.headers.get("Content-Length", "")
            try:
                length = int(raw_length)
            except ValueError:
                self._write(
                    _json_response(
                        HTTPStatus.BAD_REQUEST, {"error": "invalid_content_length"}
                    )
                )
                return
            if length < 0 or length > MAX_HTTP_BODY_BYTES:
                self._write(
                    _json_response(
                        HTTPStatus.REQUEST_ENTITY_TOO_LARGE, {"error": "body_too_large"}
                    )
                )
                return
            self._dispatch("POST", self.rfile.read(length))

        def _dispatch(self, method: str, body: bytes) -> None:
            path = self.path.partition("?")[0]
            response = application.handle(method, path, body)
            self._write(response)

        def _write(self, response: HttpResponse) -> None:
            self.send_response(response.status)
            self.send_header("Content-Type", response.content_type)
            self.send_header("Content-Length", str(len(response.body)))
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self.wfile.write(response.body)

        def log_message(self, request_format: str, *args: object) -> None:
            del request_format, args
            event = {
                "event": "http_request",
                "method": self.command,
                "path": self.path.partition("?")[0],
                "role": application.role.value,
            }
            sys.stderr.write(
                json.dumps(event, sort_keys=True, separators=(",", ":")) + "\n"
            )

    return Handler


def serve(application: RoleApplication, host: str, port: int) -> None:
    """Serve until SIGINT/SIGTERM, then fail closed and stop admission."""
    if not host or not 0 < port <= MAX_TCP_PORT:
        msg = "host or port is invalid"
        raise ValueError(msg)
    server = ThreadingHTTPServer((host, port), make_handler(application))

    def request_shutdown(_signum: int, _frame: object) -> None:
        threading.Thread(target=server.shutdown, daemon=True).start()

    previous_term = signal.signal(signal.SIGTERM, request_shutdown)
    previous_interrupt = signal.signal(signal.SIGINT, request_shutdown)
    try:
        server.serve_forever(poll_interval=0.1)
    finally:
        application.shutdown(time.monotonic_ns() + DEFAULT_SHUTDOWN_GRACE_NS)
        server.server_close()
        signal.signal(signal.SIGTERM, previous_term)
        signal.signal(signal.SIGINT, previous_interrupt)


def status_payload(application: RoleApplication) -> dict[str, object]:
    """Expose a serializable startup record for structured process logging."""
    return {
        "event": "service_start",
        "role": application.role.value,
        **asdict(application.status()),
    }
