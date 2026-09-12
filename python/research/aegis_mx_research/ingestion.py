"""Provider-neutral, bounded, offline-first source ingestion."""

from __future__ import annotations

import hashlib
import json
import os
import re
import stat
import threading
import time
from collections.abc import Mapping, Sequence
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from datetime import UTC, date, datetime, timedelta
from datetime import time as datetime_time
from enum import StrEnum
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, Final, Protocol, Self, cast
from urllib.parse import parse_qsl, urlencode, urlsplit, urlunsplit

from aegis_mx_research.data_repository import (
    MAX_BYTES,
    AdmissionDecision,
    AdmissionLease,
    DataRepository,
    LineageRelation,
    ManifestLineage,
    ManifestTimeRange,
    QuotaEvidence,
    SourceManifest,
    StorageError,
    StorageRequest,
)

if TYPE_CHECKING:
    from collections.abc import Callable

INGESTION_SCHEMA_VERSION: Final = "1.0.0"
DEFAULT_INGESTION_SEED: Final = 20260831
MAX_FETCH_OBJECTS: Final = 20_000
MAX_FETCH_TICKERS: Final = 10_000
MAX_FETCH_DAYS: Final = 3_660
MAX_CHUNK_BYTES: Final = 16_777_216
MAX_CHECKPOINT_BYTES: Final = 65_536
MANIFEST_ESTIMATE_BYTES: Final = 4_096
MAX_LOCATOR_BYTES: Final = 512
MAX_CREDENTIAL_BYTES: Final = 16_384
MAX_REQUEST_RATE: Final = 1_000_000
MAX_PROVIDER_TIMEOUT_NS: Final = 300_000_000_000
MAX_RETRY_ATTEMPTS: Final = 16
MAX_CONCURRENCY: Final = 32
MAX_METADATA_TEXT_BYTES: Final = 160
MAX_REPLAY_TREE_DEPTH: Final = 16
MAX_REPLAY_TREE_ENTRIES: Final = MAX_FETCH_OBJECTS * 4
SATURDAY_INDEX: Final = 5
_IDENTIFIER = re.compile(r"^[a-z][a-z0-9_-]{2,127}$")
_SYMBOL = re.compile(r"^[A-Z][A-Z0-9]*(?:[.-][A-Z0-9]+)?$")
_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_SECRET_FIELD = re.compile(
    r"(?i)(api[-_]?key|authorization|credential|password|private[-_]?key|secret|signature|token)"
)
_REDACTED = "[REDACTED]"


class FetchErrorCode(StrEnum):
    """Stable fail-closed ingestion reason codes."""

    INVALID_REQUEST = "INVALID_REQUEST"
    UNAUTHORIZED = "UNAUTHORIZED"
    PLAN_MISMATCH = "PLAN_MISMATCH"
    PROVIDER_UNAVAILABLE = "PROVIDER_UNAVAILABLE"
    SIZE_UNKNOWN = "SIZE_UNKNOWN"
    OBJECT_TOO_LARGE = "OBJECT_TOO_LARGE"
    RETRY_EXHAUSTED = "RETRY_EXHAUSTED"
    TIMEOUT = "TIMEOUT"
    RATE_LIMITED = "RATE_LIMITED"
    PARTIAL_RESPONSE = "PARTIAL_RESPONSE"
    OBJECT_CHANGED = "OBJECT_CHANGED"
    HASH_MISMATCH = "HASH_MISMATCH"
    MALFORMED_OBJECT = "MALFORMED_OBJECT"
    DUPLICATE_OBJECT = "DUPLICATE_OBJECT"
    CHECKPOINT_CORRUPT = "CHECKPOINT_CORRUPT"
    CREDENTIAL_UNAVAILABLE = "CREDENTIAL_UNAVAILABLE"
    SHUTDOWN = "SHUTDOWN"
    IO_FAILURE = "IO_FAILURE"


class FetchStatus(StrEnum):
    """Plan, operation, and per-object outcomes."""

    PLANNED = "PLANNED"
    COMPLETED = "COMPLETED"
    ALREADY_PRESENT = "ALREADY_PRESENT"
    QUARANTINED = "QUARANTINED"
    STOPPED = "STOPPED"
    FAILED = "FAILED"
    PARTIAL = "PARTIAL"


class ProviderHealthState(StrEnum):
    """Provider readiness as observed without a remote health probe."""

    UNKNOWN = "UNKNOWN"
    HEALTHY = "HEALTHY"
    DEGRADED = "DEGRADED"
    UNAVAILABLE = "UNAVAILABLE"


class DataEntitlementState(StrEnum):
    """Authorization state independent of provider reachability."""

    UNKNOWN = "UNKNOWN"
    DISABLED = "DISABLED"
    REVIEW_REQUIRED = "REVIEW_REQUIRED"
    BLOCKED = "BLOCKED"
    AUTHORIZED = "AUTHORIZED"
    EXPIRED = "EXPIRED"
    DENIED = "DENIED"
    LOCAL_TEST_ONLY = "LOCAL_TEST_ONLY"


class ResumeMode(StrEnum):
    """How an interrupted source object may be continued."""

    VERIFIED_RANGE = "VERIFIED_RANGE"
    COMPLETE_OBJECT_RESTART = "COMPLETE_OBJECT_RESTART"


class MockFailure(StrEnum):
    """Deterministic in-process transport outcomes used by integration tests."""

    SUCCESS = "SUCCESS"
    TIMEOUT = "TIMEOUT"
    RATE_LIMIT = "RATE_LIMIT"
    PARTIAL_RESPONSE = "PARTIAL_RESPONSE"
    CHANGED_OBJECT = "CHANGED_OBJECT"
    OVERSIZED_OBJECT = "OVERSIZED_OBJECT"
    BAD_OFFSET = "BAD_OFFSET"
    EMPTY_RESPONSE = "EMPTY_RESPONSE"
    BAD_COMPLETION = "BAD_COMPLETION"
    NONRETRYABLE = "NONRETRYABLE"


class FetchContractError(RuntimeError):
    """Typed ingestion failure with a redacted, bounded message."""

    def __init__(self, code: FetchErrorCode, message: str) -> None:
        """Store a stable reason code and only a redacted bounded message."""
        super().__init__(redact_text(message)[:512])
        self.code = code


class ProviderFetchError(FetchContractError):
    """Provider failure carrying explicit retry semantics."""

    def __init__(
        self,
        code: FetchErrorCode,
        message: str,
        *,
        retryable: bool,
        retry_after_ns: int = 0,
    ) -> None:
        """Validate and retain explicit retry metadata."""
        super().__init__(code, message)
        if retry_after_ns < 0 or retry_after_ns > MAX_BYTES:
            raise FetchContractError(
                FetchErrorCode.INVALID_REQUEST,
                "retry_after_ns is outside the supported range",
            )
        self.retryable = retryable
        self.retry_after_ns = retry_after_ns


