"""Immutable targets, contexts, and provenance for time-series forecasting."""

from __future__ import annotations

import hashlib
import json
from dataclasses import asdict, dataclass
from enum import IntEnum
from itertools import pairwise
from typing import TYPE_CHECKING, Final, cast

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

if TYPE_CHECKING:
    from collections.abc import Mapping

PPM: Final = 1_000_000
MAX_CONTEXT_BYTES: Final = 1 << 20
MAX_HISTORY_POINTS: Final = 4_096
MAX_HORIZONS: Final = 16
MAX_SCHEDULED_EVENT_FLAGS: Final = (1 << 32) - 1
MIN_HISTORY_POINTS: Final = 4
QUANTILE_COUNT: Final = 3
SECONDS_PER_DAY: Final = 86_400
IDENTIFIER_HEX_LENGTH: Final = 32
SHA256_HEX_LENGTH: Final = 64


class ForecastTarget(IntEnum):
    """Supported target series; raw price is deliberately absent."""

    RETURN = 1
    REALIZED_VOLATILITY = 2
    VOLUME = 3
    SPREAD = 4
    MARKET_FACTOR = 5
    SECTOR_FACTOR = 6


class AdapterDevice(IntEnum):
    """Explicit inference device selected outside the hot path."""

    CPU = 1
    GPU = 2


class SessionState(IntEnum):
    """Known-future market session state."""

    UNKNOWN = 0
    PRE_OPEN = 1
    OPEN = 2
    HALTED = 3
    AUCTION = 4
    CLOSED = 5


class ModelHealth(IntEnum):
    """Fail-closed model health separate from process liveness."""

    UNKNOWN = 0
    WARMING = 1
    HEALTHY = 2
    DEGRADED = 3
    FAILED = 4
    DISABLED = 5


class WorkerStatus(IntEnum):
    """Machine-readable worker outcomes."""

    ACCEPTED = 1
    QUEUE_FULL = 2
    DEADLINE_MISSED = 3
    COMPLETED = 4
    FALLBACK_COMPLETED = 5
    NO_RESULT = 6
    MODEL_FAILURE = 7
    INVALID_REQUEST = 8
    STOPPED = 9


class BaselineKind(IntEnum):
    """Deterministic comparator models."""

    LAST_VALUE = 1
    SEASONAL_NAIVE = 2
    ARIMA_COMPATIBLE = 3
    GARCH_COMPATIBLE = 4
    GRADIENT_BOOSTING = 5
    COMPACT_TEMPORAL = 6


class AdapterDeviceError(RuntimeError):
    """Raised only when the selected device cannot execute the adapter."""


class AdapterUnavailableError(RuntimeError):
    """Raised when an adapter cannot produce a valid bounded result."""


@dataclass(frozen=True, slots=True)
class TimeSeriesPoint:
    """One integer observation with event and point-in-time availability."""

    exchange_event_time_ns: int
    available_wall_clock_utc_ns: int
    value: int


@dataclass(frozen=True, slots=True)
class KnownFutureCovariate:
    """One horizon's covariates and when they became knowable."""

    horizon_ns: int
    exchange_event_time_ns: int
    known_wall_clock_utc_ns: int
    time_of_day_second: int
    session_state: SessionState
    scheduled_event_flags: int


@dataclass(frozen=True, slots=True)
class ContextBuildRequest:
    """Untrusted input to the point-in-time context builder."""

    instrument_id: InstrumentId
    session_id: SessionId
    feature_snapshot_id: FeatureSnapshotId
    configuration_version: ConfigurationVersion
    feature_definition_version: ConfigurationVersion
    source_first_global_event_id: GlobalEventId
    source_last_global_event_id: GlobalEventId
    target: ForecastTarget
    as_of_exchange_event_time_ns: int
    point_in_time_wall_clock_utc_ns: int
    built_process_monotonic_time_ns: int
    source_payload_sha256: bytes
    horizons_ns: tuple[int, ...]
    quantiles_ppm: tuple[int, int, int]
    history: tuple[TimeSeriesPoint, ...]
    future_covariates: tuple[KnownFutureCovariate, ...]