def _canonical_bytes(value: object) -> bytes:
    return json.dumps(
        value, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode("ascii")


def _sha256(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def _require_identifier(value: str, field_name: str) -> None:
    if not isinstance(value, str) or _IDENTIFIER.fullmatch(value) is None:
        raise FetchContractError(
            FetchErrorCode.INVALID_REQUEST,
            f"{field_name} must be a lowercase bounded identifier",
        )


def _require_sha256(value: str, field_name: str) -> None:
    if _SHA256.fullmatch(value) is None or value == "0" * 64:
        raise FetchContractError(
            FetchErrorCode.INVALID_REQUEST,
            f"{field_name} must be a nonzero lowercase SHA-256 digest",
        )


def _require_byte_count(value: int, field_name: str, *, positive: bool = False) -> None:
    minimum = 1 if positive else 0
    if type(value) is not int or value < minimum or value > MAX_BYTES:
        raise FetchContractError(
            FetchErrorCode.INVALID_REQUEST,
            f"{field_name} is outside the signed 64-bit byte range",
        )


def _checked_sum(values: Sequence[int]) -> int:
    total = 0
    for value in values:
        _require_byte_count(value, "byte count")
        if value > MAX_BYTES - total:
            raise FetchContractError(
                FetchErrorCode.INVALID_REQUEST,
                "signed 64-bit byte calculation overflow",
            )
        total += value
    return total


def _utc_nanoseconds(value: datetime) -> int:
    if value.tzinfo is None or value.utcoffset() != timedelta(0):
        raise FetchContractError(
            FetchErrorCode.INVALID_REQUEST, "timestamp must be explicitly UTC"
        )
    epoch = datetime(1970, 1, 1, tzinfo=UTC)
    delta = value - epoch
    return (
        delta.days * 86_400_000_000_000
        + delta.seconds * 1_000_000_000
        + delta.microseconds * 1_000
    )


def _date_range(start: date, end: date) -> tuple[date, ...]:
    days = (end - start).days + 1
    if days < 1 or days > MAX_FETCH_DAYS:
        raise FetchContractError(
            FetchErrorCode.INVALID_REQUEST,
            f"date range must contain 1 through {MAX_FETCH_DAYS} days",
        )
    return tuple(start + timedelta(days=offset) for offset in range(days))


def _safe_locator(value: str) -> str:
    if (
        not isinstance(value, str)
        or not value
        or len(value.encode("utf-8")) > MAX_LOCATOR_BYTES
        or "\x00" in value
        or "?" in value
        or "#" in value
        or "@" in value
    ):
        raise FetchContractError(
            FetchErrorCode.INVALID_REQUEST,
            "source locator must be bounded and contain no credentials or query",
        )
    return value


def redact_text(value: str, secret_values: Sequence[str] = ()) -> str:
    """Redact known secret values and credential-like URL/query fields."""
    redacted = str(value)
    for secret in secret_values:
        if secret:
            redacted = redacted.replace(secret, _REDACTED)
    redacted = re.sub(
        r"(?i)\b(api[-_]?key|authorization|credential|password|private[-_]?key|secret|signature|token)\s*[:=]\s*[^\s,&;]+",
        lambda match: f"{match.group(1)}={_REDACTED}",
        redacted,
    )
    try:
        split = urlsplit(redacted)
        netloc = split.netloc
        if "@" in netloc:
            netloc = f"{_REDACTED}@{netloc.rsplit('@', 1)[1]}"
        if split.query or netloc != split.netloc:
            query = [
                (key, _REDACTED if _SECRET_FIELD.search(key) else item)
                for key, item in parse_qsl(split.query, keep_blank_values=True)
            ]
            redacted = urlunsplit(
                (
                    split.scheme,
                    netloc,
                    split.path,
                    urlencode(query),
                    split.fragment,
                )
            )
    except ValueError:
        return _REDACTED
    return redacted


def redact_structure(value: object, secret_values: Sequence[str] = ()) -> object:
    """Return a recursively redacted copy suitable for logs and audit output."""
    if isinstance(value, Mapping):
        return {
            str(key): (
                _REDACTED
                if _SECRET_FIELD.search(str(key))
                else redact_structure(child, secret_values)
            )
            for key, child in value.items()
        }
    if isinstance(value, Sequence) and not isinstance(value, (str, bytes, bytearray)):
        return [redact_structure(item, secret_values) for item in value]
    if isinstance(value, str):
        return redact_text(value, secret_values)
    return value


class SecretValue:
    """A deliberately non-printing credential value."""

    __slots__ = ("__value",)

    def __init__(self, value: str) -> None:
        """Retain a bounded nonempty secret without printable disclosure."""
        if (
            not value
            or "\x00" in value
            or len(value.encode("utf-8")) > MAX_CREDENTIAL_BYTES
        ):
            raise FetchContractError(
                FetchErrorCode.CREDENTIAL_UNAVAILABLE,
                "credential value is absent or malformed",
            )
        self.__value = value

    def reveal(self) -> str:
        """Reveal only to a provider implementation at its trust boundary."""
        return self.__value

    def __repr__(self) -> str:
        """Return a redaction marker rather than the credential."""
        return _REDACTED

    def __str__(self) -> str:
        """Return a redaction marker rather than the credential."""
        return _REDACTED


class CredentialProvider(Protocol):
    """External credential lookup boundary."""

    def get(self, reference: str) -> SecretValue | None:
        """Return a secret without serializing it."""
        ...


@dataclass(frozen=True, slots=True)
class EnvironmentCredentialProvider:
    """Read an allowlisted environment variable without logging its value."""

    environment: Mapping[str, str] = field(default_factory=lambda: os.environ)
    allowed_prefix: str = "AEGIS_DATA_"

    def get(self, reference: str) -> SecretValue | None:
        """Resolve one non-secret environment-variable reference."""
        if (
            not reference.startswith(self.allowed_prefix)
            or re.fullmatch(r"[A-Z][A-Z0-9_]{2,127}", reference) is None
        ):
            raise FetchContractError(
                FetchErrorCode.CREDENTIAL_UNAVAILABLE,
                "credential reference is outside the allowlist",
            )
        value = self.environment.get(reference)
        return None if value is None else SecretValue(value)


@dataclass(frozen=True, slots=True)
class DataEntitlement:
    """Immutable execution authorization projected from a reviewed source policy."""

    source_id: str
    dataset_id: str
    state: DataEntitlementState = DataEntitlementState.UNKNOWN
    network_access_authorized: bool = False
    policy_sha256: str | None = None
    approval_record_sha256: str | None = None
    expires_at_utc: datetime | None = None

    def __post_init__(self) -> None:
        """Require complete evidence before representing remote authorization."""
        _require_identifier(self.source_id, "entitlement source_id")
        _require_identifier(self.dataset_id, "entitlement dataset_id")
        if not isinstance(self.state, DataEntitlementState):
            raise FetchContractError(
                FetchErrorCode.UNAUTHORIZED,
                "data entitlement state is unknown",
            )
        if self.state is DataEntitlementState.AUTHORIZED:
            if not self.network_access_authorized:
                raise FetchContractError(
                    FetchErrorCode.UNAUTHORIZED,
                    "authorized remote entitlement lacks network authority",
                )
            if self.policy_sha256 is None or self.approval_record_sha256 is None:
                raise FetchContractError(
                    FetchErrorCode.UNAUTHORIZED,
                    "authorized remote entitlement lacks immutable evidence",
                )
            _require_sha256(self.policy_sha256, "entitlement policy_sha256")
            _require_sha256(
                self.approval_record_sha256,
                "entitlement approval_record_sha256",
            )
            if self.expires_at_utc is None or self.expires_at_utc.tzinfo is None:
                raise FetchContractError(
                    FetchErrorCode.UNAUTHORIZED,
                    "authorized remote entitlement lacks a UTC expiry",
                )
            if self.expires_at_utc.utcoffset() != timedelta(0):
                raise FetchContractError(
                    FetchErrorCode.UNAUTHORIZED,
                    "entitlement expiry must be UTC",
                )
        elif self.network_access_authorized:
            raise FetchContractError(
                FetchErrorCode.UNAUTHORIZED,
                "non-authorized entitlement cannot grant network access",
            )

    def permits(self, *, remote: bool, now_utc: datetime) -> bool:
        """Return whether the exact source/dataset may execute now."""
        if not remote:
            return self.state is DataEntitlementState.LOCAL_TEST_ONLY
        return (
            self.state is DataEntitlementState.AUTHORIZED
            and self.network_access_authorized
            and self.expires_at_utc is not None
            and self.expires_at_utc > now_utc
        )

    @property
    def evidence_version(self) -> str:
        """Return the approval digest or an explicit local-only version."""
        return self.approval_record_sha256 or self.state.value.lower()

    def to_dict(self) -> dict[str, object]:
        """Serialize authorization metadata without credentials."""
        return {
            "approval_record_sha256": self.approval_record_sha256,
            "dataset_id": self.dataset_id,
            "expires_at_utc": (
                None
                if self.expires_at_utc is None
                else self.expires_at_utc.isoformat().replace("+00:00", "Z")
            ),
            "network_access_authorized": self.network_access_authorized,
            "policy_sha256": self.policy_sha256,
            "source_id": self.source_id,
            "state": self.state.value,
        }


@dataclass(frozen=True, slots=True)
class RateLimitPolicy:
    """Bounded fixed-interval provider request policy."""

    requests_per_window: int = 10
    window_ns: int = 1_000_000_000
    maximum_wait_ns: int = 60_000_000_000

    def __post_init__(self) -> None:
        """Validate rate and wait bounds."""
        for value, name in (
            (self.requests_per_window, "requests_per_window"),
            (self.window_ns, "window_ns"),
            (self.maximum_wait_ns, "maximum_wait_ns"),
        ):
            _require_byte_count(value, name, positive=True)
        if self.requests_per_window > MAX_REQUEST_RATE:
            raise FetchContractError(
                FetchErrorCode.INVALID_REQUEST, "request rate exceeds policy bound"
            )

    @property
    def minimum_interval_ns(self) -> int:
        """Return the conservative interval between request starts."""
        return max(1, self.window_ns // self.requests_per_window)

    def to_dict(self) -> dict[str, int]:
        """Return deterministic policy fields."""
        return {
            "maximum_wait_ns": self.maximum_wait_ns,
            "requests_per_window": self.requests_per_window,
            "window_ns": self.window_ns,
        }


@dataclass(frozen=True, slots=True)
class RetryPolicy:
    """Bounded exponential retry with deterministic per-object jitter."""

    maximum_attempts: int = 3
    base_delay_ns: int = 10_000_000
    maximum_delay_ns: int = 2_000_000_000
    maximum_jitter_ns: int = 5_000_000
    deterministic_seed: int = DEFAULT_INGESTION_SEED

    def __post_init__(self) -> None:
        """Validate retry count, delay bounds, and deterministic seed."""
        if not 1 <= self.maximum_attempts <= MAX_RETRY_ATTEMPTS:
            raise FetchContractError(
                FetchErrorCode.INVALID_REQUEST,
                "maximum_attempts must be between 1 and 16",
            )
        for value, name in (
            (self.base_delay_ns, "base_delay_ns"),
            (self.maximum_delay_ns, "maximum_delay_ns"),
            (self.maximum_jitter_ns, "maximum_jitter_ns"),
        ):
            _require_byte_count(value, name)
        if self.base_delay_ns > self.maximum_delay_ns:
            raise FetchContractError(
                FetchErrorCode.INVALID_REQUEST,
                "base retry delay exceeds maximum delay",
            )
        if type(self.deterministic_seed) is not int:
            raise FetchContractError(
                FetchErrorCode.INVALID_REQUEST, "retry seed must be an integer"
            )

    def delay_ns(self, attempt: int, object_id: str) -> int:
        """Derive a stable capped delay for a one-based retry attempt."""
        if attempt < 1 or attempt >= self.maximum_attempts:
            raise FetchContractError(
                FetchErrorCode.INVALID_REQUEST,
                "retry attempt is outside the configured budget",
            )
        exponential = min(
            self.maximum_delay_ns,
            self.base_delay_ns * (1 << min(attempt - 1, 62)),
        )
        digest = hashlib.sha256(
            f"{self.deterministic_seed}:{object_id}:{attempt}".encode("ascii")
        ).digest()
        jitter = (
            0
            if self.maximum_jitter_ns == 0
            else int.from_bytes(digest[:8], "big") % (self.maximum_jitter_ns + 1)
        )
        return min(self.maximum_delay_ns, exponential + jitter)

    def to_dict(self) -> dict[str, int]:
        """Return deterministic retry policy fields."""
        return {
            "base_delay_ns": self.base_delay_ns,
            "deterministic_seed": self.deterministic_seed,
            "maximum_attempts": self.maximum_attempts,
            "maximum_delay_ns": self.maximum_delay_ns,
            "maximum_jitter_ns": self.maximum_jitter_ns,
        }


@dataclass(frozen=True, slots=True)
class FetchRequest:
    """One finite provider/dataset/date/universe request."""

    request_id: str
    provider_id: str
    dataset_id: str
    start_date: date
    end_date: date
    tickers: tuple[str, ...]
    universe_snapshot_sha256: str
    execute: bool = False
    maximum_object_bytes: int = 50_000_000
    maximum_total_bytes: int = 10_000_000_000
    maximum_concurrency: int = 2
    chunk_bytes: int = 1_048_576
    request_timeout_ns: int = 30_000_000_000
    deterministic_seed: int = DEFAULT_INGESTION_SEED

    def __post_init__(self) -> None:
        """Validate finite filters and resource limits."""
        for value, name in (
            (self.request_id, "request_id"),
            (self.provider_id, "provider_id"),
            (self.dataset_id, "dataset_id"),
        ):
            _require_identifier(value, name)
        _date_range(self.start_date, self.end_date)
        if not self.tickers or len(self.tickers) > MAX_FETCH_TICKERS:
            raise FetchContractError(
                FetchErrorCode.INVALID_REQUEST, "ticker filter is empty or too large"
            )
        if len(self.tickers) != len(set(self.tickers)):
            raise FetchContractError(
                FetchErrorCode.INVALID_REQUEST, "ticker filter contains duplicates"
            )
        if any(_SYMBOL.fullmatch(ticker) is None for ticker in self.tickers):
            raise FetchContractError(
                FetchErrorCode.INVALID_REQUEST, "ticker filter is malformed"
            )
        _require_sha256(self.universe_snapshot_sha256, "universe_snapshot_sha256")
        _require_byte_count(
            self.maximum_object_bytes, "maximum_object_bytes", positive=True
        )
        _require_byte_count(
            self.maximum_total_bytes, "maximum_total_bytes", positive=True
        )
        _require_byte_count(self.chunk_bytes, "chunk_bytes", positive=True)
        _require_byte_count(
            self.request_timeout_ns, "request_timeout_ns", positive=True
        )
        if self.request_timeout_ns > MAX_PROVIDER_TIMEOUT_NS:
            raise FetchContractError(
                FetchErrorCode.INVALID_REQUEST,
                "request_timeout_ns exceeds the implementation bound",
            )
        if self.chunk_bytes > min(MAX_CHUNK_BYTES, self.maximum_object_bytes):
            raise FetchContractError(
                FetchErrorCode.INVALID_REQUEST,
                "chunk_bytes exceeds the object or implementation bound",
            )
        if not 1 <= self.maximum_concurrency <= MAX_CONCURRENCY:
            raise FetchContractError(
                FetchErrorCode.INVALID_REQUEST,
                "maximum_concurrency must be between 1 and 32",
            )
        if type(self.deterministic_seed) is not int:
            raise FetchContractError(
                FetchErrorCode.INVALID_REQUEST, "request seed must be an integer"
            )

    def to_dict(self) -> dict[str, object]:
        """Return the immutable request representation."""
        return {
            "chunk_bytes_decimal": self.chunk_bytes,
            "dataset_id": self.dataset_id,
            "deterministic_seed": self.deterministic_seed,
            "end_date": self.end_date.isoformat(),
            "execute": self.execute,
            "maximum_concurrency": self.maximum_concurrency,
            "maximum_object_bytes_decimal": self.maximum_object_bytes,
            "maximum_total_bytes_decimal": self.maximum_total_bytes,
            "provider_id": self.provider_id,
            "request_id": self.request_id,
            "request_timeout_ns": self.request_timeout_ns,
            "start_date": self.start_date.isoformat(),
            "tickers": list(self.tickers),
            "universe_snapshot_sha256": self.universe_snapshot_sha256,
        }


@dataclass(frozen=True, slots=True)
class SourceObject:
    """Immutable planned source object with no credential-bearing locator."""

    provider_id: str
    dataset_id: str
    source_version: str
    locator: str
    version_id: str
    coverage_date: date
    tickers: tuple[str, ...]
    estimated_size_bytes: int
    expected_sha256: str | None
    record_count: int
    schema_name: str
    schema_version: str
    content_format: str
    times: ManifestTimeRange
    resume_mode: ResumeMode = ResumeMode.VERIFIED_RANGE
    lineage: ManifestLineage = field(default_factory=ManifestLineage)

    def __post_init__(self) -> None:
        """Validate identity, provenance, size, and ticker scope."""
        _require_identifier(self.provider_id, "source provider_id")
        _require_identifier(self.dataset_id, "source dataset_id")
        _safe_locator(self.locator)
        for value, name in (
            (self.source_version, "source_version"),
            (self.version_id, "version_id"),
            (self.schema_name, "schema_name"),
            (self.schema_version, "schema_version"),
            (self.content_format, "content_format"),
        ):
            if (
                not value
                or "\x00" in value
                or len(value.encode("utf-8")) > MAX_METADATA_TEXT_BYTES
            ):
                raise FetchContractError(
                    FetchErrorCode.INVALID_REQUEST, f"{name} is malformed"
                )
        if not self.tickers or len(self.tickers) != len(set(self.tickers)):
            raise FetchContractError(
                FetchErrorCode.INVALID_REQUEST,
                "source-object ticker coverage must be nonempty and unique",
            )
        if any(_SYMBOL.fullmatch(ticker) is None for ticker in self.tickers):
            raise FetchContractError(
                FetchErrorCode.INVALID_REQUEST,
                "source-object ticker coverage is malformed",
            )
        _require_byte_count(
            self.estimated_size_bytes, "estimated_size_bytes", positive=True
        )
        _require_byte_count(self.record_count, "record_count")
        if self.expected_sha256 is not None:
            _require_sha256(self.expected_sha256, "expected_sha256")
        if not isinstance(self.resume_mode, ResumeMode) or not isinstance(
            self.lineage, ManifestLineage
        ):
            raise FetchContractError(
                FetchErrorCode.INVALID_REQUEST,
                "source-object recovery metadata is malformed",
            )

    @property
    def object_id(self) -> str:
        """Return a deterministic request-independent source identity."""
        return f"object-{_sha256(_canonical_bytes(self.to_dict()))}"

    def to_dict(self) -> dict[str, object]:
        """Return the deterministic object representation."""
        return {
            "content_format": self.content_format,
            "coverage_date": self.coverage_date.isoformat(),
            "dataset_id": self.dataset_id,
            "estimated_size_bytes_decimal": self.estimated_size_bytes,
            "expected_sha256": self.expected_sha256,
            "lineage": self.lineage.to_dict(),
            "locator": self.locator,
            "provider_id": self.provider_id,
            "record_count": self.record_count,
            "resume_mode": self.resume_mode.value,
            "schema_name": self.schema_name,
            "schema_version": self.schema_version,
            "source_version": self.source_version,
            "tickers": list(self.tickers),
            "times": self.times.to_dict(),
            "version_id": self.version_id,
        }


@dataclass(frozen=True, slots=True)
class ProviderChunk:
    """One bounded provider response body range."""

    offset: int
    data: bytes
    total_size_bytes: int
    version_id: str
    complete: bool


class DataProvider(Protocol):
    """Provider-neutral boundary; planning must not perform remote I/O."""

    @property
    def provider_id(self) -> str:
        """Return the stable provider identity."""
        ...

    @property
    def dataset_id(self) -> str:
        """Return the exact dataset identity."""
        ...

    @property
    def remote(self) -> bool:
        """Return whether fetching can cross a network boundary."""
        ...

    @property
    def credential_reference(self) -> str | None:
        """Return only the external secret reference, never its value."""
        ...

    def health(self) -> ProviderHealthState:
        """Return locally known provider health without probing a network."""
        ...

    def plan(self, request: FetchRequest) -> tuple[SourceObject, ...]:
        """Build a finite local fetch plan."""
        ...

    def fetch_range(
        self,
        source: SourceObject,
        *,
        offset: int,
        maximum_bytes: int,
        timeout_ns: int,
        credential: SecretValue | None,
    ) -> ProviderChunk:
        """Retrieve one bounded range."""
        ...

    def validate_payload(self, source: SourceObject, payload: bytes) -> int:
        """Validate complete object bytes and return the record count."""
        ...


@dataclass(frozen=True, slots=True)
class DownloadCheckpoint:
    """Durable resumable state authenticated by a canonical hash envelope."""

    plan_id: str
    request_id: str
    object_id: str
    provider_id: str
    dataset_id: str
    locator_sha256: str
    version_id: str
    expected_size_bytes: int
    completed_bytes: int
    prefix_sha256: str
    attempts: int
    sequence: int
    schema_version: str = INGESTION_SCHEMA_VERSION

    def __post_init__(self) -> None:
        """Validate checkpoint identity and monotonic progress."""
        _require_sha256(self.plan_id, "checkpoint plan_id")
        _require_identifier(self.request_id, "checkpoint request_id")
        if not self.object_id.startswith("object-"):
            raise FetchContractError(
                FetchErrorCode.CHECKPOINT_CORRUPT, "checkpoint object_id is malformed"
            )
        _require_sha256(self.object_id.removeprefix("object-"), "object_id digest")
        _require_identifier(self.provider_id, "checkpoint provider_id")
        _require_identifier(self.dataset_id, "checkpoint dataset_id")
        _require_sha256(self.locator_sha256, "checkpoint locator_sha256")
        _require_sha256(self.prefix_sha256, "checkpoint prefix_sha256")
        for value, name in (
            (self.expected_size_bytes, "expected_size_bytes"),
            (self.completed_bytes, "completed_bytes"),
            (self.attempts, "attempts"),
            (self.sequence, "sequence"),
        ):
            _require_byte_count(value, name)
        if self.completed_bytes > self.expected_size_bytes:
            raise FetchContractError(
                FetchErrorCode.CHECKPOINT_CORRUPT,
                "checkpoint completion exceeds expected size",
            )
        if (
            not self.version_id
            or len(self.version_id.encode("utf-8")) > MAX_METADATA_TEXT_BYTES
        ):
            raise FetchContractError(
                FetchErrorCode.CHECKPOINT_CORRUPT,
                "checkpoint version_id is malformed",
            )
        if self.schema_version != INGESTION_SCHEMA_VERSION:
            raise FetchContractError(
                FetchErrorCode.CHECKPOINT_CORRUPT,
                "checkpoint schema version is unsupported",
            )

    def to_payload(self) -> dict[str, object]:
        """Return checkpoint fields excluding its redundant digest."""
        return {
            "attempts": self.attempts,
            "completed_bytes_decimal": self.completed_bytes,
            "dataset_id": self.dataset_id,
            "expected_size_bytes_decimal": self.expected_size_bytes,
            "locator_sha256": self.locator_sha256,
            "object_id": self.object_id,
            "plan_id": self.plan_id,
            "prefix_sha256": self.prefix_sha256,
            "provider_id": self.provider_id,
            "request_id": self.request_id,
            "schema_version": self.schema_version,
            "sequence": self.sequence,
            "version_id": self.version_id,
        }

    def encode(self) -> bytes:
        """Encode canonical checkpoint bytes with redundant SHA-256."""
        payload = self.to_payload()
        return (
            _canonical_bytes(
                {"checkpoint_sha256": _sha256(_canonical_bytes(payload)), **payload}
            )
            + b"\n"
        )

    @classmethod
    def decode(cls, encoded: bytes) -> Self:
        """Strictly decode and authenticate untrusted checkpoint bytes."""
        if not encoded or len(encoded) > MAX_CHECKPOINT_BYTES:
            raise FetchContractError(
                FetchErrorCode.CHECKPOINT_CORRUPT,
                "checkpoint size is outside its bound",
            )
        try:
            value = json.loads(encoded)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise FetchContractError(
                FetchErrorCode.CHECKPOINT_CORRUPT, "checkpoint is not valid JSON"
            ) from error
        if not isinstance(value, dict):
            raise FetchContractError(
                FetchErrorCode.CHECKPOINT_CORRUPT,
                "checkpoint must be a JSON object",
            )
        expected_fields = {
            "attempts",
            "checkpoint_sha256",
            "completed_bytes_decimal",
            "dataset_id",
            "expected_size_bytes_decimal",
            "locator_sha256",
            "object_id",
            "plan_id",
            "prefix_sha256",
            "provider_id",
            "request_id",
            "schema_version",
            "sequence",
            "version_id",
        }
        if set(value) != expected_fields:
            raise FetchContractError(
                FetchErrorCode.CHECKPOINT_CORRUPT,
                "checkpoint fields do not match the contract",
            )
        supplied_hash = value.pop("checkpoint_sha256")
        if supplied_hash != _sha256(_canonical_bytes(value)):
            raise FetchContractError(
                FetchErrorCode.CHECKPOINT_CORRUPT,
                "checkpoint SHA-256 does not match its payload",
            )
        try:
            return cls(
                plan_id=cast("str", value["plan_id"]),
                request_id=cast("str", value["request_id"]),
                object_id=cast("str", value["object_id"]),
                provider_id=cast("str", value["provider_id"]),
                dataset_id=cast("str", value["dataset_id"]),
                locator_sha256=cast("str", value["locator_sha256"]),
                version_id=cast("str", value["version_id"]),
                expected_size_bytes=cast("int", value["expected_size_bytes_decimal"]),
                completed_bytes=cast("int", value["completed_bytes_decimal"]),
                prefix_sha256=cast("str", value["prefix_sha256"]),
                attempts=cast("int", value["attempts"]),
                sequence=cast("int", value["sequence"]),
                schema_version=cast("str", value["schema_version"]),
            )
        except FetchContractError:
            raise
        except (AttributeError, TypeError, ValueError) as error:
            raise FetchContractError(
                FetchErrorCode.CHECKPOINT_CORRUPT,
                "checkpoint field types are invalid",
            ) from error


@dataclass(frozen=True, slots=True)
class FetchPlan:
    """Immutable dry-run output with storage and authorization evidence."""

    request: FetchRequest
    objects: tuple[SourceObject, ...]
    duplicate_object_count: int
    total_estimated_bytes: int
    storage_request: StorageRequest
    admission: AdmissionDecision
    rate_limit: RateLimitPolicy
    retry: RetryPolicy
    provider_health: ProviderHealthState
    entitlement: DataEntitlement

    @property
    def plan_id(self) -> str:
        """Hash deterministic requested work, excluding variable admission state."""
        return _sha256(
            _canonical_bytes(
                {
                    "entitlement": self.entitlement.to_dict(),
                    "objects": [item.to_dict() for item in self.objects],
                    "rate_limit": self.rate_limit.to_dict(),
                    "request": self.request.to_dict(),
                    "retry": self.retry.to_dict(),
                    "schema_version": INGESTION_SCHEMA_VERSION,
                    "storage_request": self.storage_request.to_dict(),
                }
            )
        )

    @property
    def dry_run(self) -> bool:
        """Return whether execution authority was deliberately omitted."""
        return not self.request.execute

    def to_dict(self) -> dict[str, object]:
        """Return plan and admission evidence without performing network I/O."""
        return {
            "admission": self.admission.to_dict(),
            "dry_run": self.dry_run,
            "duplicate_object_count": self.duplicate_object_count,
            "entitlement": self.entitlement.to_dict(),
            "network_access_performed": False,
            "objects": [
                {"object_id": item.object_id, **item.to_dict()} for item in self.objects
            ],
            "plan_id": self.plan_id,
            "provider_health": self.provider_health.value,
            "rate_limit": self.rate_limit.to_dict(),
            "request": self.request.to_dict(),
            "retry": self.retry.to_dict(),
            "schema_version": INGESTION_SCHEMA_VERSION,
            "total_estimated_bytes_decimal": self.total_estimated_bytes,
        }


@dataclass(frozen=True, slots=True)
class ObjectFetchResult:
    """One source object's terminal outcome."""

    object_id: str
    status: FetchStatus
    bytes_received: int
    attempts: int
    retry_delays_ns: tuple[int, ...] = ()
    object_sha256: str | None = None
    storage_path: str | None = None
    manifest_id: str | None = None
    checkpoint_path: str | None = None
    quarantine_path: str | None = None
    error_code: FetchErrorCode | None = None
    error_message: str | None = None

    def to_dict(self) -> dict[str, object]:
        """Return a recursively redacted per-object result."""
        return cast(
            "dict[str, object]",
            redact_structure(
                {
                    "attempts": self.attempts,
                    "bytes_received_decimal": self.bytes_received,
                    "checkpoint_path": self.checkpoint_path,
                    "error_code": (
                        None if self.error_code is None else self.error_code.value
                    ),
                    "error_message": self.error_message,
                    "manifest_id": self.manifest_id,
                    "object_id": self.object_id,
                    "object_sha256": self.object_sha256,
                    "quarantine_path": self.quarantine_path,
                    "retry_delays_ns": list(self.retry_delays_ns),
                    "status": self.status.value,
                    "storage_path": self.storage_path,
                }
            ),
        )


@dataclass(frozen=True, slots=True)
class FetchResult:
    """Machine-readable bounded execution result."""

    plan_id: str
    request_id: str
    status: FetchStatus
    objects: tuple[ObjectFetchResult, ...]
    network_access_performed: bool
    deterministic_seed: int

    @property
    def bytes_received(self) -> int:
        """Return the bounded sum of bytes written or quarantined."""
        return sum(item.bytes_received for item in self.objects)

    def to_dict(self) -> dict[str, object]:
        """Return deterministic execution evidence."""
        return {
            "bytes_received_decimal": self.bytes_received,
            "deterministic_seed": self.deterministic_seed,
            "network_access_performed": self.network_access_performed,
            "objects": [item.to_dict() for item in self.objects],
            "plan_id": self.plan_id,
            "request_id": self.request_id,
            "schema_version": INGESTION_SCHEMA_VERSION,
            "status": self.status.value,
        }


class ShutdownSignal:
    """Thread-safe cooperative stop request shared by bounded workers."""

    def __init__(self) -> None:
        """Create an unset cooperative shutdown signal."""
        self._event = threading.Event()

    def request(self) -> None:
        """Request shutdown idempotently."""
        self._event.set()

    @property
    def requested(self) -> bool:
        """Return whether shutdown has been requested."""
        return self._event.is_set()


class _RateLimiter:
    def __init__(
        self,
        policy: RateLimitPolicy,
        *,
        monotonic_ns: Callable[[], int],
        sleep: Callable[[float], None],
    ) -> None:
        self._policy = policy
        self._monotonic_ns = monotonic_ns
        self._sleep = sleep
        self._next_ns = 0
        self._lock = threading.Lock()

    def acquire(self) -> None:
        with self._lock:
            now = self._monotonic_ns()
            wait_ns = max(self._next_ns - now, 0)
            if wait_ns > self._policy.maximum_wait_ns:
                raise FetchContractError(
                    FetchErrorCode.RATE_LIMITED,
                    "rate-limit wait exceeds the configured bound",
                )
            if wait_ns:
                self._sleep(wait_ns / 1_000_000_000)
                now = self._monotonic_ns()
            self._next_ns = max(now, self._next_ns) + self._policy.minimum_interval_ns


def _fixture_time_range(coverage_date: date) -> ManifestTimeRange:
    start = _utc_nanoseconds(
        datetime.combine(coverage_date, datetime_time(14, 30), tzinfo=UTC)
    )
    end = start + 389 * 60_000_000_000
    return ManifestTimeRange(
        event_time_min_ns=start,
        event_time_max_ns=end,
        publication_time_min_ns=start,
        publication_time_max_ns=end,
        receive_time_min_ns=start,
        receive_time_max_ns=end,
        processing_time_min_ns=start,
        processing_time_max_ns=end,
        revision_time_min_ns=start,
        revision_time_max_ns=end,
    )


def _synthetic_payload(ticker: str, session: date, seed: int) -> bytes:
    state = int.from_bytes(
        hashlib.sha256(f"{seed}:{ticker}:{session.isoformat()}".encode()).digest()[:8],
        "big",
    )
    price = 5_000 + state % 50_000
    start_ns = _fixture_time_range(session).event_time_min_ns
    lines: list[bytes] = []
    for minute in range(390):
        state = (state * 6_364_136_223_846_793_005 + 1) & ((1 << 64) - 1)
        change = int((state >> 32) % 7) - 3
        opening = price
        closing = max(1, opening + change)
        high = max(opening, closing) + int((state >> 40) % 3)
        low = max(1, min(opening, closing) - int((state >> 44) % 3))
        volume = 100 + int((state >> 16) % 9_901)
        line = {
            "close_ticks": closing,
            "event_time_ns": start_ns + minute * 60_000_000_000,
            "high_ticks": high,
            "low_ticks": low,
            "open_ticks": opening,
            "quantity_units": volume,
            "symbol": ticker,
        }
        lines.append(_canonical_bytes(line) + b"\n")
        price = closing
    return b"".join(lines)


@dataclass(frozen=True, slots=True)
class SyntheticMinuteBarProvider:
    """Deterministic local minute-bar fixture provider for infrastructure tests."""

    dataset_id: str = "synthetic-minute-bars"
    provider_id: str = "synthetic-minute-bars"
    remote: bool = False
    credential_reference: str | None = None

    def __post_init__(self) -> None:
        """Validate the synthetic dataset identity."""
        _require_identifier(self.dataset_id, "synthetic dataset_id")

    def health(self) -> ProviderHealthState:
        """Report local deterministic generation readiness."""
        return ProviderHealthState.HEALTHY

    def plan(self, request: FetchRequest) -> tuple[SourceObject, ...]:
        """Generate exact deterministic object metadata without persisting bytes."""
        if (
            request.provider_id != self.provider_id
            or request.dataset_id != self.dataset_id
        ):
            raise FetchContractError(
                FetchErrorCode.PLAN_MISMATCH,
                "request does not match the synthetic provider",
            )
        objects: list[SourceObject] = []
        for session in _date_range(request.start_date, request.end_date):
            if session.weekday() >= SATURDAY_INDEX:
                continue
            for ticker in request.tickers:
                payload = _synthetic_payload(
                    ticker, session, request.deterministic_seed
                )
                objects.append(
                    SourceObject(
                        provider_id=self.provider_id,
                        dataset_id=self.dataset_id,
                        source_version="synthetic-minute-v1",
                        locator=f"{ticker}/{session.isoformat()}.jsonl",
                        version_id=f"seed-{request.deterministic_seed}",
                        coverage_date=session,
                        tickers=(ticker,),
                        estimated_size_bytes=len(payload),
                        expected_sha256=_sha256(payload),
                        record_count=390,
                        schema_name="synthetic-minute-bars-jsonl",
                        schema_version="1.0.0",
                        content_format="application/x-ndjson",
                        times=_fixture_time_range(session),
                    )
                )
        return tuple(objects)

    def fetch_range(
        self,
        source: SourceObject,
        *,
        offset: int,
        maximum_bytes: int,
        timeout_ns: int,
        credential: SecretValue | None,
    ) -> ProviderChunk:
        """Generate and return one bounded deterministic byte range."""
        del timeout_ns
        if credential is not None:
            raise FetchContractError(
                FetchErrorCode.INVALID_REQUEST,
                "synthetic provider does not accept a credential",
            )
        ticker = source.tickers[0]
        seed = int(source.version_id.removeprefix("seed-"))
        payload = _synthetic_payload(ticker, source.coverage_date, seed)
        body = payload[offset : offset + maximum_bytes]
        return ProviderChunk(
            offset=offset,
            data=body,
            total_size_bytes=len(payload),
            version_id=source.version_id,
            complete=offset + len(body) == len(payload),
        )

    def validate_payload(self, source: SourceObject, payload: bytes) -> int:
        """Validate JSON lines, ticker scope, and expected record count."""
        lines = payload.splitlines()
        if len(lines) != source.record_count:
            raise FetchContractError(
                FetchErrorCode.MALFORMED_OBJECT,
                "synthetic object has an unexpected record count",
            )
        for line in lines:
            try:
                record = json.loads(line)
            except json.JSONDecodeError as error:
                raise FetchContractError(
                    FetchErrorCode.MALFORMED_OBJECT,
                    "synthetic object contains malformed JSON",
                ) from error
            if record.get("symbol") not in source.tickers:
                raise FetchContractError(
                    FetchErrorCode.MALFORMED_OBJECT,
                    "synthetic object contains an out-of-scope ticker",
                )
        return len(lines)


def _lineage_from_dict(value: object) -> ManifestLineage:
    if not isinstance(value, dict) or set(value) != {
        "parent_manifest_id",
        "relation",
        "reason",
    }:
        raise FetchContractError(
            FetchErrorCode.MALFORMED_OBJECT,
            "filesystem replay lineage is malformed",
        )
    try:
        return ManifestLineage(
            relation=LineageRelation(cast("str", value["relation"])),
            parent_manifest_id=cast("str | None", value["parent_manifest_id"]),
            reason=cast("str | None", value["reason"]),
        )
    except (ValueError, StorageError) as error:
        raise FetchContractError(
            FetchErrorCode.MALFORMED_OBJECT,
            "filesystem replay lineage is invalid",
        ) from error


def _times_from_dict(value: object) -> ManifestTimeRange:
    if not isinstance(value, dict):
        raise FetchContractError(
            FetchErrorCode.MALFORMED_OBJECT,
            "filesystem replay time range is malformed",
        )
    expected = set(_fixture_time_range(date(2000, 1, 3)).to_dict())
    if set(value) != expected:
        raise FetchContractError(
            FetchErrorCode.MALFORMED_OBJECT,
            "filesystem replay time fields do not match the contract",
        )
    try:
        return ManifestTimeRange(**cast("dict[str, int]", value))
    except (StorageError, TypeError) as error:
        raise FetchContractError(
            FetchErrorCode.MALFORMED_OBJECT,
            "filesystem replay time range is invalid",
        ) from error


@dataclass(frozen=True, slots=True)
class FilesystemReplayProvider:
    """Replay explicitly manifested local fixture objects without network access."""

    root: Path
    dataset_id: str
    provider_id: str = "filesystem-replay"
    remote: bool = False
    credential_reference: str | None = None

    def __post_init__(self) -> None:
        """Require an absolute non-symlink replay root."""
        _require_identifier(self.dataset_id, "filesystem dataset_id")
        if not self.root.is_absolute() or self.root.is_symlink():
            raise FetchContractError(
                FetchErrorCode.INVALID_REQUEST,
                "filesystem replay root must be an absolute non-symlink directory",
            )

    def health(self) -> ProviderHealthState:
        """Report whether the local fixture directory is available."""
        return (
            ProviderHealthState.HEALTHY
            if self.root.is_dir() and not self.root.is_symlink()
            else ProviderHealthState.UNAVAILABLE
        )

    def _metadata_paths(self) -> tuple[Path, ...]:
        paths: list[Path] = []
        entries_seen = 0

        def walk(directory: Path, depth: int) -> None:
            nonlocal entries_seen
            if depth > MAX_REPLAY_TREE_DEPTH:
                raise FetchContractError(
                    FetchErrorCode.MALFORMED_OBJECT,
                    "filesystem replay tree exceeds its depth bound",
                )
            entries: list[os.DirEntry[str]] = []
            try:
                with os.scandir(directory) as iterator:
                    for entry in iterator:
                        entries_seen += 1
                        if entries_seen > MAX_REPLAY_TREE_ENTRIES:
                            raise FetchContractError(
                                FetchErrorCode.OBJECT_TOO_LARGE,
                                "filesystem replay tree exceeds its entry bound",
                            )
                        entries.append(entry)
                for entry in sorted(entries, key=lambda item: item.name):
                    status = entry.stat(follow_symlinks=False)
                    if stat.S_ISLNK(status.st_mode):
                        raise FetchContractError(
                            FetchErrorCode.MALFORMED_OBJECT,
                            "filesystem replay tree contains a symlink",
                        )
                    path = Path(entry.path)
                    if stat.S_ISDIR(status.st_mode):
                        walk(path, depth + 1)
                    elif stat.S_ISREG(status.st_mode) and path.name.endswith(
                        ".source.json"
                    ):
                        paths.append(path)
                    elif not stat.S_ISREG(status.st_mode):
                        raise FetchContractError(
                            FetchErrorCode.MALFORMED_OBJECT,
                            "filesystem replay tree contains a special file",
                        )
            except FetchContractError:
                raise
            except OSError as error:
                raise FetchContractError(
                    FetchErrorCode.MALFORMED_OBJECT,
                    "filesystem replay tree could not be inspected safely",
                ) from error

        walk(self.root, 0)
        return tuple(paths)

    def _decode_metadata(self, path: Path) -> SourceObject:
        payload = self._read_metadata(path)
        try:
            value = json.loads(payload)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise FetchContractError(
                FetchErrorCode.MALFORMED_OBJECT,
                "filesystem replay metadata is not valid JSON",
            ) from error
        expected = {
            "content_format",
            "coverage_date",
            "dataset_id",
            "estimated_size_bytes_decimal",
            "expected_sha256",
            "lineage",
            "locator",
            "record_count",
            "resume_mode",
            "schema_name",
            "schema_version",
            "source_version",
            "tickers",
            "times",
            "version_id",
        }
        if not isinstance(value, dict) or set(value) != expected:
            raise FetchContractError(
                FetchErrorCode.MALFORMED_OBJECT,
                "filesystem replay metadata fields do not match the contract",
            )
        try:
            source = SourceObject(
                provider_id=self.provider_id,
                dataset_id=cast("str", value["dataset_id"]),
                source_version=cast("str", value["source_version"]),
                locator=cast("str", value["locator"]),
                version_id=cast("str", value["version_id"]),
                coverage_date=date.fromisoformat(cast("str", value["coverage_date"])),
                tickers=tuple(cast("list[str]", value["tickers"])),
                estimated_size_bytes=cast("int", value["estimated_size_bytes_decimal"]),
                expected_sha256=cast("str", value["expected_sha256"]),
                record_count=cast("int", value["record_count"]),
                schema_name=cast("str", value["schema_name"]),
                schema_version=cast("str", value["schema_version"]),
                content_format=cast("str", value["content_format"]),
                times=_times_from_dict(value["times"]),
                resume_mode=ResumeMode(cast("str", value["resume_mode"])),
                lineage=_lineage_from_dict(value["lineage"]),
            )
        except (FetchContractError, TypeError, ValueError) as error:
            if isinstance(error, FetchContractError):
                raise
            raise FetchContractError(
                FetchErrorCode.MALFORMED_OBJECT,
                "filesystem replay metadata values are invalid",
            ) from error
        data_path = self._data_path(source.locator)
        status = os.lstat(data_path)
        if (
            not stat.S_ISREG(status.st_mode)
            or status.st_nlink != 1
            or status.st_size != source.estimated_size_bytes
        ):
            raise FetchContractError(
                FetchErrorCode.MALFORMED_OBJECT,
                "filesystem replay object identity or size is invalid",
            )
        return source

    @staticmethod
    def _read_metadata(path: Path) -> bytes:
        try:
            status = os.lstat(path)
            if (
                not stat.S_ISREG(status.st_mode)
                or status.st_nlink != 1
                or status.st_size > MAX_CHECKPOINT_BYTES
            ):
                raise FetchContractError(
                    FetchErrorCode.MALFORMED_OBJECT,
                    "filesystem replay metadata failed its size bound "
                    "or identity check",
                )
            descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
            try:
                before = os.fstat(descriptor)
                payload = os.read(descriptor, before.st_size + 1)
                after = os.fstat(descriptor)
            finally:
                os.close(descriptor)
        except FetchContractError:
            raise
        except OSError as error:
            raise FetchContractError(
                FetchErrorCode.MALFORMED_OBJECT,
                "filesystem replay metadata could not be read safely",
            ) from error
        if len(payload) != before.st_size or (
            before.st_dev,
            before.st_ino,
            before.st_size,
            before.st_mtime_ns,
        ) != (
            after.st_dev,
            after.st_ino,
            after.st_size,
            after.st_mtime_ns,
        ):
            raise FetchContractError(
                FetchErrorCode.MALFORMED_OBJECT,
                "filesystem replay metadata changed during read",
            )
        return payload

    def _data_path(self, locator: str) -> Path:
        relative = PurePosixPath(_safe_locator(locator))
        if relative.is_absolute() or any(
            part in {"", ".", ".."} for part in relative.parts
        ):
            raise FetchContractError(
                FetchErrorCode.MALFORMED_OBJECT,
                "filesystem replay locator escapes its root",
            )
        path = self.root.joinpath(*relative.parts)
        current = self.root
        for part in relative.parts:
            current /= part
            try:
                status = os.lstat(current)
            except OSError as error:
                raise FetchContractError(
                    FetchErrorCode.MALFORMED_OBJECT,
                    "filesystem replay locator is unavailable",
                ) from error
            if stat.S_ISLNK(status.st_mode):
                raise FetchContractError(
                    FetchErrorCode.MALFORMED_OBJECT,
                    "filesystem replay locator traverses a symlink",
                )
        return path

    def plan(self, request: FetchRequest) -> tuple[SourceObject, ...]:
        """Select only sidecar-manifested objects matching all request filters."""
        if (
            request.provider_id != self.provider_id
            or request.dataset_id != self.dataset_id
        ):
            raise FetchContractError(
                FetchErrorCode.PLAN_MISMATCH,
                "request does not match the filesystem replay provider",
            )
        requested = set(request.tickers)
        return tuple(
            source
            for source in (
                self._decode_metadata(path) for path in self._metadata_paths()
            )
            if request.start_date <= source.coverage_date <= request.end_date
            and requested.intersection(source.tickers)
        )

    def fetch_range(
        self,
        source: SourceObject,
        *,
        offset: int,
        maximum_bytes: int,
        timeout_ns: int,
        credential: SecretValue | None,
    ) -> ProviderChunk:
        """Read one verified local file range without following symlinks."""
        del timeout_ns
        if credential is not None:
            raise FetchContractError(
                FetchErrorCode.INVALID_REQUEST,
                "filesystem replay provider does not accept a credential",
            )
        path = self._data_path(source.locator)
        descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
        try:
            before = os.fstat(descriptor)
            os.lseek(descriptor, offset, os.SEEK_SET)
            body = os.read(descriptor, maximum_bytes)
            after = os.fstat(descriptor)
        finally:
            os.close(descriptor)
        if (
            before.st_dev,
            before.st_ino,
            before.st_size,
            before.st_mtime_ns,
        ) != (
            after.st_dev,
            after.st_ino,
            after.st_size,
            after.st_mtime_ns,
        ):
            raise ProviderFetchError(
                FetchErrorCode.OBJECT_CHANGED,
                "filesystem replay object changed during read",
                retryable=False,
            )
        return ProviderChunk(
            offset=offset,
            data=body,
            total_size_bytes=before.st_size,
            version_id=source.version_id,
            complete=offset + len(body) == before.st_size,
        )

    def validate_payload(self, source: SourceObject, payload: bytes) -> int:
        """Verify the complete replay record count."""
        count = len(payload.splitlines())
        if count != source.record_count:
            raise FetchContractError(
                FetchErrorCode.MALFORMED_OBJECT,
                "filesystem replay record count does not match its metadata",
            )
        return count


@dataclass(frozen=True, slots=True)
class MockObject:
    """One deterministic HTTP/S3-like fixture without a socket."""

    locator: str
    coverage_date: date
    tickers: tuple[str, ...]
    payload: bytes
    times: ManifestTimeRange
    version_id: str = "mock-version-1"
    record_count: int = 1
    expected_sha256_override: str | None = None
    omit_expected_sha256: bool = False
    malformed: bool = False
    resume_mode: ResumeMode = ResumeMode.VERIFIED_RANGE
    validated_record_count_override: int | None = None
    lineage: ManifestLineage = field(default_factory=ManifestLineage)


class DeterministicMockHttpS3Provider:
    """Scripted in-process remote provider for deterministic integration tests."""

    remote = True

    def __init__(
        self,
        *,
        provider_id: str,
        dataset_id: str,
        objects: Sequence[MockObject],
        failures: Mapping[str, Sequence[MockFailure]] | None = None,
        credential_reference: str | None = None,
        on_fetch: Callable[[str, int], None] | None = None,
        duplicate_first_object: bool = False,
    ) -> None:
        """Create a socket-free scripted provider with bounded fixture state."""
        _require_identifier(provider_id, "mock provider_id")
        _require_identifier(dataset_id, "mock dataset_id")
        self.provider_id = provider_id
        self.dataset_id = dataset_id
        self.credential_reference = credential_reference
        self._objects = tuple(objects)
        self._by_locator = {item.locator: item for item in objects}
        self._failures = {
            locator: list(script) for locator, script in (failures or {}).items()
        }
        self._calls: dict[str, int] = {}
        self._lock = threading.Lock()
        self._on_fetch = on_fetch
        self._duplicate_first_object = duplicate_first_object

    def health(self) -> ProviderHealthState:
        """Report deterministic in-process availability."""
        return ProviderHealthState.HEALTHY

    def _source(self, item: MockObject) -> SourceObject:
        expected = (
            None
            if item.omit_expected_sha256
            else item.expected_sha256_override or _sha256(item.payload)
        )
        return SourceObject(
            provider_id=self.provider_id,
            dataset_id=self.dataset_id,
            source_version="deterministic-mock-v1",
            locator=item.locator,
            version_id=item.version_id,
            coverage_date=item.coverage_date,
            tickers=item.tickers,
            estimated_size_bytes=len(item.payload),
            expected_sha256=expected,
            record_count=item.record_count,
            schema_name="mock-object",
            schema_version="1.0.0",
            content_format="application/octet-stream",
            times=item.times,
            resume_mode=item.resume_mode,
            lineage=item.lineage,
        )

    def plan(self, request: FetchRequest) -> tuple[SourceObject, ...]:
        """Filter predeclared fixture metadata without contacting a network."""
        if (
            request.provider_id != self.provider_id
            or request.dataset_id != self.dataset_id
        ):
            raise FetchContractError(
                FetchErrorCode.PLAN_MISMATCH, "request does not match mock provider"
            )
        requested = set(request.tickers)
        planned = tuple(
            self._source(item)
            for item in self._objects
            if request.start_date <= item.coverage_date <= request.end_date
            and requested.intersection(item.tickers)
        )
        if self._duplicate_first_object and planned:
            return (planned[0], *planned)
        return planned

    def fetch_range(
        self,
        source: SourceObject,
        *,
        offset: int,
        maximum_bytes: int,
        timeout_ns: int,
        credential: SecretValue | None,
    ) -> ProviderChunk:
        """Return a scripted bounded range or deterministic transport failure."""
        del credential, timeout_ns
        with self._lock:
            call_number = self._calls.get(source.locator, 0) + 1
            self._calls[source.locator] = call_number
            script = self._failures.get(source.locator, [])
            failure = script.pop(0) if script else None
        if self._on_fetch is not None:
            self._on_fetch(source.locator, call_number)
        if failure is MockFailure.TIMEOUT:
            raise ProviderFetchError(
                FetchErrorCode.TIMEOUT, "mock timeout", retryable=True
            )
        if failure is MockFailure.RATE_LIMIT:
            raise ProviderFetchError(
                FetchErrorCode.RATE_LIMITED,
                "mock rate limit",
                retryable=True,
                retry_after_ns=1,
            )
        if failure is MockFailure.PARTIAL_RESPONSE:
            raise ProviderFetchError(
                FetchErrorCode.PARTIAL_RESPONSE,
                "mock partial response",
                retryable=True,
            )
        if failure is MockFailure.NONRETRYABLE:
            raise ProviderFetchError(
                FetchErrorCode.OBJECT_CHANGED,
                "mock nonretryable failure",
                retryable=False,
            )
        if failure is MockFailure.SUCCESS:
            failure = None
        item = self._by_locator[source.locator]
        body = item.payload[offset : offset + maximum_bytes]
        if failure is MockFailure.EMPTY_RESPONSE:
            body = b""
        version = (
            f"{item.version_id}-changed"
            if failure is MockFailure.CHANGED_OBJECT
            else item.version_id
        )
        total = (
            len(item.payload) + 1
            if failure is MockFailure.OVERSIZED_OBJECT
            else len(item.payload)
        )
        return ProviderChunk(
            offset=offset + 1 if failure is MockFailure.BAD_OFFSET else offset,
            data=body,
            total_size_bytes=total,
            version_id=version,
            complete=(offset + len(body) == len(item.payload))
            != (failure is MockFailure.BAD_COMPLETION),
        )

    def validate_payload(self, source: SourceObject, payload: bytes) -> int:
        """Apply fixture structural validation after complete transfer."""
        del payload
        item = self._by_locator[source.locator]
        if item.malformed:
            raise FetchContractError(
                FetchErrorCode.MALFORMED_OBJECT,
                "mock object failed structural validation",
            )
        return item.validated_record_count_override or item.record_count


class IngestionCoordinator:
    """Plan and execute bounded source-object ingestion under one storage lease."""

    def __init__(
        self,
        repository: DataRepository,
        provider: DataProvider,
        entitlement: DataEntitlement,
        *,
        rate_limit: RateLimitPolicy | None = None,
        retry: RetryPolicy | None = None,
        credential_provider: CredentialProvider | None = None,
        shutdown: ShutdownSignal | None = None,
        monotonic_ns: Callable[[], int] = time.monotonic_ns,
        sleep: Callable[[float], None] = time.sleep,
        utc_now: Callable[[], datetime] = lambda: datetime.now(UTC),
    ) -> None:
        """Bind one provider to repository, authorization, and resource policies."""
        self.repository = repository
        self.provider = provider
        self.entitlement = entitlement
        self.rate_limit = rate_limit or RateLimitPolicy()
        self.retry = retry or RetryPolicy()
        self.credential_provider = credential_provider
        self.shutdown = shutdown or ShutdownSignal()
        self._utc_now = utc_now
        self._sleep = sleep
        self._rate_limiter = _RateLimiter(
            self.rate_limit, monotonic_ns=monotonic_ns, sleep=sleep
        )

    def plan(self, request: FetchRequest, quota: QuotaEvidence) -> FetchPlan:
        """Create a no-network plan and perform conservative storage admission."""
        if (
            request.provider_id != self.provider.provider_id
            or request.dataset_id != self.provider.dataset_id
            or request.provider_id != self.entitlement.source_id
            or request.dataset_id != self.entitlement.dataset_id
        ):
            raise FetchContractError(
                FetchErrorCode.PLAN_MISMATCH,
                "request, provider, and entitlement identities do not match",
            )
        health = self.provider.health()
        if not isinstance(health, ProviderHealthState):
            raise FetchContractError(
                FetchErrorCode.PROVIDER_UNAVAILABLE,
                "provider returned an unknown health state",
            )
        if health is ProviderHealthState.UNAVAILABLE:
            raise FetchContractError(
                FetchErrorCode.PROVIDER_UNAVAILABLE,
                "provider is locally unavailable",
            )
        self.repository.initialize()
        raw_objects = self.provider.plan(request)
        if not raw_objects:
            raise FetchContractError(
                FetchErrorCode.SIZE_UNKNOWN,
                "provider produced no objects for the requested filters",
            )
        if len(raw_objects) > MAX_FETCH_OBJECTS:
            raise FetchContractError(
                FetchErrorCode.OBJECT_TOO_LARGE,
                "provider plan exceeds the object-count bound",
            )
        unique: dict[str, SourceObject] = {}
        duplicates = 0
        requested_tickers = set(request.tickers)
        for source in raw_objects:
            if (
                source.provider_id != request.provider_id
                or source.dataset_id != request.dataset_id
                or not request.start_date <= source.coverage_date <= request.end_date
                or not set(source.tickers).issubset(requested_tickers)
            ):
                raise FetchContractError(
                    FetchErrorCode.PLAN_MISMATCH,
                    "provider returned an out-of-scope source object",
                )
            if source.estimated_size_bytes > request.maximum_object_bytes:
                raise FetchContractError(
                    FetchErrorCode.OBJECT_TOO_LARGE,
                    "source object exceeds the request size bound",
                )
            existing = unique.get(source.object_id)
            if existing is None:
                unique[source.object_id] = source
            elif existing == source:
                duplicates += 1
            else:
                raise FetchContractError(
                    FetchErrorCode.DUPLICATE_OBJECT,
                    "provider returned a conflicting object identity",
                )
        objects = tuple(sorted(unique.values(), key=lambda item: item.object_id))
        total = _checked_sum([item.estimated_size_bytes for item in objects])
        if total > request.maximum_total_bytes:
            raise FetchContractError(
                FetchErrorCode.OBJECT_TOO_LARGE,
                "fetch plan exceeds the request total-byte bound",
            )
        manifest_bytes = _checked_sum([MANIFEST_ESTIMATE_BYTES] * len(objects))
        output_bytes = _checked_sum([total, manifest_bytes])
        largest = max(item.estimated_size_bytes for item in objects)
        temporary_bytes = _checked_sum(
            [
                largest * min(request.maximum_concurrency, len(objects)),
                MAX_CHECKPOINT_BYTES * min(request.maximum_concurrency, len(objects)),
            ]
        )
        storage_request = StorageRequest(
            operation_id=request.request_id,
            output_bytes=output_bytes,
            temporary_bytes=temporary_bytes,
            retry_overhead_bytes=0,
        )
        admission = self.repository.estimate(storage_request, quota)
        return FetchPlan(
            request=request,
            objects=objects,
            duplicate_object_count=duplicates,
            total_estimated_bytes=total,
            storage_request=storage_request,
            admission=admission,
            rate_limit=self.rate_limit,
            retry=self.retry,
            provider_health=health,
            entitlement=self.entitlement,
        )

    def execute(
        self,
        plan: FetchPlan,
        quota: QuotaEvidence,
        *,
        resume: bool = False,
    ) -> FetchResult:
        """Execute an explicitly requested admitted plan with bounded workers."""
        if plan.request.execute is False:
            return FetchResult(
                plan.plan_id,
                plan.request.request_id,
                FetchStatus.PLANNED,
                (),
                False,
                plan.request.deterministic_seed,
            )
        current_plan = self.plan(plan.request, quota)
        if plan.plan_id != current_plan.plan_id:
            raise FetchContractError(
                FetchErrorCode.PLAN_MISMATCH,
                "fetch plan no longer matches provider metadata",
            )
        if current_plan.provider_health is not ProviderHealthState.HEALTHY:
            raise FetchContractError(
                FetchErrorCode.PROVIDER_UNAVAILABLE,
                "provider health does not permit execution",
            )
        now = self._utc_now()
        if now.tzinfo is None or now.utcoffset() != timedelta(0):
            raise FetchContractError(
                FetchErrorCode.UNAUTHORIZED, "authorization clock is not UTC"
            )
        if not self.entitlement.permits(remote=self.provider.remote, now_utc=now):
            raise FetchContractError(
                FetchErrorCode.UNAUTHORIZED,
                "data entitlement does not permit this execution",
            )
        if not plan.admission.admitted:
            raise FetchContractError(
                FetchErrorCode.SIZE_UNKNOWN,
                "fetch plan did not pass storage admission",
            )
        credential: SecretValue | None = None
        reference = self.provider.credential_reference
        if reference is not None:
            if self.credential_provider is None:
                raise FetchContractError(
                    FetchErrorCode.CREDENTIAL_UNAVAILABLE,
                    "provider requires an external credential interface",
                )
            credential = self.credential_provider.get(reference)
            if credential is None:
                raise FetchContractError(
                    FetchErrorCode.CREDENTIAL_UNAVAILABLE,
                    "provider credential is unavailable",
                )

        results: list[ObjectFetchResult] = []
        network_access = False
        try:
            with self.repository.acquire_admission(
                plan.storage_request, quota
            ) as lease:
                for start in range(
                    0, len(plan.objects), plan.request.maximum_concurrency
                ):
                    if self.shutdown.requested:
                        break
                    batch = plan.objects[
                        start : start + plan.request.maximum_concurrency
                    ]
                    with ThreadPoolExecutor(max_workers=len(batch)) as workers:
                        futures = [
                            workers.submit(
                                self._fetch_one,
                                plan,
                                source,
                                lease,
                                credential,
                                resume,
                            )
                            for source in batch
                        ]
                        for future in futures:
                            result, accessed = future.result()
                            results.append(result)
                            network_access = network_access or accessed
        except StorageError as error:
            raise FetchContractError(
                FetchErrorCode.IO_FAILURE, f"storage operation failed: {error}"
            ) from error
        completed_ids = {item.object_id for item in results}
        if self.shutdown.requested:
            for source in plan.objects:
                if source.object_id not in completed_ids:
                    results.append(
                        ObjectFetchResult(
                            object_id=source.object_id,
                            status=FetchStatus.STOPPED,
                            bytes_received=0,
                            attempts=0,
                            error_code=FetchErrorCode.SHUTDOWN,
                            error_message="shutdown requested before object start",
                        )
                    )
        ordered = tuple(sorted(results, key=lambda item: item.object_id))
        statuses = {item.status for item in ordered}
        if statuses <= {FetchStatus.COMPLETED, FetchStatus.ALREADY_PRESENT}:
            status = FetchStatus.COMPLETED
        elif FetchStatus.STOPPED in statuses:
            status = FetchStatus.STOPPED
        elif len(statuses) == 1 and FetchStatus.FAILED in statuses:
            status = FetchStatus.FAILED
        else:
            status = FetchStatus.PARTIAL
        return FetchResult(
            plan.plan_id,
            plan.request.request_id,
            status,
            ordered,
            network_access,
            plan.request.deterministic_seed,
        )

    def _fetch_one(
        self,
        plan: FetchPlan,
        source: SourceObject,
        lease: AdmissionLease,
        credential: SecretValue | None,
        resume: bool,
    ) -> tuple[ObjectFetchResult, bool]:
        checkpoint_relative = f"tmp/checkpoints/{source.object_id}.json"
        staged_relative = f"tmp/downloads/{source.object_id}.partial"
        checkpoint_path = self.repository.root / checkpoint_relative
        staged_path = self.repository.root / staged_relative
        checkpoint_path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        staged_path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        expected_path = (
            None
            if source.expected_sha256 is None
            else self._raw_relative(source, source.expected_sha256)
        )
        if expected_path is not None:
            present = self._existing_result(
                plan, source, expected_path, checkpoint_path, lease
            )
            if present is not None:
                return present, False
        try:
            checkpoint, payload = self._open_or_create_checkpoint(
                plan,
                source,
                checkpoint_path,
                staged_path,
                resume=resume,
            )
        except FetchContractError as error:
            return self._failed(source, error, checkpoint_relative), False

        attempts = checkpoint.attempts
        sequence = checkpoint.sequence
        offset = checkpoint.completed_bytes
        retry_delays: list[int] = []
        accessed = False
        secret_values = () if credential is None else (credential.reveal(),)
        if source.resume_mode is ResumeMode.COMPLETE_OBJECT_RESTART and offset:
            self._rewrite_staged(staged_path)
            payload = b""
            offset = 0
            sequence += 1
            checkpoint = self._checkpoint(
                plan, source, payload, attempts=attempts, sequence=sequence
            )
            self._save_checkpoint(checkpoint_path, checkpoint)

        while offset < source.estimated_size_bytes:
            if self.shutdown.requested:
                return (
                    ObjectFetchResult(
                        object_id=source.object_id,
                        status=FetchStatus.STOPPED,
                        bytes_received=offset,
                        attempts=attempts,
                        retry_delays_ns=tuple(retry_delays),
                        checkpoint_path=checkpoint_relative,
                        error_code=FetchErrorCode.SHUTDOWN,
                        error_message="shutdown requested; durable checkpoint retained",
                    ),
                    accessed,
                )
            chunk: ProviderChunk | None = None
            terminal_error: FetchContractError | None = None
            for retry_attempt in range(  # pragma: no branch - policy requires >= 1
                1, self.retry.maximum_attempts + 1
            ):
                attempts += 1
                try:
                    self._rate_limiter.acquire()
                    accessed = accessed or self.provider.remote
                    chunk = self.provider.fetch_range(
                        source,
                        offset=offset,
                        maximum_bytes=plan.request.chunk_bytes,
                        timeout_ns=plan.request.request_timeout_ns,
                        credential=credential,
                    )
                    break
                except ProviderFetchError as error:
                    terminal_error = FetchContractError(
                        error.code, redact_text(str(error), secret_values)
                    )
                    if (
                        not error.retryable
                        or retry_attempt == self.retry.maximum_attempts
                    ):
                        break
                    delay = max(
                        self.retry.delay_ns(retry_attempt, source.object_id),
                        error.retry_after_ns,
                    )
                    if delay > self.retry.maximum_delay_ns:
                        terminal_error = FetchContractError(
                            FetchErrorCode.RETRY_EXHAUSTED,
                            "provider retry-after exceeds the configured delay bound",
                        )
                        break
                    retry_delays.append(delay)
                    self._sleep(delay / 1_000_000_000)
                    checkpoint = self._checkpoint(
                        plan,
                        source,
                        payload,
                        attempts=attempts,
                        sequence=sequence + 1,
                    )
                    sequence = checkpoint.sequence
                    self._save_checkpoint(checkpoint_path, checkpoint)
                except FetchContractError as error:
                    terminal_error = FetchContractError(
                        error.code, redact_text(str(error), secret_values)
                    )
                    break
                except Exception as error:
                    terminal_error = FetchContractError(
                        FetchErrorCode.IO_FAILURE,
                        redact_text(str(error), secret_values),
                    )
                    break
            if chunk is None:
                result_error = terminal_error or FetchContractError(
                    FetchErrorCode.RETRY_EXHAUSTED, "provider retry budget exhausted"
                )
                return self._failed(
                    source,
                    result_error,
                    checkpoint_relative,
                    bytes_received=offset,
                    attempts=attempts,
                    retry_delays=tuple(retry_delays),
                ), accessed
            validation_error = self._validate_chunk(plan, source, chunk, offset)
            if validation_error is not None:
                if staged_path.exists() and staged_path.stat().st_size:
                    return (
                        self._quarantine(
                            source,
                            staged_relative,
                            checkpoint_path,
                            validation_error,
                            lease,
                            attempts,
                            tuple(retry_delays),
                        ),
                        accessed,
                    )
                return self._failed(
                    source,
                    validation_error,
                    checkpoint_relative,
                    attempts=attempts,
                    retry_delays=tuple(retry_delays),
                ), accessed
            self._append_staged(staged_path, chunk.data)
            payload += chunk.data
            offset += len(chunk.data)
            sequence += 1
            checkpoint = self._checkpoint(
                plan, source, payload, attempts=attempts, sequence=sequence
            )
            self._save_checkpoint(checkpoint_path, checkpoint)

        digest = _sha256(payload)
        try:
            if source.expected_sha256 is not None and digest != source.expected_sha256:
                raise FetchContractError(
                    FetchErrorCode.HASH_MISMATCH,
                    "source object SHA-256 does not match the plan",
                )
            records = self.provider.validate_payload(source, payload)
            if records != source.record_count:
                raise FetchContractError(
                    FetchErrorCode.MALFORMED_OBJECT,
                    "validated record count does not match the plan",
                )
        except FetchContractError as error:
            return (
                self._quarantine(
                    source,
                    staged_relative,
                    checkpoint_path,
                    error,
                    lease,
                    attempts,
                    tuple(retry_delays),
                ),
                accessed,
            )
        raw_relative = self._raw_relative(source, digest)
        try:
            self.repository.publish_staged_object(
                staged_relative,
                raw_relative,
                expected_sha256=digest,
                expected_size_bytes=len(payload),
                lease=lease,
            )
            manifest = self._manifest(plan, source, raw_relative, digest, records)
            self.repository.publish_manifest(manifest, lease)
            self._remove_checkpoint(checkpoint_path)
        except StorageError as error:
            return self._failed(
                source,
                FetchContractError(
                    FetchErrorCode.IO_FAILURE, f"publication failed: {error}"
                ),
                checkpoint_relative,
                bytes_received=len(payload),
                attempts=attempts,
                retry_delays=tuple(retry_delays),
            ), accessed
        return (
            ObjectFetchResult(
                object_id=source.object_id,
                status=FetchStatus.COMPLETED,
                bytes_received=len(payload),
                attempts=attempts,
                retry_delays_ns=tuple(retry_delays),
                object_sha256=digest,
                storage_path=raw_relative,
                manifest_id=manifest.manifest_id,
            ),
            accessed,
        )

    def _validate_chunk(
        self,
        plan: FetchPlan,
        source: SourceObject,
        chunk: ProviderChunk,
        offset: int,
    ) -> FetchContractError | None:
        if chunk.offset != offset:
            return FetchContractError(
                FetchErrorCode.PARTIAL_RESPONSE,
                "provider range offset does not match the checkpoint",
            )
        if chunk.version_id != source.version_id:
            return FetchContractError(
                FetchErrorCode.OBJECT_CHANGED,
                "provider object version changed after planning",
            )
        if (
            chunk.total_size_bytes != source.estimated_size_bytes
            or chunk.total_size_bytes > plan.request.maximum_object_bytes
            or offset + len(chunk.data) > chunk.total_size_bytes
        ):
            return FetchContractError(
                FetchErrorCode.OBJECT_TOO_LARGE,
                "provider response size differs from the admitted estimate",
            )
        if not chunk.data:
            return FetchContractError(
                FetchErrorCode.PARTIAL_RESPONSE,
                "provider returned an empty incomplete range",
            )
        if chunk.complete != (offset + len(chunk.data) == chunk.total_size_bytes):
            return FetchContractError(
                FetchErrorCode.PARTIAL_RESPONSE,
                "provider completion marker is inconsistent",
            )
        return None

    def _existing_result(
        self,
        plan: FetchPlan,
        source: SourceObject,
        raw_relative: str,
        checkpoint_path: Path,
        lease: AdmissionLease,
    ) -> ObjectFetchResult | None:
        raw_path = self.repository.root / raw_relative
        if not raw_path.exists():
            return None
        digest = cast("str", source.expected_sha256)
        manifest = self._manifest(
            plan, source, raw_relative, digest, source.record_count
        )
        try:
            self.repository.publish_manifest(manifest, lease)
        except StorageError as error:
            raise FetchContractError(
                FetchErrorCode.IO_FAILURE,
                f"existing object or source manifest failed verification: {error}",
            ) from error
        self._remove_checkpoint(checkpoint_path)
        return ObjectFetchResult(
            object_id=source.object_id,
            status=FetchStatus.ALREADY_PRESENT,
            bytes_received=0,
            attempts=0,
            object_sha256=digest,
            storage_path=raw_relative,
            manifest_id=manifest.manifest_id,
        )

    def _open_or_create_checkpoint(
        self,
        plan: FetchPlan,
        source: SourceObject,
        checkpoint_path: Path,
        staged_path: Path,
        *,
        resume: bool,
    ) -> tuple[DownloadCheckpoint, bytes]:
        if checkpoint_path.exists() or staged_path.exists():
            if not resume or not checkpoint_path.exists() or not staged_path.exists():
                raise FetchContractError(
                    FetchErrorCode.CHECKPOINT_CORRUPT,
                    "partial object requires an explicit verified resume",
                )
            checkpoint = DownloadCheckpoint.decode(
                self._read_bounded(checkpoint_path, MAX_CHECKPOINT_BYTES)
            )
            payload = self._read_bounded(staged_path, source.estimated_size_bytes)
            self._validate_checkpoint(plan, source, checkpoint, payload)
            return checkpoint, payload
        self._rewrite_staged(staged_path)
        checkpoint = self._checkpoint(plan, source, b"", attempts=0, sequence=0)
        self._save_checkpoint(checkpoint_path, checkpoint)
        return checkpoint, b""

    @staticmethod
    def _validate_checkpoint(
        plan: FetchPlan,
        source: SourceObject,
        checkpoint: DownloadCheckpoint,
        payload: bytes,
    ) -> None:
        if (
            checkpoint.plan_id != plan.plan_id
            or checkpoint.request_id != plan.request.request_id
            or checkpoint.object_id != source.object_id
            or checkpoint.provider_id != source.provider_id
            or checkpoint.dataset_id != source.dataset_id
            or checkpoint.locator_sha256 != _sha256(source.locator.encode())
            or checkpoint.version_id != source.version_id
            or checkpoint.expected_size_bytes != source.estimated_size_bytes
            or checkpoint.completed_bytes != len(payload)
            or checkpoint.prefix_sha256 != _sha256(payload)
        ):
            raise FetchContractError(
                FetchErrorCode.CHECKPOINT_CORRUPT,
                "checkpoint does not match the staged object and fetch plan",
            )

    @staticmethod
    def _checkpoint(
        plan: FetchPlan,
        source: SourceObject,
        payload: bytes,
        *,
        attempts: int,
        sequence: int,
    ) -> DownloadCheckpoint:
        return DownloadCheckpoint(
            plan_id=plan.plan_id,
            request_id=plan.request.request_id,
            object_id=source.object_id,
            provider_id=source.provider_id,
            dataset_id=source.dataset_id,
            locator_sha256=_sha256(source.locator.encode()),
            version_id=source.version_id,
            expected_size_bytes=source.estimated_size_bytes,
            completed_bytes=len(payload),
            prefix_sha256=_sha256(payload),
            attempts=attempts,
            sequence=sequence,
        )

    def _manifest(
        self,
        plan: FetchPlan,
        source: SourceObject,
        raw_relative: str,
        digest: str,
        records: int,
    ) -> SourceManifest:
        return SourceManifest(
            source_name=source.provider_id,
            source_version=_sha256(
                _canonical_bytes(
                    {
                        "entitlement": self.entitlement.to_dict(),
                        "provider_source_version": source.source_version,
                    }
                )
            ),
            source_object_id=source.object_id,
            storage_path=raw_relative,
            object_sha256=digest,
            size_bytes=source.estimated_size_bytes,
            record_count=records,
            schema_name=source.schema_name,
            schema_version=source.schema_version,
            times=source.times,
            universe_snapshot_sha256=plan.request.universe_snapshot_sha256,
            lineage=source.lineage,
        )

    @staticmethod
    def _raw_relative(source: SourceObject, digest: str) -> str:
        return f"raw/{source.provider_id}/{source.dataset_id}/{digest}.bin"

    def _quarantine(
        self,
        source: SourceObject,
        staged_relative: str,
        checkpoint_path: Path,
        error: FetchContractError,
        lease: AdmissionLease,
        attempts: int,
        retry_delays: tuple[int, ...],
    ) -> ObjectFetchResult:
        staged_path = self.repository.root / staged_relative
        payload = self._read_bounded(staged_path, source.estimated_size_bytes)
        digest = _sha256(payload)
        quarantine_relative = (
            f"quarantine/{source.provider_id}/{source.object_id}/"
            f"{error.code.value.lower()}-{digest}.bin"
        )
        try:
            self.repository.publish_staged_object(
                staged_relative,
                quarantine_relative,
                expected_sha256=digest,
                expected_size_bytes=len(payload),
                lease=lease,
            )
            self._remove_checkpoint(checkpoint_path)
        except StorageError as storage_error:
            return self._failed(
                source,
                FetchContractError(
                    FetchErrorCode.IO_FAILURE,
                    f"quarantine publication failed: {storage_error}",
                ),
                checkpoint_path.relative_to(self.repository.root).as_posix(),
                bytes_received=len(payload),
                attempts=attempts,
                retry_delays=retry_delays,
            )
        return ObjectFetchResult(
            object_id=source.object_id,
            status=FetchStatus.QUARANTINED,
            bytes_received=len(payload),
            attempts=attempts,
            retry_delays_ns=retry_delays,
            object_sha256=digest,
            quarantine_path=quarantine_relative,
            error_code=error.code,
            error_message=str(error),
        )

    @staticmethod
    def _failed(
        source: SourceObject,
        error: FetchContractError,
        checkpoint_path: str | None,
        *,
        bytes_received: int = 0,
        attempts: int = 0,
        retry_delays: tuple[int, ...] = (),
    ) -> ObjectFetchResult:
        return ObjectFetchResult(
            object_id=source.object_id,
            status=FetchStatus.FAILED,
            bytes_received=bytes_received,
            attempts=attempts,
            retry_delays_ns=retry_delays,
            checkpoint_path=checkpoint_path,
            error_code=error.code,
            error_message=str(error),
        )

    @staticmethod
    def _append_staged(path: Path, payload: bytes) -> None:
        descriptor = os.open(
            path, os.O_WRONLY | os.O_APPEND | os.O_CLOEXEC | os.O_NOFOLLOW
        )
        try:
            view = memoryview(payload)
            written = 0
            while written < len(view):
                count = os.write(descriptor, view[written:])
                if count <= 0:
                    raise FetchContractError(
                        FetchErrorCode.IO_FAILURE, "short staged-object write"
                    )
                written += count
            os.fsync(descriptor)
        finally:
            os.close(descriptor)

    @staticmethod
    def _rewrite_staged(path: Path) -> None:
        flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_CLOEXEC | os.O_NOFOLLOW
        descriptor = os.open(path, flags, 0o600)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)

    @staticmethod
    def _save_checkpoint(path: Path, checkpoint: DownloadCheckpoint) -> None:
        temporary = path.with_suffix(".new")
        if temporary.exists() or temporary.is_symlink():
            raise FetchContractError(
                FetchErrorCode.CHECKPOINT_CORRUPT,
                "stale checkpoint publication file requires review",
            )
        descriptor = os.open(
            temporary,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
            0o600,
        )
        try:
            encoded = checkpoint.encode()
            view = memoryview(encoded)
            written = 0
            while written < len(view):
                count = os.write(descriptor, view[written:])
                if count <= 0:
                    raise FetchContractError(
                        FetchErrorCode.IO_FAILURE, "short checkpoint write"
                    )
                written += count
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
        temporary.replace(path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)

    @staticmethod
    def _remove_checkpoint(path: Path) -> None:
        try:
            path.unlink()
        except FileNotFoundError:
            return
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)

    @staticmethod
    def _read_bounded(path: Path, maximum_bytes: int) -> bytes:
        status = os.lstat(path)
        if (
            stat.S_ISLNK(status.st_mode)
            or not stat.S_ISREG(status.st_mode)
            or status.st_nlink != 1
            or status.st_size > maximum_bytes
        ):
            raise FetchContractError(
                FetchErrorCode.CHECKPOINT_CORRUPT,
                "staged or checkpoint file failed integrity bounds",
            )
        descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
        try:
            before = os.fstat(descriptor)
            payload = os.read(descriptor, before.st_size + 1)
            after = os.fstat(descriptor)
        finally:
            os.close(descriptor)
        if len(payload) != before.st_size or (
            before.st_dev,
            before.st_ino,
            before.st_size,
            before.st_mtime_ns,
        ) != (
            after.st_dev,
            after.st_ino,
            after.st_size,
            after.st_mtime_ns,
        ):
            raise FetchContractError(
                FetchErrorCode.CHECKPOINT_CORRUPT,
                "staged or checkpoint file changed during read",
            )
        return payload