@dataclass(frozen=True, slots=True)
class ForecastContext:
    """Validated immutable model context with a canonical SHA-256 identity."""

    instrument_id: InstrumentId
    session_id: SessionId
    feature_snapshot_id: FeatureSnapshotId
    configuration_version: ConfigurationVersion
    feature_definition_version: ConfigurationVersion
    source_first_global_event_id: GlobalEventId
    source_last_global_event_id: GlobalEventId
    target: ForecastTarget
    as_of_exchange_event_time_ns: int
    point_in_time_wall_clock_utc_ns: int
    built_process_monotonic_time_ns: int
    source_payload_sha256: bytes
    horizons_ns: tuple[int, ...]
    quantiles_ppm: tuple[int, int, int]
    history: tuple[TimeSeriesPoint, ...]
    future_covariates: tuple[KnownFutureCovariate, ...]
    context_sha256: bytes


def target_bounds(target: ForecastTarget) -> tuple[int, int]:
    """Return the canonical inclusive range for one target."""
    if target in {
        ForecastTarget.RETURN,
        ForecastTarget.MARKET_FACTOR,
        ForecastTarget.SECTOR_FACTOR,
    }:
        return (-10_000_000, 10_000_000)
    if target is ForecastTarget.REALIZED_VOLATILITY:
        return (0, 10_000_000)
    if target is ForecastTarget.VOLUME:
        return (0, 1_000_000_000_000)
    if target is ForecastTarget.SPREAD:
        return (0, 1_000_000_000)
    msg = "unknown forecast target"
    raise ValueError(msg)


def _id(identifier: Identifier128) -> str:
    return identifier.hex()


def _identifier(value: object, field: str) -> Identifier128:
    if not isinstance(value, str) or len(value) != IDENTIFIER_HEX_LENGTH:
        msg = f"{field} must be a 32-character identifier"
        raise ValueError(msg)
    try:
        high = int(value[:16], 16)
        low = int(value[16:], 16)
    except ValueError as error:
        msg = f"{field} must be lowercase hexadecimal"
        raise ValueError(msg) from error
    if value != value.lower():
        msg = f"{field} must be lowercase hexadecimal"
        raise ValueError(msg)
    return Identifier128(high, low)


def _context_payload(
    context: ContextBuildRequest | ForecastContext,
) -> dict[str, object]:
    return {
        "as_of_exchange_event_time_ns": context.as_of_exchange_event_time_ns,
        "built_process_monotonic_time_ns": context.built_process_monotonic_time_ns,
        "configuration_version": _id(context.configuration_version),
        "feature_definition_version": _id(context.feature_definition_version),
        "feature_snapshot_id": _id(context.feature_snapshot_id),
        "future_covariates": [asdict(item) for item in context.future_covariates],
        "history": [asdict(item) for item in context.history],
        "horizons_ns": list(context.horizons_ns),
        "instrument_id": _id(context.instrument_id),
        "point_in_time_wall_clock_utc_ns": context.point_in_time_wall_clock_utc_ns,
        "quantiles_ppm": list(context.quantiles_ppm),
        "session_id": _id(context.session_id),
        "source_first_global_event_id": _id(context.source_first_global_event_id),
        "source_last_global_event_id": _id(context.source_last_global_event_id),
        "source_payload_sha256": context.source_payload_sha256.hex(),
        "target": context.target.name.lower(),
    }


def _canonical_bytes(payload: Mapping[str, object]) -> bytes:
    return json.dumps(
        payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("ascii")


class ContextBuilder:
    """Validate untrusted series and build immutable point-in-time context."""

    def __init__(self, minimum_history: int = 16) -> None:
        """Create a builder with an explicit bounded warm-up requirement."""
        if not MIN_HISTORY_POINTS <= minimum_history <= MAX_HISTORY_POINTS:
            msg = "minimum history is outside the supported bound"
            raise ValueError(msg)
        self._minimum_history = minimum_history

    def build(self, request: ContextBuildRequest) -> ForecastContext:
        """Validate provenance, units, history, covariates, and bounds."""
        self._validate_shape(request)
        self._validate_history(request)
        self._validate_covariates(request)

        digest = hashlib.sha256(_canonical_bytes(_context_payload(request))).digest()
        return ForecastContext(
            instrument_id=request.instrument_id,
            session_id=request.session_id,
            feature_snapshot_id=request.feature_snapshot_id,
            configuration_version=request.configuration_version,
            feature_definition_version=request.feature_definition_version,
            source_first_global_event_id=request.source_first_global_event_id,
            source_last_global_event_id=request.source_last_global_event_id,
            target=request.target,
            as_of_exchange_event_time_ns=request.as_of_exchange_event_time_ns,
            point_in_time_wall_clock_utc_ns=request.point_in_time_wall_clock_utc_ns,
            built_process_monotonic_time_ns=request.built_process_monotonic_time_ns,
            source_payload_sha256=request.source_payload_sha256,
            horizons_ns=request.horizons_ns,
            quantiles_ppm=request.quantiles_ppm,
            history=request.history,
            future_covariates=request.future_covariates,
            context_sha256=digest,
        )

    def _validate_shape(self, request: ContextBuildRequest) -> None:
        if not self._minimum_history <= len(request.history) <= MAX_HISTORY_POINTS:
            msg = "history length is outside the configured bound"
            raise ValueError(msg)
        if not 0 < len(request.horizons_ns) <= MAX_HORIZONS or any(
            value <= 0 for value in request.horizons_ns
        ):
            msg = "horizon set is empty or outside the configured bound"
            raise ValueError(msg)
        if any(
            current >= following for current, following in pairwise(request.horizons_ns)
        ):
            msg = "horizons must be strictly increasing"
            raise ValueError(msg)
        if (
            len(set(request.quantiles_ppm)) != QUANTILE_COUNT
            or not 0
            <= request.quantiles_ppm[0]
            < request.quantiles_ppm[1]
            < request.quantiles_ppm[2]
            <= PPM
        ):
            msg = "quantiles must be three distinct increasing PPM values"
            raise ValueError(msg)
        if len(request.source_payload_sha256) != hashlib.sha256().digest_size:
            msg = "source payload SHA-256 must contain exactly 32 bytes"
            raise ValueError(msg)
        if (
            request.as_of_exchange_event_time_ns <= 0
            or request.point_in_time_wall_clock_utc_ns <= 0
            or request.built_process_monotonic_time_ns <= 0
        ):
            msg = "context timestamps, including monotonic time, must be positive"
            raise ValueError(msg)

    @staticmethod
    def _validate_history(request: ContextBuildRequest) -> None:
        minimum, maximum = target_bounds(request.target)
        previous_event_time = 0
        for point in request.history:
            if point.exchange_event_time_ns <= previous_event_time:
                msg = "history exchange timestamps must be strictly increasing"
                raise ValueError(msg)
            if point.exchange_event_time_ns > request.as_of_exchange_event_time_ns:
                msg = "history contains an event after the as-of time"
                raise ValueError(msg)
            if (
                not 0
                < point.available_wall_clock_utc_ns
                <= request.point_in_time_wall_clock_utc_ns
            ):
                msg = "history violates the point-in-time availability cutoff"
                raise ValueError(msg)
            if not minimum <= point.value <= maximum:
                msg = "history value is outside the target unit range"
                raise ValueError(msg)
            previous_event_time = point.exchange_event_time_ns
        if previous_event_time != request.as_of_exchange_event_time_ns:
            msg = "as-of time must identify the final history observation"
            raise ValueError(msg)

    @staticmethod
    def _validate_covariates(request: ContextBuildRequest) -> None:
        if len(request.future_covariates) != len(request.horizons_ns):
            msg = "one future covariate is required for every horizon"
            raise ValueError(msg)
        for horizon, covariate in zip(
            request.horizons_ns, request.future_covariates, strict=True
        ):
            if covariate.horizon_ns != horizon:
                msg = "future covariate horizon does not match the request"
                raise ValueError(msg)
            if (
                covariate.exchange_event_time_ns
                != request.as_of_exchange_event_time_ns + horizon
            ):
                msg = "future timestamp does not equal as-of plus horizon"
                raise ValueError(msg)
            if (
                not 0
                < covariate.known_wall_clock_utc_ns
                <= request.point_in_time_wall_clock_utc_ns
            ):
                msg = "future covariate violates the point-in-time cutoff"
                raise ValueError(msg)
            if not 0 <= covariate.time_of_day_second < SECONDS_PER_DAY:
                msg = "time-of-day covariate is outside the session day"
                raise ValueError(msg)
            if covariate.session_state is SessionState.UNKNOWN:
                msg = "future covariate session state is unknown"
                raise ValueError(msg)
            if not 0 <= covariate.scheduled_event_flags <= MAX_SCHEDULED_EVENT_FLAGS:
                msg = "scheduled-event flags are outside uint32"
                raise ValueError(msg)


def encode_context(context: ForecastContext) -> bytes:
    """Return bounded canonical JSON including the verified context digest."""
    payload = _context_payload(context)
    payload["context_sha256"] = context.context_sha256.hex()
    encoded = _canonical_bytes(payload)
    if len(encoded) > MAX_CONTEXT_BYTES:
        msg = "encoded context exceeds the service bound"
        raise ValueError(msg)
    return encoded


def _require_mapping(value: object) -> Mapping[str, object]:
    if not isinstance(value, dict):
        msg = "context JSON root must be an object"
        raise ValueError(msg)  # noqa: TRY004 - decoder exposes one rejection type.
    return cast("Mapping[str, object]", value)


def _require_int(value: object, field: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        msg = f"{field} must be an integer"
        raise ValueError(msg)  # noqa: TRY004 - decoder exposes one rejection type.
    return value


def _require_int_list(value: object, field: str) -> tuple[int, ...]:
    if not isinstance(value, list):
        msg = f"{field} must be an array"
        raise ValueError(msg)  # noqa: TRY004 - decoder exposes one rejection type.
    return tuple(_require_int(item, field) for item in value)


def _require_list(value: object, field: str) -> list[object]:
    if not isinstance(value, list):
        msg = f"{field} must be an array"
        raise ValueError(msg)  # noqa: TRY004 - decoder exposes one rejection type.
    return cast("list[object]", value)


def decode_context(encoded: bytes) -> ForecastContext:
    """Parse bounded untrusted JSON and revalidate its canonical digest."""
    if not encoded or len(encoded) > MAX_CONTEXT_BYTES:
        msg = "encoded context size is invalid"
        raise ValueError(msg)
    try:
        payload = _require_mapping(json.loads(encoded.decode("ascii")))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        msg = "encoded context must be canonical ASCII JSON"
        raise ValueError(msg) from error
    expected_keys = {
        "as_of_exchange_event_time_ns",
        "built_process_monotonic_time_ns",
        "configuration_version",
        "context_sha256",
        "feature_definition_version",
        "feature_snapshot_id",
        "future_covariates",
        "history",
        "horizons_ns",
        "instrument_id",
        "point_in_time_wall_clock_utc_ns",
        "quantiles_ppm",
        "session_id",
        "source_first_global_event_id",
        "source_last_global_event_id",
        "source_payload_sha256",
        "target",
    }
    if set(payload) != expected_keys:
        msg = "context JSON fields do not match the schema"
        raise ValueError(msg)
    try:
        target = ForecastTarget[str(payload["target"]).upper()]
        history_value = payload["history"]
        covariates_value = payload["future_covariates"]
        history_value = _require_list(history_value, "history")
        covariates_value = _require_list(covariates_value, "future_covariates")
        history = tuple(
            TimeSeriesPoint(
                exchange_event_time_ns=_require_int(
                    _require_mapping(item)["exchange_event_time_ns"],
                    "history.exchange_event_time_ns",
                ),
                available_wall_clock_utc_ns=_require_int(
                    _require_mapping(item)["available_wall_clock_utc_ns"],
                    "history.available_wall_clock_utc_ns",
                ),
                value=_require_int(_require_mapping(item)["value"], "history.value"),
            )
            for item in history_value
        )
        covariates = tuple(
            KnownFutureCovariate(
                horizon_ns=_require_int(
                    _require_mapping(item)["horizon_ns"], "covariate.horizon_ns"
                ),
                exchange_event_time_ns=_require_int(
                    _require_mapping(item)["exchange_event_time_ns"],
                    "covariate.exchange_event_time_ns",
                ),
                known_wall_clock_utc_ns=_require_int(
                    _require_mapping(item)["known_wall_clock_utc_ns"],
                    "covariate.known_wall_clock_utc_ns",
                ),
                time_of_day_second=_require_int(
                    _require_mapping(item)["time_of_day_second"],
                    "covariate.time_of_day_second",
                ),
                session_state=SessionState(
                    _require_int(
                        _require_mapping(item)["session_state"],
                        "covariate.session_state",
                    )
                ),
                scheduled_event_flags=_require_int(
                    _require_mapping(item)["scheduled_event_flags"],
                    "covariate.scheduled_event_flags",
                ),
            )
            for item in covariates_value
        )
        source_digest = bytes.fromhex(str(payload["source_payload_sha256"]))
        context_digest = bytes.fromhex(str(payload["context_sha256"]))
    except (KeyError, TypeError, ValueError) as error:
        msg = "context JSON contains a malformed field"
        raise ValueError(msg) from error

    quantiles = _require_int_list(payload["quantiles_ppm"], "quantiles_ppm")
    if len(quantiles) != QUANTILE_COUNT:
        msg = "quantiles_ppm must contain three values"
        raise ValueError(msg)
    request = ContextBuildRequest(
        instrument_id=InstrumentId(
            _identifier(payload["instrument_id"], "instrument_id")
        ),
        session_id=SessionId(_identifier(payload["session_id"], "session_id")),
        feature_snapshot_id=FeatureSnapshotId(
            _identifier(payload["feature_snapshot_id"], "feature_snapshot_id")
        ),
        configuration_version=ConfigurationVersion(
            _identifier(payload["configuration_version"], "configuration_version")
        ),
        feature_definition_version=ConfigurationVersion(
            _identifier(
                payload["feature_definition_version"], "feature_definition_version"
            )
        ),
        source_first_global_event_id=GlobalEventId(
            _identifier(
                payload["source_first_global_event_id"],
                "source_first_global_event_id",
            )
        ),
        source_last_global_event_id=GlobalEventId(
            _identifier(
                payload["source_last_global_event_id"], "source_last_global_event_id"
            )
        ),
        target=target,
        as_of_exchange_event_time_ns=_require_int(
            payload["as_of_exchange_event_time_ns"], "as_of_exchange_event_time_ns"
        ),
        point_in_time_wall_clock_utc_ns=_require_int(
            payload["point_in_time_wall_clock_utc_ns"],
            "point_in_time_wall_clock_utc_ns",
        ),
        built_process_monotonic_time_ns=_require_int(
            payload["built_process_monotonic_time_ns"],
            "built_process_monotonic_time_ns",
        ),
        source_payload_sha256=source_digest,
        horizons_ns=_require_int_list(payload["horizons_ns"], "horizons_ns"),
        quantiles_ppm=(quantiles[0], quantiles[1], quantiles[2]),
        history=history,
        future_covariates=covariates,
    )
    context = ContextBuilder().build(request)
    if context.context_sha256 != context_digest:
        msg = "context SHA-256 does not match its canonical fields"
        raise ValueError(msg)
    return context


def derived_identifier(domain: str, *parts: bytes | str | int) -> Identifier128:
    """Derive a nonzero domain-separated identifier from immutable provenance."""
    digest = hashlib.sha256()
    digest.update(b"aegis-mx-timeseries-v1\x00")
    digest.update(domain.encode("ascii"))
    for part in parts:
        encoded = part if isinstance(part, bytes) else str(part).encode("ascii")
        digest.update(len(encoded).to_bytes(8, "big"))
        digest.update(encoded)
    raw = digest.digest()[:16]
    high = int.from_bytes(raw[:8], "big")
    low = int.from_bytes(raw[8:], "big")
    return Identifier128(high, low if high != 0 or low != 0 else 1)


def validate_checkpoint_sha256(value: str | None) -> None:
    """Reject mutable or malformed checkpoint identities."""
    if value is None:
        return
    if len(value) != SHA256_HEX_LENGTH or value != value.lower():
        msg = "checkpoint SHA-256 must be 64 lowercase hexadecimal characters"
        raise ValueError(msg)
    try:
        bytes.fromhex(value)
    except ValueError as error:
        msg = "checkpoint SHA-256 must be hexadecimal"
        raise ValueError(msg) from error


@dataclass(frozen=True, slots=True)
class ModelIdentity:
    """Versioned model identity shared by adapters and publication."""

    model_id: ModelId
    model_version: ModelVersion
