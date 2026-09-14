"""Bounded, point-in-time-safe FRED/ALFRED macro-vintage ingestion."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import UTC, date, datetime, timedelta
from enum import StrEnum
from pathlib import Path
from typing import TYPE_CHECKING, Final, Protocol, Self, cast, override

from aegis_mx_research.data_repository import (
    DEFAULT_DATA_ROOT,
    AdmissionLease,
    DataRepository,
    QuotaEvidence,
    StorageError,
    StorageRequest,
)
from aegis_mx_research.ingestion import SecretValue, redact_text

if TYPE_CHECKING:
    from collections.abc import Callable, Iterator, Mapping, Sequence

ALFRED_SCHEMA_VERSION: Final = "1.0.0"
ALFRED_SOURCE_REVISION: Final = "fred-api-v1-reviewed-2026-09-11"
ALFRED_SOURCE_ID: Final = "fred_alfred_selected_series"
ALFRED_ATTRIBUTION: Final = (
    "Federal Reserve Bank of St. Louis; underlying federal source cited per series"
)
ALFRED_HOST: Final = "api.stlouisfed.org"
ALFRED_METADATA_ENDPOINT: Final = "/fred/series"
ALFRED_RELEASE_ENDPOINT: Final = "/fred/series/release"
ALFRED_RELEASE_DATES_ENDPOINT: Final = "/fred/release/dates"
ALFRED_OBSERVATIONS_ENDPOINT: Final = "/fred/series/observations"
MAX_ALFRED_STORAGE_BYTES: Final = 999_000_000
MAX_RESPONSE_BYTES: Final = 8_000_000
MAX_POLICY_BYTES: Final = 65_536
MAX_ROWS_PER_SERIES: Final = 100_000
MAX_PAGE_LIMIT: Final = 10_000
MAX_PAGES_PER_ENDPOINT: Final = 100
MAX_REQUESTS: Final = 1_000
MAX_REQUESTS_PER_MINUTE: Final = 100
MAX_RETRY_ATTEMPTS: Final = 3
MAX_TIMEOUT_SECONDS: Final = 60
MAX_TEXT_BYTES: Final = 512
MAX_NOTES_BYTES: Final = 16_384
MAX_VINTAGE_WINDOW_DAYS: Final = 7_305
MAX_OBSERVATION_WINDOW_DAYS: Final = 36_525
MIN_APPROVAL_ID_BYTES: Final = 3
MAX_APPROVAL_ID_BYTES: Final = 128
HTTP_OK: Final = 200
HTTP_TOO_MANY_REQUESTS: Final = 429
MAX_INT64: Final = (1 << 63) - 1
MICRO_UNITS: Final = 1_000_000
MANIFEST_BUDGET_BYTES: Final = 16_384
SENTINEL_END_DATE: Final = date(9999, 12, 31)
_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_SERIES_ID = re.compile(r"^[A-Z][A-Z0-9]{1,31}$")
_VALUE = re.compile(r"^(-?)([0-9]{1,16})(?:\.([0-9]{1,6}))?$")


class AlfredErrorCode(StrEnum):
    """Stable fail-closed error and rejection codes."""

    INVALID_CONFIGURATION = "INVALID_CONFIGURATION"
    UNAUTHORIZED = "UNAUTHORIZED"
    RESTRICTED_SERIES = "RESTRICTED_SERIES"
    CREDENTIAL_UNAVAILABLE = "CREDENTIAL_UNAVAILABLE"
    PROVIDER_UNAVAILABLE = "PROVIDER_UNAVAILABLE"
    RATE_LIMITED = "RATE_LIMITED"
    RETRY_EXHAUSTED = "RETRY_EXHAUSTED"
    RESPONSE_TOO_LARGE = "RESPONSE_TOO_LARGE"
    MALFORMED_RESPONSE = "MALFORMED_RESPONSE"
    METADATA_MISMATCH = "METADATA_MISMATCH"
    PAGINATION_INVALID = "PAGINATION_INVALID"
    CONFLICTING_VINTAGE = "CONFLICTING_VINTAGE"
    TIMESTAMP_INVALID = "TIMESTAMP_INVALID"
    FUTURE_LEAKAGE = "FUTURE_LEAKAGE"
    STORAGE_LIMIT = "STORAGE_LIMIT"


class AlfredError(RuntimeError):
    """Typed ALFRED failure with a bounded, redacted message."""

    def __init__(self, code: AlfredErrorCode, message: str) -> None:
        """Construct a typed failure without retaining credential material."""
        super().__init__(redact_text(message)[:512])
        self.code = code


class SeriesRightsStatus(StrEnum):
    """Application disposition after source-owner review."""

    APPROVED_PUBLIC_DOMAIN = "APPROVED_PUBLIC_DOMAIN"
    DENIED_COPYRIGHTED = "DENIED_COPYRIGHTED"


class AlfredValueStatus(StrEnum):
    """Whether a vintage contains an observed numeric value."""

    OBSERVED = "OBSERVED"
    MISSING = "MISSING"


class AlfredVintageKind(StrEnum):
    """Position of a value in one observation's immutable revision chain."""

    INITIAL_RELEASE = "INITIAL_RELEASE"
    REVISION = "REVISION"


class ReleaseTimePrecision(StrEnum):
    """Precision actually asserted by the source response."""

    DATE_ONLY = "DATE_ONLY"
    NOT_AVAILABLE = "NOT_AVAILABLE"


class AlfredObjectKind(StrEnum):
    """Immutable object categories emitted by the adapter."""

    SERIES_METADATA = "SERIES_METADATA"
    SERIES_RELEASE = "SERIES_RELEASE"
    RELEASE_DATES = "RELEASE_DATES"
    OBSERVATIONS = "OBSERVATIONS"
    CANONICAL_SNAPSHOT = "CANONICAL_SNAPSHOT"
    RUN_REPORT_JSON = "RUN_REPORT_JSON"
    RUN_REPORT_MARKDOWN = "RUN_REPORT_MARKDOWN"


def _canonical_bytes(value: object) -> bytes:
    return json.dumps(
        value, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode("ascii")


def _sha256(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def _bounded_text(value: str, field: str, maximum: int = MAX_TEXT_BYTES) -> str:
    if (
        not isinstance(value, str)
        or not value
        or "\x00" in value
        or len(value.encode("utf-8")) > maximum
    ):
        raise AlfredError(
            AlfredErrorCode.MALFORMED_RESPONSE, f"{field} is absent or unbounded"
        )
    return value


def _require_sha256(value: str, field: str) -> str:
    if _SHA256.fullmatch(value) is None:
        raise AlfredError(
            AlfredErrorCode.INVALID_CONFIGURATION, f"{field} is not SHA-256"
        )
    return value


def _require_series_id(value: str) -> str:
    if not isinstance(value, str) or _SERIES_ID.fullmatch(value) is None:
        raise AlfredError(
            AlfredErrorCode.INVALID_CONFIGURATION, "series_id is malformed"
        )
    return value


def _utc_ns(value: datetime) -> int:
    if value.tzinfo is None or value.utcoffset() != timedelta(0):
        raise AlfredError(
            AlfredErrorCode.TIMESTAMP_INVALID, "timestamp is not explicit UTC"
        )
    epoch = datetime(1970, 1, 1, tzinfo=UTC)
    delta = value - epoch
    result = (
        delta.days * 86_400_000_000_000
        + delta.seconds * 1_000_000_000
        + delta.microseconds * 1_000
    )
    if result <= 0 or result > MAX_INT64:
        raise AlfredError(
            AlfredErrorCode.TIMESTAMP_INVALID, "timestamp is outside int64"
        )
    return result


def _date_start_ns(value: date) -> int:
    return _utc_ns(datetime(value.year, value.month, value.day, tzinfo=UTC))


def _conservative_known_ns(value: date) -> int:
    if value >= SENTINEL_END_DATE:
        raise AlfredError(
            AlfredErrorCode.TIMESTAMP_INVALID, "vintage date cannot be open-ended"
        )
    return _date_start_ns(value + timedelta(days=1))


def _known_through_ns(value: date) -> int | None:
    return None if value == SENTINEL_END_DATE else _conservative_known_ns(value)


def _parse_date(value: object, field: str) -> date:
    if not isinstance(value, str):
        raise AlfredError(
            AlfredErrorCode.MALFORMED_RESPONSE, f"{field} is not a date string"
        )
    try:
        return date.fromisoformat(value)
    except ValueError as error:
        raise AlfredError(
            AlfredErrorCode.MALFORMED_RESPONSE, f"{field} is malformed"
        ) from error


def _parse_updated_ns(value: object) -> int:
    text = _bounded_text(cast("str", value), "last_updated", 40)
    try:
        parsed = datetime.fromisoformat(text.replace(" ", "T", 1))
    except ValueError as error:
        raise AlfredError(
            AlfredErrorCode.MALFORMED_RESPONSE, "last_updated is malformed"
        ) from error
    if parsed.tzinfo is None:
        raise AlfredError(
            AlfredErrorCode.TIMESTAMP_INVALID, "last_updated has no UTC offset"
        )
    return _utc_ns(parsed.astimezone(UTC))


def _parse_micro_units(value: str) -> tuple[AlfredValueStatus, int | None]:
    if value == ".":
        return AlfredValueStatus.MISSING, None
    match = _VALUE.fullmatch(value)
    if match is None:
        raise AlfredError(
            AlfredErrorCode.MALFORMED_RESPONSE, "observation value is malformed"
        )
    fraction = (match.group(3) or "").ljust(6, "0")
    magnitude = int(match.group(2)) * MICRO_UNITS + int(fraction or "0")
    result = -magnitude if match.group(1) else magnitude
    if not -MAX_INT64 <= result <= MAX_INT64:
        raise AlfredError(
            AlfredErrorCode.MALFORMED_RESPONSE, "observation value exceeds int64"
        )
    return AlfredValueStatus.OBSERVED, result


@dataclass(frozen=True, slots=True)
class AlfredSeriesSpec:
    """Reviewed source-owner and native series contract."""

    series_id: str
    macro_event_type: str
    title: str
    source_owner: str
    units: str
    frequency: str
    seasonal_adjustment: str
    release_name: str
    rights_status: SeriesRightsStatus
    attribution: str
    rights_evidence_url: str
    series_url: str

    def __post_init__(self) -> None:
        """Validate the reviewed series contract."""
        _require_series_id(self.series_id)
        for field, value in (
            ("macro_event_type", self.macro_event_type),
            ("title", self.title),
            ("source_owner", self.source_owner),
            ("units", self.units),
            ("frequency", self.frequency),
            ("seasonal_adjustment", self.seasonal_adjustment),
            ("release_name", self.release_name),
            ("attribution", self.attribution),
            ("rights_evidence_url", self.rights_evidence_url),
            ("series_url", self.series_url),
        ):
            _bounded_text(value, field)

    def to_dict(self) -> dict[str, object]:
        """Return the stable JSON representation."""
        return {
            "attribution": self.attribution,
            "frequency": self.frequency,
            "macro_event_type": self.macro_event_type,
            "release_name": self.release_name,
            "rights_evidence_url": self.rights_evidence_url,
            "rights_status": self.rights_status.value,
            "seasonal_adjustment": self.seasonal_adjustment,
            "series_id": self.series_id,
            "series_url": self.series_url,
            "source_owner": self.source_owner,
            "title": self.title,
            "units": self.units,
        }


_BLS_RIGHTS: Final = "https://www.bls.gov/bls/linksite.htm"
_BEA_RIGHTS: Final = "https://www.bea.gov/help/faq/147"
_CENSUS_RIGHTS: Final = "https://www2.census.gov/foia/ds_policies/ds027.pdf"
_BOARD_RIGHTS: Final = "https://www.federalreserve.gov/disclaimer.htm"
_ISM_RIGHTS: Final = "https://www.ismworld.org/footer/terms-of-use/"


def _spec(
    series_id: str,
    event: str,
    title: str,
    owner: str,
    units: str,
    frequency: str,
    seasonal: str,
    release: str,
    rights_url: str,
) -> AlfredSeriesSpec:
    return AlfredSeriesSpec(
        series_id=series_id,
        macro_event_type=event,
        title=title,
        source_owner=owner,
        units=units,
        frequency=frequency,
        seasonal_adjustment=seasonal,
        release_name=release,
        rights_status=SeriesRightsStatus.APPROVED_PUBLIC_DOMAIN,
        attribution=f"Source: {owner}; retrieved through FRED/ALFRED",
        rights_evidence_url=rights_url,
        series_url=f"https://fred.stlouisfed.org/series/{series_id}",
    )


APPROVED_SERIES: Final = (
    _spec(
        "CPIAUCSL",
        "CPI",
        "Consumer Price Index for All Urban Consumers: All Items in U.S. City Average",
        "U.S. Bureau of Labor Statistics",
        "Index 1982-1984=100",
        "Monthly",
        "Seasonally Adjusted",
        "Consumer Price Index",
        _BLS_RIGHTS,
    ),
    _spec(
        "PPIACO",
        "PPI",
        "Producer Price Index by Commodity: All Commodities",
        "U.S. Bureau of Labor Statistics",
        "Index 1982=100",
        "Monthly",
        "Not Seasonally Adjusted",
        "Producer Price Index",
        _BLS_RIGHTS,
    ),
    _spec(
        "PAYEMS",
        "EMPLOYMENT_REPORT",
        "All Employees, Total Nonfarm",
        "U.S. Bureau of Labor Statistics",
        "Thousands of Persons",
        "Monthly",
        "Seasonally Adjusted",
        "Employment Situation",
        _BLS_RIGHTS,
    ),
    _spec(
        "GDP",
        "GDP",
        "Gross Domestic Product",
        "U.S. Bureau of Economic Analysis",
        "Billions of Dollars",
        "Quarterly",
        "Seasonally Adjusted Annual Rate",
        "Gross Domestic Product",
        _BEA_RIGHTS,
    ),
    _spec(
        "RSAFS",
        "RETAIL_SALES",
        "Advance Retail Sales: Retail Trade and Food Services",
        "U.S. Census Bureau",
        "Millions of Dollars",
        "Monthly",
        "Seasonally Adjusted",
        "Advance Monthly Sales for Retail and Food Services",
        _CENSUS_RIGHTS,
    ),
    _spec(
        "DFEDTARU",
        "FOMC_DECISION",
        "Federal Funds Target Range - Upper Limit",
        "Board of Governors of the Federal Reserve System (US)",
        "Percent",
        "Daily, 7-Day",
        "Not Seasonally Adjusted",
        "FOMC Press Release",
        _BOARD_RIGHTS,
    ),
    _spec(
        "DFEDTARL",
        "FOMC_DECISION",
        "Federal Funds Target Range - Lower Limit",
        "Board of Governors of the Federal Reserve System (US)",
        "Percent",
        "Daily, 7-Day",
        "Not Seasonally Adjusted",
        "FOMC Press Release",
        _BOARD_RIGHTS,
    ),
)
RESTRICTED_SERIES: Final = (
    AlfredSeriesSpec(
        series_id="NAPM",
        macro_event_type="PMI",
        title="ISM Manufacturing PMI",
        source_owner="Institute for Supply Management",
        units="Index",
        frequency="Monthly",
        seasonal_adjustment="Seasonally Adjusted",
        release_name="ISM Manufacturing PMI Report",
        rights_status=SeriesRightsStatus.DENIED_COPYRIGHTED,
        attribution="Institute for Supply Management",
        rights_evidence_url=_ISM_RIGHTS,
        series_url="https://www.ismworld.org/supply-management-news-and-reports/reports/ism-pmi-reports/",
    ),
)


def reviewed_series(series_id: str) -> AlfredSeriesSpec:
    """Return one reviewed series or fail closed for unknown/restricted input."""
    _require_series_id(series_id)
    for item in APPROVED_SERIES:
        if item.series_id == series_id:
            return item
    if any(item.series_id == series_id for item in RESTRICTED_SERIES):
        raise AlfredError(
            AlfredErrorCode.RESTRICTED_SERIES,
            f"series {series_id} is copyrighted and not approved",
        )
    raise AlfredError(
        AlfredErrorCode.UNAUTHORIZED, f"series {series_id} has no rights review"
    )


def series_universe_sha256(series_ids: Sequence[str]) -> str:
    """Hash a unique, sorted macro series universe."""
    if not series_ids or len(set(series_ids)) != len(series_ids):
        raise AlfredError(
            AlfredErrorCode.INVALID_CONFIGURATION,
            "series universe must be nonempty and unique",
        )
    specs = [reviewed_series(item) for item in series_ids]
    return _sha256(
        _canonical_bytes(
            [
                item.to_dict()
                for item in sorted(specs, key=lambda value: value.series_id)
            ]
        )
    )


@dataclass(frozen=True, slots=True)
class AlfredConfig:
    """Bounded query, request, response, and storage configuration."""

    vintage_start: date
    vintage_end: date
    observation_start: date
    observation_end: date
    series_ids: tuple[str, ...] = tuple(item.series_id for item in APPROVED_SERIES)
    page_limit: int = 1_000
    max_pages_per_endpoint: int = 20
    max_requests: int = 500
    requests_per_minute: int = 100
    retry_attempts: int = 3
    timeout_seconds: int = 20
    response_limit_bytes: int = MAX_RESPONSE_BYTES
    storage_limit_bytes: int = MAX_ALFRED_STORAGE_BYTES

    def __post_init__(self) -> None:
        """Validate all configured resource and time bounds."""
        if (
            self.vintage_end < self.vintage_start
            or self.observation_end < self.observation_start
            or (self.vintage_end - self.vintage_start).days > MAX_VINTAGE_WINDOW_DAYS
            or (self.observation_end - self.observation_start).days
            > MAX_OBSERVATION_WINDOW_DAYS
        ):
            raise AlfredError(
                AlfredErrorCode.INVALID_CONFIGURATION, "ALFRED date window is invalid"
            )
        series_universe_sha256(self.series_ids)
        if (
            not 1 <= self.page_limit <= MAX_PAGE_LIMIT
            or not 1 <= self.max_pages_per_endpoint <= MAX_PAGES_PER_ENDPOINT
            or not 1 <= self.max_requests <= MAX_REQUESTS
            or not 1 <= self.requests_per_minute <= MAX_REQUESTS_PER_MINUTE
            or not 1 <= self.retry_attempts <= MAX_RETRY_ATTEMPTS
            or not 1 <= self.timeout_seconds <= MAX_TIMEOUT_SECONDS
            or not 1 <= self.response_limit_bytes <= MAX_RESPONSE_BYTES
            or not 1 <= self.storage_limit_bytes <= MAX_ALFRED_STORAGE_BYTES
        ):
            raise AlfredError(
                AlfredErrorCode.INVALID_CONFIGURATION, "ALFRED bounds are invalid"
            )


@dataclass(frozen=True, slots=True)
class AlfredAuthorization:
    """External owner-only approval for the exact reviewed series and window."""

    approval_id: str
    allowed_series: tuple[str, ...]
    valid_from: date
    valid_through: date
    expires_at_utc_ns: int
    storage_limit_bytes: int
    api_execution_authorized: bool
    approval_sha256: str

    @classmethod
    def load(cls, path: Path) -> Self:
        """Load an owner-only, self-hashed authorization without following links."""
        try:
            status = os.lstat(path)
        except OSError as error:
            raise AlfredError(
                AlfredErrorCode.UNAUTHORIZED, "ALFRED approval is unavailable"
            ) from error
        if (
            not stat.S_ISREG(status.st_mode)
            or stat.S_ISLNK(status.st_mode)
            or status.st_size > MAX_POLICY_BYTES
            or status.st_mode & 0o077
        ):
            raise AlfredError(
                AlfredErrorCode.UNAUTHORIZED,
                "ALFRED approval must be an owner-only regular file",
            )
        try:
            document = json.loads(path.read_bytes())
        except (OSError, json.JSONDecodeError) as error:
            raise AlfredError(
                AlfredErrorCode.UNAUTHORIZED, "ALFRED approval is malformed"
            ) from error
        if not isinstance(document, dict):
            raise AlfredError(
                AlfredErrorCode.UNAUTHORIZED, "ALFRED approval is not an object"
            )
        required = {
            "allowed_series",
            "api_execution_authorized",
            "approval_id",
            "approval_sha256",
            "contains_secrets",
            "derived_data_authorized",
            "distribution",
            "expires_at_utc",
            "model_training_authorized",
            "persistent_storage_authorized",
            "schema_version",
            "source_id",
            "storage_limit_bytes_decimal",
            "use_classification",
            "valid_from",
            "valid_through",
        }
        if set(document) != required:
            raise AlfredError(
                AlfredErrorCode.UNAUTHORIZED, "ALFRED approval fields are not closed"
            )
        supplied_hash = document["approval_sha256"]
        body = {
            key: value for key, value in document.items() if key != "approval_sha256"
        }
        if not isinstance(supplied_hash, str) or supplied_hash != _sha256(
            _canonical_bytes(body)
        ):
            raise AlfredError(
                AlfredErrorCode.UNAUTHORIZED, "ALFRED approval hash mismatch"
            )
        if (
            document["schema_version"] != ALFRED_SCHEMA_VERSION
            or document["source_id"] != ALFRED_SOURCE_ID
            or document["contains_secrets"] is not False
            or document["distribution"] != "OWNER_ONLY_INTERNAL"
            or document["use_classification"] != "ACADEMIC_NON_COMMERCIAL"
            or document["persistent_storage_authorized"] is not True
            or document["model_training_authorized"] is not True
            or document["derived_data_authorized"] is not True
            or type(document["api_execution_authorized"]) is not bool
        ):
            raise AlfredError(
                AlfredErrorCode.UNAUTHORIZED, "ALFRED approval scope is invalid"
            )
        allowed = document["allowed_series"]
        if (
            not isinstance(allowed, list)
            or not allowed
            or not all(isinstance(item, str) for item in allowed)
            or allowed != sorted(set(allowed))
        ):
            raise AlfredError(
                AlfredErrorCode.UNAUTHORIZED, "allowed_series is not canonical"
            )
        for series_id in allowed:
            reviewed_series(series_id)
        try:
            expiry = datetime.fromisoformat(cast("str", document["expires_at_utc"]))
            valid_from = date.fromisoformat(cast("str", document["valid_from"]))
            valid_through = date.fromisoformat(cast("str", document["valid_through"]))
        except (TypeError, ValueError) as error:
            raise AlfredError(
                AlfredErrorCode.UNAUTHORIZED, "ALFRED approval dates are malformed"
            ) from error
        if expiry.tzinfo is None:
            raise AlfredError(
                AlfredErrorCode.UNAUTHORIZED, "approval expiry has no UTC offset"
            )
        storage_limit = document["storage_limit_bytes_decimal"]
        approval_id = document["approval_id"]
        if (
            type(storage_limit) is not int
            or not 1 <= storage_limit <= MAX_ALFRED_STORAGE_BYTES
            or not isinstance(approval_id, str)
            or not MIN_APPROVAL_ID_BYTES <= len(approval_id) <= MAX_APPROVAL_ID_BYTES
            or valid_through < valid_from
        ):
            raise AlfredError(
                AlfredErrorCode.UNAUTHORIZED, "ALFRED approval values are invalid"
            )
        return cls(
            approval_id=approval_id,
            allowed_series=tuple(allowed),
            valid_from=valid_from,
            valid_through=valid_through,
            expires_at_utc_ns=_utc_ns(expiry.astimezone(UTC)),
            storage_limit_bytes=storage_limit,
            api_execution_authorized=document["api_execution_authorized"] is True,
            approval_sha256=supplied_hash,
        )

    def require(self, config: AlfredConfig, now_utc_ns: int) -> None:
        """Require unexpired execution authority for every configured series."""
        if (
            not self.api_execution_authorized
            or now_utc_ns >= self.expires_at_utc_ns
            or config.vintage_start < self.valid_from
            or config.vintage_end > self.valid_through
            or config.storage_limit_bytes > self.storage_limit_bytes
            or not set(config.series_ids).issubset(self.allowed_series)
        ):
            raise AlfredError(
                AlfredErrorCode.UNAUTHORIZED,
                "ALFRED execution is outside the approved series, window, or lifetime",
            )


def approval_document_hash(document: Mapping[str, object]) -> str:
    """Return the canonical self-hash for an approval document body."""
    return _sha256(
        _canonical_bytes(
            {key: value for key, value in document.items() if key != "approval_sha256"}
        )
    )


@dataclass(frozen=True, slots=True)
class AlfredQueryPlan:
    """One deterministic, bounded series request plan."""

    series: AlfredSeriesSpec
    vintage_start: date
    vintage_end: date
    observation_start: date
    observation_end: date
    page_limit: int
    plan_id: str

    @classmethod
    def build(cls, config: AlfredConfig, series_id: str) -> Self:
        """Build and hash one exact, deterministic series query plan."""
        spec = reviewed_series(series_id)
        body = {
            "observation_end": config.observation_end.isoformat(),
            "observation_start": config.observation_start.isoformat(),
            "output_type": 1,
            "page_limit": config.page_limit,
            "series_id": spec.series_id,
            "units": "lin",
            "vintage_end": config.vintage_end.isoformat(),
            "vintage_start": config.vintage_start.isoformat(),
        }
        return cls(
            series=spec,
            vintage_start=config.vintage_start,
            vintage_end=config.vintage_end,
            observation_start=config.observation_start,
            observation_end=config.observation_end,
            page_limit=config.page_limit,
            plan_id="alfred-plan-" + _sha256(_canonical_bytes(body)),
        )


def build_query_plans(config: AlfredConfig) -> tuple[AlfredQueryPlan, ...]:
    """Build plans in caller-supplied series order."""
    return tuple(AlfredQueryPlan.build(config, item) for item in config.series_ids)


@dataclass(frozen=True, slots=True)
class AlfredHttpResponse:
    """Bounded transport result with local receipt time."""

    status: int
    body: bytes
    received_at_utc_ns: int
    headers: Mapping[str, str]


class AlfredTransport(Protocol):
    """Injected fixed-host transport boundary."""

    @property
    def network_access(self) -> bool:
        """Report whether the transport performs real network access."""
        ...

    def get(
        self,
        endpoint: str,
        parameters: Mapping[str, str],
        api_key: SecretValue,
        timeout_seconds: int,
    ) -> AlfredHttpResponse:
        """Issue one request without exposing the API key to callers."""
        ...


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    @override
    def redirect_request(
        self,
        req: urllib.request.Request,
        fp: object,
        code: int,
        msg: str,
        headers: object,
        newurl: str,
    ) -> urllib.request.Request | None:
        """Reject redirects so credentials never leave the fixed host."""
        del req, fp, code, msg, headers, newurl
        raise AlfredError(AlfredErrorCode.PROVIDER_UNAVAILABLE, "FRED redirect refused")


class UrllibAlfredTransport:
    """HTTPS-only FRED transport with a fixed host and disabled redirects."""

    def __init__(self, *, wall_clock_ns: Callable[[], int] = time.time_ns) -> None:
        """Build a fixed-host transport with an injectable receipt clock."""
        self._wall_clock_ns = wall_clock_ns
        self._opener = urllib.request.build_opener(_NoRedirect())

    @property
    def network_access(self) -> bool:
        """Report that this transport performs HTTPS requests."""
        return True

    def get(
        self,
        endpoint: str,
        parameters: Mapping[str, str],
        api_key: SecretValue,
        timeout_seconds: int,
    ) -> AlfredHttpResponse:
        """Fetch one allowlisted endpoint while containing the API key."""
        if endpoint not in {
            ALFRED_METADATA_ENDPOINT,
            ALFRED_RELEASE_ENDPOINT,
            ALFRED_RELEASE_DATES_ENDPOINT,
            ALFRED_OBSERVATIONS_ENDPOINT,
        }:
            raise AlfredError(
                AlfredErrorCode.INVALID_CONFIGURATION, "FRED endpoint is not allowed"
            )
        query = urllib.parse.urlencode({**parameters, "api_key": api_key.reveal()})
        url = f"https://{ALFRED_HOST}{endpoint}?{query}"
        request = urllib.request.Request(
            url,
            headers={"Accept": "application/json", "User-Agent": "Aegis-MX/0.1"},
            method="GET",
        )
        try:
            response = self._opener.open(request, timeout=timeout_seconds)
            with response:
                body = response.read(MAX_RESPONSE_BYTES + 1)
                return AlfredHttpResponse(
                    status=response.status,
                    body=body,
                    received_at_utc_ns=self._wall_clock_ns(),
                    headers=dict(response.headers.items()),
                )
        except urllib.error.HTTPError as error:
            body = error.read(MAX_RESPONSE_BYTES + 1)
            return AlfredHttpResponse(
                status=error.code,
                body=body,
                received_at_utc_ns=self._wall_clock_ns(),
                headers=dict(error.headers.items()),
            )
        except AlfredError:
            raise
        except (OSError, urllib.error.URLError, TimeoutError) as error:
            raise AlfredError(
                AlfredErrorCode.PROVIDER_UNAVAILABLE, "FRED HTTPS request failed"
            ) from error


@dataclass(frozen=True, slots=True)
class AlfredRawPage:
    """One successful response retained without its credential-bearing URL."""

    endpoint: str
    series_id: str
    offset: int
    body: bytes
    received_at_utc_ns: int

    @property
    def request_id(self) -> str:
        """Return a content-derived request identifier without query secrets."""
        return "alfred-request-" + _sha256(
            _canonical_bytes(
                {
                    "body_sha256": _sha256(self.body),
                    "endpoint": self.endpoint,
                    "offset": self.offset,
                    "series_id": self.series_id,
                }
            )
        )


@dataclass(frozen=True, slots=True)
class AlfredSeriesMetadata:
    """Validated current metadata for one reviewed series."""

    series_id: str
    title: str
    units: str
    frequency: str
    seasonal_adjustment: str
    last_updated_utc_ns: int
    notes_sha256: str
    release_id: int
    release_name: str

    def to_dict(self) -> dict[str, object]:
        """Return the validated provider metadata."""
        return {
            "frequency": self.frequency,
            "last_updated_utc_ns": self.last_updated_utc_ns,
            "notes_sha256": self.notes_sha256,
            "release_id": self.release_id,
            "release_name": self.release_name,
            "seasonal_adjustment": self.seasonal_adjustment,
            "series_id": self.series_id,
            "title": self.title,
            "units": self.units,
        }


@dataclass(frozen=True, slots=True)
class AlfredReleaseRecord:
    """Date-only release-calendar evidence; no intraday time is invented."""

    series_id: str
    release_id: int
    release_name: str
    release_date: date
    known_at_utc_ns: int
    record_sha256: str = ""

    def __post_init__(self) -> None:
        """Validate conservative date-only release semantics and self-hash."""
        if self.release_id <= 0 or self.known_at_utc_ns != _conservative_known_ns(
            self.release_date
        ):
            raise AlfredError(
                AlfredErrorCode.TIMESTAMP_INVALID, "release record time is invalid"
            )
        digest = _sha256(_canonical_bytes(self.to_payload()))
        if self.record_sha256 and self.record_sha256 != digest:
            raise AlfredError(
                AlfredErrorCode.MALFORMED_RESPONSE, "release record hash mismatch"
            )
        object.__setattr__(self, "record_sha256", digest)

    def to_payload(self) -> dict[str, object]:
        """Return the self-hash input payload."""
        return {
            "known_at_utc_ns": self.known_at_utc_ns,
            "release_date": self.release_date.isoformat(),
            "release_id": self.release_id,
            "release_name": self.release_name,
            "release_time_precision": ReleaseTimePrecision.DATE_ONLY.value,
            "release_time_utc_ns": None,
            "series_id": self.series_id,
        }

    def to_dict(self) -> dict[str, object]:
        """Return the immutable release record."""
        return {**self.to_payload(), "record_sha256": self.record_sha256}


@dataclass(frozen=True, slots=True)
class AlfredVintageRecord:
    """One immutable value version for one observation period."""

    series_id: str
    observation_date: date
    observation_time_utc_ns: int
    vintage_date: date
    revision_number: int
    vintage_kind: AlfredVintageKind
    value_status: AlfredValueStatus
    value_microunits: int | None
    native_units: str
    release_date: date | None
    release_time_precision: ReleaseTimePrecision
    known_at_utc_ns: int
    known_through_utc_ns: int | None
    local_receipt_time_utc_ns: int
    processing_time_utc_ns: int
    source_request_id: str
    record_sha256: str = ""

    def __post_init__(self) -> None:
        """Validate temporal, value, and revision invariants and self-hash."""
        if (
            self.observation_time_utc_ns != _date_start_ns(self.observation_date)
            or self.known_at_utc_ns != _conservative_known_ns(self.vintage_date)
            or self.revision_number <= 0
            or self.local_receipt_time_utc_ns <= 0
            or self.processing_time_utc_ns < self.local_receipt_time_utc_ns
            or (
                self.known_through_utc_ns is not None
                and self.known_through_utc_ns <= self.known_at_utc_ns
            )
            or (self.value_status is AlfredValueStatus.OBSERVED)
            != (self.value_microunits is not None)
            or (self.revision_number == 1)
            != (self.vintage_kind is AlfredVintageKind.INITIAL_RELEASE)
            or (self.release_date is None)
            != (self.release_time_precision is ReleaseTimePrecision.NOT_AVAILABLE)
        ):
            raise AlfredError(
                AlfredErrorCode.TIMESTAMP_INVALID, "vintage record invariants fail"
            )
        _bounded_text(self.source_request_id, "source_request_id", 96)
        digest = _sha256(_canonical_bytes(self.to_payload()))
        if self.record_sha256 and self.record_sha256 != digest:
            raise AlfredError(
                AlfredErrorCode.MALFORMED_RESPONSE, "vintage record hash mismatch"
            )
        object.__setattr__(self, "record_sha256", digest)

    def to_payload(self) -> dict[str, object]:
        """Return the self-hash input payload."""
        return {
            "known_at_utc_ns": self.known_at_utc_ns,
            "known_through_utc_ns": self.known_through_utc_ns,
            "local_receipt_time_utc_ns": self.local_receipt_time_utc_ns,
            "native_units": self.native_units,
            "observation_date": self.observation_date.isoformat(),
            "observation_time_utc_ns": self.observation_time_utc_ns,
            "processing_time_utc_ns": self.processing_time_utc_ns,
            "release_date": self.release_date.isoformat()
            if self.release_date is not None
            else None,
            "release_time_precision": self.release_time_precision.value,
            "release_time_utc_ns": None,
            "revision_number": self.revision_number,
            "series_id": self.series_id,
            "source_request_id": self.source_request_id,
            "value_microunits": self.value_microunits,
            "value_scale": MICRO_UNITS,
            "value_status": self.value_status.value,
            "vintage_date": self.vintage_date.isoformat(),
            "vintage_kind": self.vintage_kind.value,
        }

    def to_dict(self) -> dict[str, object]:
        """Return the immutable vintage record."""
        return {**self.to_payload(), "record_sha256": self.record_sha256}


@dataclass(frozen=True, slots=True)
class AlfredSeriesSnapshot:
    """Content-addressed metadata, releases, and complete in-scope vintages."""

    plan_id: str
    spec: AlfredSeriesSpec
    metadata: AlfredSeriesMetadata
    releases: tuple[AlfredReleaseRecord, ...]
    vintages: tuple[AlfredVintageRecord, ...]
    source_request_ids: tuple[str, ...]

    def __post_init__(self) -> None:
        """Validate series consistency and request provenance."""
        if self.metadata.series_id != self.spec.series_id or not self.vintages:
            raise AlfredError(
                AlfredErrorCode.MALFORMED_RESPONSE, "series snapshot is incomplete"
            )
        if len(set(self.source_request_ids)) != len(self.source_request_ids):
            raise AlfredError(
                AlfredErrorCode.MALFORMED_RESPONSE,
                "series snapshot request IDs are duplicated",
            )

    def to_payload(self) -> dict[str, object]:
        """Return the content-addressed snapshot payload."""
        return {
            "attribution": self.spec.attribution,
            "live_trading_capable": False,
            "metadata": self.metadata.to_dict(),
            "plan_id": self.plan_id,
            "releases": [item.to_dict() for item in self.releases],
            "schema_version": ALFRED_SCHEMA_VERSION,
            "series_contract": self.spec.to_dict(),
            "source_request_ids": list(self.source_request_ids),
            "vintages": [item.to_dict() for item in self.vintages],
        }

    @property
    def snapshot_sha256(self) -> str:
        """Return the snapshot content hash."""
        return _sha256(_canonical_bytes(self.to_payload()))

    def to_dict(self) -> dict[str, object]:
        """Return the snapshot plus its content hash."""
        return {**self.to_payload(), "snapshot_sha256": self.snapshot_sha256}


@dataclass(frozen=True, slots=True)
class _RawObservation:
    observation_date: date
    realtime_start: date
    realtime_end: date
    value_status: AlfredValueStatus
    value_microunits: int | None
    receipt_time_utc_ns: int
    request_id: str


def _json_object(page: AlfredRawPage) -> dict[str, object]:
    if len(page.body) > MAX_RESPONSE_BYTES:
        raise AlfredError(
            AlfredErrorCode.RESPONSE_TOO_LARGE, "FRED response exceeds byte limit"
        )
    try:
        value = json.loads(page.body)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AlfredError(
            AlfredErrorCode.MALFORMED_RESPONSE, "FRED response is not valid JSON"
        ) from error
    if not isinstance(value, dict):
        raise AlfredError(
            AlfredErrorCode.MALFORMED_RESPONSE, "FRED response is not an object"
        )
    return cast("dict[str, object]", value)


def _closed_keys(
    value: Mapping[str, object], required: set[str], optional: set[str] | None = None
) -> None:
    allowed = required | (optional or set())
    if not required.issubset(value) or not set(value).issubset(allowed):
        raise AlfredError(
            AlfredErrorCode.MALFORMED_RESPONSE, "FRED response fields changed"
        )


def _object_list(value: object, field: str) -> list[dict[str, object]]:
    if not isinstance(value, list) or len(value) > MAX_ROWS_PER_SERIES:
        raise AlfredError(
            AlfredErrorCode.RESPONSE_TOO_LARGE, f"{field} is absent or too large"
        )
    if not all(isinstance(item, dict) for item in value):
        raise AlfredError(
            AlfredErrorCode.MALFORMED_RESPONSE, f"{field} has a non-object row"
        )
    return cast("list[dict[str, object]]", value)


def _integer(value: object, field: str, *, minimum: int = 0) -> int:
    if type(value) is not int or value < minimum or value > MAX_INT64:
        raise AlfredError(
            AlfredErrorCode.MALFORMED_RESPONSE, f"{field} is not a bounded integer"
        )
    return value


def _string(value: object, field: str, maximum: int = MAX_TEXT_BYTES) -> str:
    return _bounded_text(cast("str", value), field, maximum)


def _parse_metadata(
    page: AlfredRawPage, spec: AlfredSeriesSpec
) -> tuple[dict[str, object], str]:
    document = _json_object(page)
    _closed_keys(document, {"realtime_start", "realtime_end", "seriess"})
    rows = _object_list(document["seriess"], "seriess")
    if len(rows) != 1:
        raise AlfredError(
            AlfredErrorCode.METADATA_MISMATCH, "series metadata is not singular"
        )
    row = rows[0]
    required = {
        "frequency",
        "frequency_short",
        "id",
        "last_updated",
        "observation_end",
        "observation_start",
        "popularity",
        "realtime_end",
        "realtime_start",
        "seasonal_adjustment",
        "seasonal_adjustment_short",
        "title",
        "units",
        "units_short",
    }
    _closed_keys(row, required, {"notes"})
    notes = row.get("notes", "")
    if not isinstance(notes, str) or len(notes.encode("utf-8")) > MAX_NOTES_BYTES:
        raise AlfredError(
            AlfredErrorCode.RESPONSE_TOO_LARGE,
            "series notes are malformed or too large",
        )
    if "copyright" in notes.casefold():
        raise AlfredError(
            AlfredErrorCode.RESTRICTED_SERIES,
            "series metadata contains a copyright restriction",
        )
    expected = {
        "frequency": spec.frequency,
        "id": spec.series_id,
        "seasonal_adjustment": spec.seasonal_adjustment,
        "title": spec.title,
        "units": spec.units,
    }
    if any(row.get(key) != value for key, value in expected.items()):
        raise AlfredError(
            AlfredErrorCode.METADATA_MISMATCH,
            "series metadata differs from the reviewed contract",
        )
    return row, _sha256(notes.encode("utf-8"))


def _parse_release(page: AlfredRawPage, spec: AlfredSeriesSpec) -> tuple[int, str]:
    document = _json_object(page)
    _closed_keys(document, {"realtime_start", "realtime_end", "releases"})
    rows = _object_list(document["releases"], "releases")
    if len(rows) != 1:
        raise AlfredError(
            AlfredErrorCode.METADATA_MISMATCH, "series release is not singular"
        )
    row = rows[0]
    _closed_keys(
        row,
        {"id", "name", "press_release", "realtime_end", "realtime_start"},
        {"link", "notes"},
    )
    release_id = _integer(row["id"], "release id", minimum=1)
    release_name = _string(row["name"], "release name")
    if release_name != spec.release_name or type(row["press_release"]) is not bool:
        raise AlfredError(
            AlfredErrorCode.METADATA_MISMATCH,
            "series release differs from the reviewed contract",
        )
    return release_id, release_name


def _pagination(document: Mapping[str, object], field: str) -> tuple[int, int, int]:
    count = _integer(document.get("count"), "count")
    offset = _integer(document.get("offset"), "offset")
    limit = _integer(document.get("limit"), "limit", minimum=1)
    if count > MAX_ROWS_PER_SERIES or limit > MAX_PAGE_LIMIT:
        raise AlfredError(
            AlfredErrorCode.RESPONSE_TOO_LARGE, f"{field} pagination exceeds bounds"
        )
    return count, offset, limit


def _parse_release_dates_page(
    page: AlfredRawPage, release_id: int
) -> tuple[int, int, tuple[date, ...]]:
    document = _json_object(page)
    _closed_keys(
        document,
        {
            "count",
            "limit",
            "offset",
            "order_by",
            "realtime_end",
            "realtime_start",
            "release_dates",
            "sort_order",
        },
    )
    count, offset, _ = _pagination(document, "release dates")
    rows = _object_list(document["release_dates"], "release_dates")
    dates: list[date] = []
    for row in rows:
        _closed_keys(
            row, {"date", "release_id"}, {"release_name", "release_last_updated"}
        )
        if _integer(row["release_id"], "release id", minimum=1) != release_id:
            raise AlfredError(
                AlfredErrorCode.METADATA_MISMATCH, "release date ID mismatch"
            )
        dates.append(_parse_date(row["date"], "release date"))
    if dates != sorted(dates) or len(set(dates)) != len(dates):
        raise AlfredError(
            AlfredErrorCode.PAGINATION_INVALID,
            "release dates are not strictly ascending",
        )
    return count, offset, tuple(dates)


def _parse_observations_page(
    page: AlfredRawPage, plan: AlfredQueryPlan
) -> tuple[int, int, tuple[_RawObservation, ...]]:
    document = _json_object(page)
    _closed_keys(
        document,
        {
            "count",
            "file_type",
            "limit",
            "observation_end",
            "observation_start",
            "observations",
            "offset",
            "order_by",
            "output_type",
            "realtime_end",
            "realtime_start",
            "sort_order",
            "units",
        },
    )
    if (
        document["file_type"] != "json"
        or document["units"] != "lin"
        or document["output_type"] != 1
        or document["sort_order"] != "asc"
    ):
        raise AlfredError(
            AlfredErrorCode.METADATA_MISMATCH, "observation response semantics changed"
        )
    count, offset, _ = _pagination(document, "observations")
    rows = _object_list(document["observations"], "observations")
    parsed: list[_RawObservation] = []
    for row in rows:
        _closed_keys(row, {"date", "realtime_end", "realtime_start", "value"})
        observation_date = _parse_date(row["date"], "observation date")
        realtime_start = _parse_date(row["realtime_start"], "realtime_start")
        realtime_end = _parse_date(row["realtime_end"], "realtime_end")
        if (
            not plan.observation_start <= observation_date <= plan.observation_end
            or realtime_end < realtime_start
            or realtime_end < plan.vintage_start
            or realtime_start > plan.vintage_end
        ):
            raise AlfredError(
                AlfredErrorCode.TIMESTAMP_INVALID,
                "observation is outside the requested point-in-time window",
            )
        value_status, value = _parse_micro_units(_string(row["value"], "value", 40))
        parsed.append(
            _RawObservation(
                observation_date=observation_date,
                realtime_start=realtime_start,
                realtime_end=realtime_end,
                value_status=value_status,
                value_microunits=value,
                receipt_time_utc_ns=page.received_at_utc_ns,
                request_id=page.request_id,
            )
        )
    ordering = [(item.observation_date, item.realtime_start) for item in parsed]
    if ordering != sorted(ordering):
        raise AlfredError(
            AlfredErrorCode.PAGINATION_INVALID,
            "observations are not in deterministic order",
        )
    return count, offset, tuple(parsed)


@dataclass(frozen=True, slots=True)
class AlfredFetchedSeries:
    """Validated raw pages and parsed values for one plan."""

    metadata_row: Mapping[str, object]
    notes_sha256: str
    release_id: int
    release_name: str
    release_dates: tuple[date, ...]
    observations: tuple[_RawObservation, ...]
    pages: tuple[AlfredRawPage, ...]


class AlfredClient:
    """Bounded FRED API v1 client with deterministic retry and throttling."""

    def __init__(
        self,
        config: AlfredConfig,
        api_key: SecretValue,
        transport: AlfredTransport,
        *,
        monotonic_ns: Callable[[], int] = time.monotonic_ns,
        sleeper: Callable[[float], None] = time.sleep,
    ) -> None:
        """Construct a bounded client with injectable clocks and transport."""
        self.config = config
        self._api_key = api_key
        self.transport = transport
        self._monotonic_ns = monotonic_ns
        self._sleeper = sleeper
        self._next_request_ns = 0
        self.request_count = 0
        self.retry_count = 0

    def _throttle(self) -> None:
        now = self._monotonic_ns()
        if now < self._next_request_ns:
            self._sleeper((self._next_request_ns - now) / 1_000_000_000)
            now = self._monotonic_ns()
        interval = 60_000_000_000 // self.config.requests_per_minute
        self._next_request_ns = max(now, self._next_request_ns) + interval

    def _request(
        self, endpoint: str, parameters: Mapping[str, str], *, offset: int = 0
    ) -> AlfredRawPage:
        for attempt in range(1, self.config.retry_attempts + 1):  # pragma: no branch
            if self.request_count >= self.config.max_requests:
                raise AlfredError(
                    AlfredErrorCode.RETRY_EXHAUSTED, "FRED request budget exhausted"
                )
            self._throttle()
            response = self.transport.get(
                endpoint, parameters, self._api_key, self.config.timeout_seconds
            )
            self.request_count += 1
            if len(response.body) > self.config.response_limit_bytes:
                raise AlfredError(
                    AlfredErrorCode.RESPONSE_TOO_LARGE,
                    "FRED response exceeds configured byte limit",
                )
            if response.status == HTTP_OK:
                return AlfredRawPage(
                    endpoint=endpoint,
                    series_id=parameters.get("series_id", "_RELEASE"),
                    offset=offset,
                    body=response.body,
                    received_at_utc_ns=response.received_at_utc_ns,
                )
            if response.status not in {423, 429, 500}:
                raise AlfredError(
                    AlfredErrorCode.PROVIDER_UNAVAILABLE,
                    f"FRED returned HTTP {response.status}",
                )
            if attempt == self.config.retry_attempts:
                code = (
                    AlfredErrorCode.RATE_LIMITED
                    if response.status == HTTP_TOO_MANY_REQUESTS
                    else AlfredErrorCode.RETRY_EXHAUSTED
                )
                raise AlfredError(code, "FRED bounded retries exhausted")
            self.retry_count += 1
            retry_after = response.headers.get("Retry-After", "1")
            delay = int(retry_after) if retry_after.isdigit() else 1
            self._sleeper(float(min(max(delay, 1), 30)))
        message = "bounded retry loop exhausted unexpectedly"  # pragma: no cover
        raise AssertionError(message)  # pragma: no cover

    def _metadata(
        self, plan: AlfredQueryPlan
    ) -> tuple[AlfredRawPage, dict[str, object], str]:
        page = self._request(
            ALFRED_METADATA_ENDPOINT,
            {"file_type": "json", "series_id": plan.series.series_id},
        )
        row, notes_hash = _parse_metadata(page, plan.series)
        return page, row, notes_hash

    def _release(self, plan: AlfredQueryPlan) -> tuple[AlfredRawPage, int, str]:
        page = self._request(
            ALFRED_RELEASE_ENDPOINT,
            {"file_type": "json", "series_id": plan.series.series_id},
        )
        release_id, release_name = _parse_release(page, plan.series)
        return page, release_id, release_name

    def _release_dates(
        self, plan: AlfredQueryPlan, release_id: int
    ) -> tuple[tuple[AlfredRawPage, ...], tuple[date, ...]]:
        pages: list[AlfredRawPage] = []
        dates: list[date] = []
        expected_count: int | None = None
        offset = 0
        while True:
            if len(pages) >= self.config.max_pages_per_endpoint:
                raise AlfredError(
                    AlfredErrorCode.PAGINATION_INVALID,
                    "release-date page bound exhausted",
                )
            page = self._request(
                ALFRED_RELEASE_DATES_ENDPOINT,
                {
                    "file_type": "json",
                    "include_release_dates_with_no_data": "true",
                    "limit": str(plan.page_limit),
                    "offset": str(offset),
                    "realtime_end": plan.vintage_end.isoformat(),
                    "realtime_start": plan.vintage_start.isoformat(),
                    "release_id": str(release_id),
                    "sort_order": "asc",
                },
                offset=offset,
            )
            count, returned_offset, values = _parse_release_dates_page(page, release_id)
            if returned_offset != offset or (
                expected_count is not None and count != expected_count
            ):
                raise AlfredError(
                    AlfredErrorCode.PAGINATION_INVALID,
                    "release-date pagination changed during retrieval",
                )
            expected_count = count
            pages.append(page)
            dates.extend(values)
            offset += len(values)
            if offset >= count:
                break
            if not values:
                raise AlfredError(
                    AlfredErrorCode.PAGINATION_INVALID,
                    "release-date pagination made no progress",
                )
        if dates != sorted(set(dates)):
            raise AlfredError(
                AlfredErrorCode.PAGINATION_INVALID,
                "release dates conflict across pages",
            )
        return tuple(pages), tuple(dates)

    def _observations(
        self, plan: AlfredQueryPlan
    ) -> tuple[tuple[AlfredRawPage, ...], tuple[_RawObservation, ...]]:
        pages: list[AlfredRawPage] = []
        observations: list[_RawObservation] = []
        expected_count: int | None = None
        offset = 0
        while True:
            if len(pages) >= self.config.max_pages_per_endpoint:
                raise AlfredError(
                    AlfredErrorCode.PAGINATION_INVALID,
                    "observation page bound exhausted",
                )
            page = self._request(
                ALFRED_OBSERVATIONS_ENDPOINT,
                {
                    "file_type": "json",
                    "limit": str(plan.page_limit),
                    "observation_end": plan.observation_end.isoformat(),
                    "observation_start": plan.observation_start.isoformat(),
                    "offset": str(offset),
                    "output_type": "1",
                    "realtime_end": plan.vintage_end.isoformat(),
                    "realtime_start": plan.vintage_start.isoformat(),
                    "series_id": plan.series.series_id,
                    "sort_order": "asc",
                    "units": "lin",
                },
                offset=offset,
            )
            count, returned_offset, values = _parse_observations_page(page, plan)
            if returned_offset != offset or (
                expected_count is not None and count != expected_count
            ):
                raise AlfredError(
                    AlfredErrorCode.PAGINATION_INVALID,
                    "observation pagination changed during retrieval",
                )
            expected_count = count
            pages.append(page)
            observations.extend(values)
            offset += len(values)
            if offset >= count:
                break
            if not values:
                raise AlfredError(
                    AlfredErrorCode.PAGINATION_INVALID,
                    "observation pagination made no progress",
                )
        ordering = [
            (item.observation_date, item.realtime_start) for item in observations
        ]
        if ordering != sorted(ordering):
            raise AlfredError(
                AlfredErrorCode.PAGINATION_INVALID,
                "observation ordering conflicts across pages",
            )
        return tuple(pages), tuple(observations)

    def fetch_series(self, plan: AlfredQueryPlan) -> AlfredFetchedSeries:
        """Fetch and validate every in-scope page for one reviewed series."""
        metadata_page, metadata, notes_hash = self._metadata(plan)
        release_page, release_id, release_name = self._release(plan)
        release_pages, release_dates = self._release_dates(plan, release_id)
        observation_pages, observations = self._observations(plan)
        if not observations:
            raise AlfredError(
                AlfredErrorCode.MALFORMED_RESPONSE,
                "series has no observations in the requested window",
            )
        return AlfredFetchedSeries(
            metadata_row=metadata,
            notes_sha256=notes_hash,
            release_id=release_id,
            release_name=release_name,
            release_dates=release_dates,
            observations=observations,
            pages=(metadata_page, release_page, *release_pages, *observation_pages),
        )


class ScriptedAlfredTransport:
    """Socket-free FIFO transport for deterministic integration tests."""

    def __init__(
        self,
        responses: Mapping[str, Sequence[AlfredHttpResponse | Exception]],
    ) -> None:
        """Copy deterministic endpoint response queues."""
        self._responses = {key: list(value) for key, value in responses.items()}
        self.requests: list[tuple[str, dict[str, str]]] = []

    @property
    def network_access(self) -> bool:
        """Report that this transport never opens a socket."""
        return False

    def get(
        self,
        endpoint: str,
        parameters: Mapping[str, str],
        api_key: SecretValue,
        timeout_seconds: int,
    ) -> AlfredHttpResponse:
        """Return the next scripted response for the requested endpoint."""
        del api_key, timeout_seconds
        self.requests.append((endpoint, dict(parameters)))
        queue = self._responses.get(endpoint, [])
        if not queue:
            raise AlfredError(
                AlfredErrorCode.PROVIDER_UNAVAILABLE,
                "scripted FRED response is unavailable",
            )
        outcome = queue.pop(0)
        if isinstance(outcome, Exception):
            raise outcome
        return outcome


def build_series_snapshot(
    plan: AlfredQueryPlan,
    fetched: AlfredFetchedSeries,
    *,
    processing_time_utc_ns: int,
) -> AlfredSeriesSnapshot:
    """Deduplicate, validate lineage, and number all immutable vintages."""
    metadata = AlfredSeriesMetadata(
        series_id=plan.series.series_id,
        title=plan.series.title,
        units=plan.series.units,
        frequency=plan.series.frequency,
        seasonal_adjustment=plan.series.seasonal_adjustment,
        last_updated_utc_ns=_parse_updated_ns(fetched.metadata_row["last_updated"]),
        notes_sha256=fetched.notes_sha256,
        release_id=fetched.release_id,
        release_name=fetched.release_name,
    )
    releases = tuple(
        AlfredReleaseRecord(
            series_id=plan.series.series_id,
            release_id=fetched.release_id,
            release_name=fetched.release_name,
            release_date=item,
            known_at_utc_ns=_conservative_known_ns(item),
        )
        for item in fetched.release_dates
    )
    release_set = set(fetched.release_dates)
    unique: dict[tuple[date, date], _RawObservation] = {}
    for item in fetched.observations:
        key = (item.observation_date, item.realtime_start)
        previous = unique.get(key)
        if previous is not None and (
            previous.value_status is not item.value_status
            or previous.value_microunits != item.value_microunits
            or previous.realtime_end != item.realtime_end
        ):
            raise AlfredError(
                AlfredErrorCode.CONFLICTING_VINTAGE,
                "same ALFRED vintage contains conflicting values",
            )
        unique[key] = previous or item
    by_observation: dict[date, list[_RawObservation]] = {}
    for item in unique.values():
        by_observation.setdefault(item.observation_date, []).append(item)
    records: list[AlfredVintageRecord] = []
    for observation_date in sorted(by_observation):
        chain = sorted(
            by_observation[observation_date], key=lambda item: item.realtime_start
        )
        prior_known_through = 0
        for index, item in enumerate(chain, start=1):
            known_at = _conservative_known_ns(item.realtime_start)
            known_through = _known_through_ns(item.realtime_end)
            if known_at < prior_known_through:
                raise AlfredError(
                    AlfredErrorCode.CONFLICTING_VINTAGE,
                    "ALFRED vintage validity intervals overlap",
                )
            prior_known_through = known_through or MAX_INT64
            matched_release = (
                item.realtime_start if item.realtime_start in release_set else None
            )
            records.append(
                AlfredVintageRecord(
                    series_id=plan.series.series_id,
                    observation_date=observation_date,
                    observation_time_utc_ns=_date_start_ns(observation_date),
                    vintage_date=item.realtime_start,
                    revision_number=index,
                    vintage_kind=(
                        AlfredVintageKind.INITIAL_RELEASE
                        if index == 1
                        else AlfredVintageKind.REVISION
                    ),
                    value_status=item.value_status,
                    value_microunits=item.value_microunits,
                    native_units=plan.series.units,
                    release_date=matched_release,
                    release_time_precision=(
                        ReleaseTimePrecision.DATE_ONLY
                        if matched_release is not None
                        else ReleaseTimePrecision.NOT_AVAILABLE
                    ),
                    known_at_utc_ns=known_at,
                    known_through_utc_ns=known_through,
                    local_receipt_time_utc_ns=item.receipt_time_utc_ns,
                    processing_time_utc_ns=processing_time_utc_ns,
                    source_request_id=item.request_id,
                )
            )
    return AlfredSeriesSnapshot(
        plan_id=plan.plan_id,
        spec=plan.series,
        metadata=metadata,
        releases=releases,
        vintages=tuple(records),
        source_request_ids=tuple(page.request_id for page in fetched.pages),
    )


class AlfredVintageStore:
    """Deterministic in-memory reference implementation for as-known-at queries."""

    def __init__(self, records: Sequence[AlfredVintageRecord]) -> None:
        """Validate and deterministically index immutable vintage records."""
        ordered = sorted(
            records,
            key=lambda item: (
                item.series_id,
                item.observation_date,
                item.known_at_utc_ns,
                item.revision_number,
                item.record_sha256,
            ),
        )
        identities: set[tuple[str, date, int]] = set()
        for item in ordered:
            identity = (item.series_id, item.observation_date, item.revision_number)
            if identity in identities:
                raise AlfredError(
                    AlfredErrorCode.CONFLICTING_VINTAGE,
                    "duplicate revision identity in vintage store",
                )
            identities.add(identity)
        self._records = tuple(ordered)

    def as_known_at(
        self, timestamp_utc_ns: int, *, series_id: str | None = None
    ) -> tuple[AlfredVintageRecord, ...]:
        """Return the latest available revision of each observation inclusively."""
        return self._query(timestamp_utc_ns, series_id=series_id, strict=False)

    def latest_available_before(
        self, timestamp_utc_ns: int, *, series_id: str | None = None
    ) -> tuple[AlfredVintageRecord, ...]:
        """Return latest revisions using a strict known-time boundary."""
        return self._query(timestamp_utc_ns, series_id=series_id, strict=True)

    def _query(
        self, timestamp_utc_ns: int, *, series_id: str | None, strict: bool
    ) -> tuple[AlfredVintageRecord, ...]:
        if timestamp_utc_ns <= 0:
            raise AlfredError(
                AlfredErrorCode.TIMESTAMP_INVALID, "as-known-at time must be positive"
            )
        if series_id is not None:
            reviewed_series(series_id)
        selected: dict[tuple[str, date], AlfredVintageRecord] = {}
        for item in self._records:
            if series_id is not None and item.series_id != series_id:
                continue
            available = (
                item.known_at_utc_ns < timestamp_utc_ns
                if strict
                else item.known_at_utc_ns <= timestamp_utc_ns
            )
            still_current = (
                item.known_through_utc_ns is None
                or timestamp_utc_ns < item.known_through_utc_ns
            )
            if available and still_current:
                selected[(item.series_id, item.observation_date)] = item
        return tuple(selected[key] for key in sorted(selected))

    def revisions_after(self, timestamp_utc_ns: int) -> tuple[AlfredVintageRecord, ...]:
        """Return immutable revisions strictly after the supplied known time."""
        if timestamp_utc_ns <= 0:
            raise AlfredError(
                AlfredErrorCode.TIMESTAMP_INVALID, "revision cutoff must be positive"
            )
        return tuple(
            item for item in self._records if item.known_at_utc_ns > timestamp_utc_ns
        )

    def require_no_future(
        self, records: Sequence[AlfredVintageRecord], cutoff_ns: int
    ) -> None:
        """Reject a dataset containing a vintage unavailable at its cutoff."""
        if any(item.known_at_utc_ns > cutoff_ns for item in records):
            raise AlfredError(
                AlfredErrorCode.FUTURE_LEAKAGE,
                "dataset includes a future ALFRED revision",
            )


@dataclass(frozen=True, slots=True)
class AlfredArtifactManifest:
    """Immutable content-addressed ALFRED object manifest."""

    approval_id: str
    series_universe_sha256: str
    series_id: str
    plan_id: str
    object_kind: AlfredObjectKind
    storage_path: str
    object_sha256: str
    size_bytes: int
    record_count: int
    event_time_min_ns: int
    event_time_max_ns: int
    revision_time_min_ns: int
    revision_time_max_ns: int
    receipt_time_utc_ns: int
    processing_time_utc_ns: int

    def to_payload(self) -> dict[str, object]:
        """Return the immutable manifest payload."""
        return {
            "approval_id": self.approval_id,
            "attribution": ALFRED_ATTRIBUTION,
            "event_time_max_ns": self.event_time_max_ns,
            "event_time_min_ns": self.event_time_min_ns,
            "live_trading_capable": False,
            "object_kind": self.object_kind.value,
            "object_sha256": self.object_sha256,
            "plan_id": self.plan_id,
            "processing_time_utc_ns": self.processing_time_utc_ns,
            "receipt_time_utc_ns": self.receipt_time_utc_ns,
            "record_count": self.record_count,
            "revision_time_max_ns": self.revision_time_max_ns,
            "revision_time_min_ns": self.revision_time_min_ns,
            "schema_version": ALFRED_SCHEMA_VERSION,
            "series_id": self.series_id,
            "series_universe_sha256": self.series_universe_sha256,
            "size_bytes_decimal": self.size_bytes,
            "source_revision": ALFRED_SOURCE_REVISION,
            "storage_path": self.storage_path,
        }

    @property
    def manifest_sha256(self) -> str:
        """Return the manifest content hash."""
        return _sha256(_canonical_bytes(self.to_payload()))

    @property
    def manifest_id(self) -> str:
        """Return the content-addressed manifest identifier."""
        return "alfred-" + self.manifest_sha256

    def encode(self) -> bytes:
        """Serialize the complete manifest canonically."""
        return (
            _canonical_bytes(
                {
                    **self.to_payload(),
                    "manifest_id": self.manifest_id,
                    "manifest_sha256": self.manifest_sha256,
                }
            )
            + b"\n"
        )


def _write_all(descriptor: int, payload: bytes) -> None:
    offset = 0
    while offset < len(payload):
        written = os.write(descriptor, payload[offset:])
        if written <= 0:
            raise AlfredError(AlfredErrorCode.STORAGE_LIMIT, "ALFRED write was partial")
        offset += written


class AlfredRepository:
    """Single-writer immutable publication under global and sub-1 GB caps."""

    def __init__(
        self,
        repository: DataRepository,
        quota: QuotaEvidence,
        authorization: AlfredAuthorization,
        *,
        series_universe_hash: str,
    ) -> None:
        """Bind repository writes to quota, approval, and universe evidence."""
        self.repository = repository
        self.quota = quota
        self.authorization = authorization
        self.series_universe_hash = _require_sha256(
            series_universe_hash, "series universe hash"
        )
        self.storage_limit_bytes = authorization.storage_limit_bytes
        self._lock = threading.Lock()
        self._batch_lock = threading.Lock()
        self._batch_lease: AdmissionLease | None = None

    def usage_bytes(self) -> int:
        """Count every ALFRED object, manifest, report, and partial."""
        roots = (
            self.repository.root / "raw/alfred",
            self.repository.root / "canonical/alfred",
            self.repository.root / "reports/alfred",
            self.repository.root / "manifests/alfred",
        )
        total = 0

        def count(path: Path) -> None:
            nonlocal total
            status = os.lstat(path)
            if not stat.S_ISREG(status.st_mode) or stat.S_ISLNK(status.st_mode):
                raise AlfredError(
                    AlfredErrorCode.STORAGE_LIMIT, "ALFRED tree is unsafe"
                )
            total += status.st_size
            if total >= self.storage_limit_bytes:
                raise AlfredError(
                    AlfredErrorCode.STORAGE_LIMIT, "ALFRED storage cap is exhausted"
                )

        for root in roots:
            if not root.exists():
                continue
            if root.is_symlink():
                raise AlfredError(
                    AlfredErrorCode.STORAGE_LIMIT, "ALFRED root is a symlink"
                )
            for directory, names, files in os.walk(root, followlinks=False):
                if any((Path(directory) / name).is_symlink() for name in names):
                    raise AlfredError(
                        AlfredErrorCode.STORAGE_LIMIT, "ALFRED tree has a symlink"
                    )
                for filename in files:
                    count(Path(directory) / filename)
        temporary = self.repository.root / "tmp"
        for path in temporary.iterdir():
            if path.name.startswith("alfred-"):
                count(path)
        return total

    @contextmanager
    def publication_batch(self) -> Iterator[None]:
        """Reserve remaining global capacity and retain the process fence."""
        with self._batch_lock:
            remaining = self.storage_limit_bytes - self.usage_bytes() - 1
            if remaining <= 0:
                raise AlfredError(
                    AlfredErrorCode.STORAGE_LIMIT, "ALFRED storage cap is exhausted"
                )
            request = StorageRequest(
                "alfred-bounded-publication-batch", remaining, MAX_RESPONSE_BYTES
            )
            try:
                with self.repository.acquire_admission(request, self.quota) as lease:
                    self._batch_lease = lease
                    try:
                        yield
                    finally:
                        self._batch_lease = None
            except StorageError as error:
                raise AlfredError(
                    AlfredErrorCode.STORAGE_LIMIT, "ALFRED storage admission failed"
                ) from error

    def publish(
        self,
        *,
        series_id: str,
        plan_id: str,
        object_kind: AlfredObjectKind,
        payload: bytes,
        record_count: int,
        event_time_min_ns: int,
        event_time_max_ns: int,
        revision_time_min_ns: int,
        revision_time_max_ns: int,
        receipt_time_utc_ns: int,
        processing_time_utc_ns: int,
    ) -> AlfredArtifactManifest:
        """Atomically publish one object and its immutable custom manifest."""
        if not payload or record_count <= 0 or len(payload) > MAX_RESPONSE_BYTES:
            raise AlfredError(
                AlfredErrorCode.STORAGE_LIMIT, "ALFRED publication is invalid"
            )
        with self._lock:
            digest = _sha256(payload)
            area = (
                "canonical"
                if object_kind is AlfredObjectKind.CANONICAL_SNAPSHOT
                else "reports"
                if object_kind
                in {
                    AlfredObjectKind.RUN_REPORT_JSON,
                    AlfredObjectKind.RUN_REPORT_MARKDOWN,
                }
                else "raw"
            )
            suffix = (
                "md" if object_kind is AlfredObjectKind.RUN_REPORT_MARKDOWN else "json"
            )
            safe_series = series_id.lower() if series_id != "_ALL" else "all"
            final_relative = (
                f"{area}/alfred/{safe_series}/"
                f"{object_kind.value.lower()}/{digest}.{suffix}"
            )
            manifest = AlfredArtifactManifest(
                approval_id=self.authorization.approval_id,
                series_universe_sha256=self.series_universe_hash,
                series_id=series_id,
                plan_id=plan_id,
                object_kind=object_kind,
                storage_path=final_relative,
                object_sha256=digest,
                size_bytes=len(payload),
                record_count=record_count,
                event_time_min_ns=event_time_min_ns,
                event_time_max_ns=event_time_max_ns,
                revision_time_min_ns=revision_time_min_ns,
                revision_time_max_ns=revision_time_max_ns,
                receipt_time_utc_ns=receipt_time_utc_ns,
                processing_time_utc_ns=processing_time_utc_ns,
            )
            manifest_bytes = manifest.encode()
            required = len(payload) + len(manifest_bytes)
            if self.usage_bytes() + required >= self.storage_limit_bytes:
                raise AlfredError(
                    AlfredErrorCode.STORAGE_LIMIT,
                    "ALFRED object would reach or exceed the sub-1 GB cap",
                )
            if self._batch_lease is not None:
                self._publish_admitted(
                    payload, final_relative, manifest, self._batch_lease
                )
            else:
                request = StorageRequest("alfred-" + digest[:24], required, required)
                try:
                    with self.repository.acquire_admission(
                        request, self.quota
                    ) as lease:
                        if self.usage_bytes() + required >= self.storage_limit_bytes:
                            raise AlfredError(
                                AlfredErrorCode.STORAGE_LIMIT,
                                "concurrent ALFRED publication exhausted the cap",
                            )
                        self._publish_admitted(payload, final_relative, manifest, lease)
                except StorageError as error:
                    raise AlfredError(
                        AlfredErrorCode.STORAGE_LIMIT,
                        "ALFRED storage admission failed",
                    ) from error
            return manifest

    def _publish_admitted(
        self,
        payload: bytes,
        final_relative: str,
        manifest: AlfredArtifactManifest,
        lease: AdmissionLease,
    ) -> None:
        digest = manifest.object_sha256
        staged_relative = f"tmp/alfred-{digest}.partial"
        staged = self.repository.root / staged_relative
        self._stage(staged, payload)
        try:
            self.repository.publish_staged_object(
                staged_relative,
                final_relative,
                expected_sha256=digest,
                expected_size_bytes=len(payload),
                lease=lease,
            )
        except StorageError as error:
            raise AlfredError(
                AlfredErrorCode.STORAGE_LIMIT, "ALFRED object publication failed"
            ) from error
        self._publish_manifest(manifest)

    @staticmethod
    def _stage(path: Path, payload: bytes) -> None:
        try:
            descriptor = os.open(
                path,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
                0o400,
            )
        except FileExistsError:
            if path.is_symlink() or path.read_bytes() != payload:
                raise AlfredError(
                    AlfredErrorCode.STORAGE_LIMIT, "staged ALFRED object conflicts"
                ) from None
            return
        try:
            _write_all(descriptor, payload)
            os.fsync(descriptor)
        finally:
            os.close(descriptor)

    def _publish_manifest(self, manifest: AlfredArtifactManifest) -> None:
        encoded = manifest.encode()
        directory = self.repository.root / "manifests/alfred"
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        final = directory / f"{manifest.manifest_id}.json"
        if final.exists():
            if final.is_symlink() or final.read_bytes() != encoded:
                raise AlfredError(
                    AlfredErrorCode.STORAGE_LIMIT, "ALFRED manifest conflicts"
                )
            return
        staged = self.repository.root / f"tmp/alfred-{manifest.manifest_id}.partial"
        self._stage(staged, encoded)
        try:
            os.link(staged, final, follow_symlinks=False)
        except FileExistsError as error:
            raise AlfredError(
                AlfredErrorCode.STORAGE_LIMIT,
                "ALFRED manifest appeared concurrently",
            ) from error
        finally:
            if staged.exists() and not staged.is_symlink():
                staged.unlink()


@dataclass(frozen=True, slots=True)
class AlfredRunReport:
    """Machine-readable series coverage, quality, requests, and storage evidence."""

    run_id: str
    series_universe_sha256: str
    vintage_start: date
    vintage_end: date
    observation_start: date
    observation_end: date
    generated_at_utc_ns: int
    network_access_performed: bool
    request_count: int
    retry_count: int
    snapshots: tuple[AlfredSeriesSnapshot, ...]
    manifest_ids: tuple[str, ...]
    usage_before_bytes: int
    usage_after_bytes: int
    storage_limit_bytes: int

    def to_payload(self) -> dict[str, object]:
        """Summarize deterministic coverage and data-quality evidence."""
        initial = sum(
            item.vintage_kind is AlfredVintageKind.INITIAL_RELEASE
            for snapshot in self.snapshots
            for item in snapshot.vintages
        )
        revisions = sum(len(snapshot.vintages) for snapshot in self.snapshots) - initial
        missing_values = sum(
            item.value_status is AlfredValueStatus.MISSING
            for snapshot in self.snapshots
            for item in snapshot.vintages
        )
        missing_release = sum(
            len(
                {item.release_date for item in snapshot.releases}
                - {item.vintage_date for item in snapshot.vintages}
            )
            for snapshot in self.snapshots
        )
        return {
            "attribution": ALFRED_ATTRIBUTION,
            "coverage": {
                "initial_release_records": initial,
                "missing_release_dates": missing_release,
                "missing_value_records": missing_values,
                "revision_records": revisions,
                "series": {
                    item.spec.series_id: len(item.vintages) for item in self.snapshots
                },
                "series_count": len(self.snapshots),
            },
            "data_quality": {
                "precise_intraday_release_times": 0,
                "release_time_precision": "DATE_ONLY_OR_NOT_AVAILABLE",
                "state": "DEGRADED" if missing_release or missing_values else "VALID",
            },
            "generated_at_utc_ns": self.generated_at_utc_ns,
            "live_trading_capable": False,
            "manifest_ids": list(self.manifest_ids),
            "network_access_performed": self.network_access_performed,
            "observation_end": self.observation_end.isoformat(),
            "observation_start": self.observation_start.isoformat(),
            "requests": {
                "request_count": self.request_count,
                "retry_count": self.retry_count,
            },
            "run_id": self.run_id,
            "schema_version": ALFRED_SCHEMA_VERSION,
            "series_universe_sha256": self.series_universe_sha256,
            "snapshot_sha256s": [item.snapshot_sha256 for item in self.snapshots],
            "storage": {
                "limit_bytes_decimal": self.storage_limit_bytes,
                "usage_after_bytes_decimal": self.usage_after_bytes,
                "usage_before_bytes_decimal": self.usage_before_bytes,
            },
            "vintage_end": self.vintage_end.isoformat(),
            "vintage_start": self.vintage_start.isoformat(),
        }

    @property
    def report_sha256(self) -> str:
        """Return the report content hash."""
        return _sha256(_canonical_bytes(self.to_payload()))

    def to_dict(self) -> dict[str, object]:
        """Return the report plus its content hash."""
        return {**self.to_payload(), "report_sha256": self.report_sha256}

    def encode(self) -> bytes:
        """Serialize the report canonically."""
        return _canonical_bytes(self.to_dict()) + b"\n"


class AlfredAdapter:
    """Orchestrate reviewed series fetch, normalization, and immutable publication."""

    def __init__(
        self,
        config: AlfredConfig,
        authorization: AlfredAuthorization,
        *,
        repository: AlfredRepository | None = None,
        wall_clock_ns: Callable[[], int] = time.time_ns,
    ) -> None:
        """Bind configuration and authorization to an optional sink."""
        self.config = config
        self.authorization = authorization
        self.repository = repository
        self._wall_clock_ns = wall_clock_ns

    def run(self, client: AlfredClient) -> AlfredRunReport:
        """Run the complete bounded plan; malformed series abort the run."""
        now = self._wall_clock_ns()
        self.authorization.require(self.config, now)
        universe_hash = series_universe_sha256(self.config.series_ids)
        if self.repository is not None and (
            self.repository.series_universe_hash != universe_hash
            or self.repository.authorization.approval_sha256
            != self.authorization.approval_sha256
        ):
            raise AlfredError(
                AlfredErrorCode.UNAUTHORIZED,
                "ALFRED repository authorization binding mismatch",
            )
        usage_before = self.repository.usage_bytes() if self.repository else 0
        snapshots: list[AlfredSeriesSnapshot] = []
        manifests: list[str] = []

        @contextmanager
        def publication_scope() -> Iterator[None]:
            if self.repository is None:
                yield
            else:
                with self.repository.publication_batch():
                    yield

        with publication_scope():
            for plan in build_query_plans(self.config):
                fetched = client.fetch_series(plan)
                processing = self._wall_clock_ns()
                snapshot = build_series_snapshot(
                    plan, fetched, processing_time_utc_ns=processing
                )
                snapshots.append(snapshot)
                if self.repository is None:
                    continue
                for page in fetched.pages:
                    kind = {
                        ALFRED_METADATA_ENDPOINT: AlfredObjectKind.SERIES_METADATA,
                        ALFRED_RELEASE_ENDPOINT: AlfredObjectKind.SERIES_RELEASE,
                        ALFRED_RELEASE_DATES_ENDPOINT: AlfredObjectKind.RELEASE_DATES,
                        ALFRED_OBSERVATIONS_ENDPOINT: AlfredObjectKind.OBSERVATIONS,
                    }[page.endpoint]
                    manifest = self.repository.publish(
                        series_id=plan.series.series_id,
                        plan_id=plan.plan_id,
                        object_kind=kind,
                        payload=page.body,
                        record_count=1,
                        event_time_min_ns=_date_start_ns(plan.observation_start),
                        event_time_max_ns=_date_start_ns(plan.observation_end),
                        revision_time_min_ns=_date_start_ns(plan.vintage_start),
                        revision_time_max_ns=_date_start_ns(plan.vintage_end),
                        receipt_time_utc_ns=page.received_at_utc_ns,
                        processing_time_utc_ns=processing,
                    )
                    manifests.append(manifest.manifest_id)
                encoded = _canonical_bytes(snapshot.to_dict()) + b"\n"
                canonical = self.repository.publish(
                    series_id=plan.series.series_id,
                    plan_id=plan.plan_id,
                    object_kind=AlfredObjectKind.CANONICAL_SNAPSHOT,
                    payload=encoded,
                    record_count=len(snapshot.vintages),
                    event_time_min_ns=min(
                        item.observation_time_utc_ns for item in snapshot.vintages
                    ),
                    event_time_max_ns=max(
                        item.observation_time_utc_ns for item in snapshot.vintages
                    ),
                    revision_time_min_ns=min(
                        item.known_at_utc_ns for item in snapshot.vintages
                    ),
                    revision_time_max_ns=max(
                        item.known_at_utc_ns for item in snapshot.vintages
                    ),
                    receipt_time_utc_ns=max(
                        item.local_receipt_time_utc_ns for item in snapshot.vintages
                    ),
                    processing_time_utc_ns=processing,
                )
                manifests.append(canonical.manifest_id)
        usage_after = self.repository.usage_bytes() if self.repository else 0
        run_body = {
            "plans": [item.plan_id for item in build_query_plans(self.config)],
            "snapshots": [item.snapshot_sha256 for item in snapshots],
            "series_universe_sha256": universe_hash,
        }
        return AlfredRunReport(
            run_id="alfred-run-" + _sha256(_canonical_bytes(run_body)),
            series_universe_sha256=universe_hash,
            vintage_start=self.config.vintage_start,
            vintage_end=self.config.vintage_end,
            observation_start=self.config.observation_start,
            observation_end=self.config.observation_end,
            generated_at_utc_ns=self._wall_clock_ns(),
            network_access_performed=client.transport.network_access,
            request_count=client.request_count,
            retry_count=client.retry_count,
            snapshots=tuple(snapshots),
            manifest_ids=tuple(manifests),
            usage_before_bytes=usage_before,
            usage_after_bytes=usage_after,
            storage_limit_bytes=self.config.storage_limit_bytes,
        )


def write_reports(
    report: AlfredRunReport, repository: AlfredRepository
) -> tuple[Path, Path]:
    """Publish machine and human reports through the same storage boundary."""
    machine = report.encode()
    human = (
        "# Bounded ALFRED macro-vintage ingestion report\n\n"
        f"- Run: `{report.run_id}`\n"
        f"- Series: {len(report.snapshots)}\n"
        f"- Requests: {report.request_count}\n"
        f"- Retries: {report.retry_count}\n"
        f"- Network access: `{str(report.network_access_performed).lower()}`\n"
        "- Precise intraday release times: unavailable\n"
        "- Live trading capable: `false`\n"
    ).encode()
    now = report.generated_at_utc_ns
    machine_manifest = repository.publish(
        series_id="_ALL",
        plan_id=report.run_id,
        object_kind=AlfredObjectKind.RUN_REPORT_JSON,
        payload=machine,
        record_count=1,
        event_time_min_ns=_date_start_ns(report.observation_start),
        event_time_max_ns=_date_start_ns(report.observation_end),
        revision_time_min_ns=_date_start_ns(report.vintage_start),
        revision_time_max_ns=_date_start_ns(report.vintage_end),
        receipt_time_utc_ns=now,
        processing_time_utc_ns=now,
    )
    human_manifest = repository.publish(
        series_id="_ALL",
        plan_id=report.run_id,
        object_kind=AlfredObjectKind.RUN_REPORT_MARKDOWN,
        payload=human,
        record_count=1,
        event_time_min_ns=_date_start_ns(report.observation_start),
        event_time_max_ns=_date_start_ns(report.observation_end),
        revision_time_min_ns=_date_start_ns(report.vintage_start),
        revision_time_max_ns=_date_start_ns(report.vintage_end),
        receipt_time_utc_ns=now,
        processing_time_utc_ns=now,
    )
    return (
        repository.repository.root / machine_manifest.storage_path,
        repository.repository.root / human_manifest.storage_path,
    )


def _plan_document(config: AlfredConfig) -> dict[str, object]:
    plans = build_query_plans(config)
    body: dict[str, object] = {
        "approved_series": [item.series.series_id for item in plans],
        "blocked_series": [item.series_id for item in RESTRICTED_SERIES],
        "live_trading_capable": False,
        "network_access_performed": False,
        "observation_end": config.observation_end.isoformat(),
        "observation_start": config.observation_start.isoformat(),
        "plan_ids": [item.plan_id for item in plans],
        "series_universe_sha256": series_universe_sha256(config.series_ids),
        "storage_limit_bytes_decimal": config.storage_limit_bytes,
        "vintage_end": config.vintage_end.isoformat(),
        "vintage_start": config.vintage_start.isoformat(),
    }
    return {**body, "plan_sha256": _sha256(_canonical_bytes(body))}


def _config_from_args(arguments: argparse.Namespace) -> AlfredConfig:
    series = (
        tuple(arguments.series)
        if arguments.series
        else tuple(item.series_id for item in APPROVED_SERIES)
    )
    return AlfredConfig(
        vintage_start=arguments.vintage_start,
        vintage_end=arguments.vintage_end,
        observation_start=arguments.observation_start,
        observation_end=arguments.observation_end,
        series_ids=series,
    )


def cli_main(argv: Sequence[str] | None = None) -> int:
    """Plan without network by default; execute only with every external gate."""
    parser = argparse.ArgumentParser(prog="aegis-alfred")
    parser.add_argument("command", choices=("plan", "ingest"))
    parser.add_argument("--vintage-start", type=date.fromisoformat, required=True)
    parser.add_argument("--vintage-end", type=date.fromisoformat, required=True)
    parser.add_argument("--observation-start", type=date.fromisoformat, required=True)
    parser.add_argument("--observation-end", type=date.fromisoformat, required=True)
    parser.add_argument("--series", action="append")
    parser.add_argument("--approval", type=Path)
    parser.add_argument("--data-root", type=Path, default=DEFAULT_DATA_ROOT)
    parser.add_argument("--quota-limit-bytes", type=int)
    parser.add_argument("--quota-used-bytes", type=int)
    parser.add_argument("--quota-source")
    parser.add_argument("--quota-observed-at-utc")
    parser.add_argument("--quota-authoritative", action="store_true")
    parser.add_argument("--execute", action="store_true")
    arguments = parser.parse_args(argv)
    config = _config_from_args(arguments)
    if arguments.command == "plan" or not arguments.execute:
        sys.stdout.write(json.dumps(_plan_document(config), sort_keys=True) + "\n")
        return 0
    if arguments.approval is None:
        raise AlfredError(
            AlfredErrorCode.UNAUTHORIZED, "execution requires an approval file"
        )
    authorization = AlfredAuthorization.load(arguments.approval)
    quota = QuotaEvidence(
        limit_bytes=arguments.quota_limit_bytes,
        used_bytes=arguments.quota_used_bytes,
        source=arguments.quota_source,
        observed_at_utc=arguments.quota_observed_at_utc,
        authoritative=arguments.quota_authoritative,
    )
    if not quota.known:
        raise AlfredError(
            AlfredErrorCode.STORAGE_LIMIT,
            "execution requires authoritative quota evidence",
        )
    key = os.environ.get("AEGIS_FRED_API_KEY", "")
    if re.fullmatch(r"[a-z0-9]{32}", key) is None:
        raise AlfredError(
            AlfredErrorCode.CREDENTIAL_UNAVAILABLE,
            "AEGIS_FRED_API_KEY is absent or malformed",
        )
    repository = DataRepository(arguments.data_root)
    repository.initialize()
    universe_hash = series_universe_sha256(config.series_ids)
    sink = AlfredRepository(
        repository, quota, authorization, series_universe_hash=universe_hash
    )
    adapter = AlfredAdapter(config, authorization, repository=sink)
    client = AlfredClient(config, SecretValue(key), UrllibAlfredTransport())
    report = adapter.run(client)
    machine, human = write_reports(report, sink)
    sys.stdout.write(
        json.dumps(
            {
                "human_report": str(human),
                "machine_report": str(machine),
                "report_sha256": report.report_sha256,
            },
            sort_keys=True,
        )
        + "\n"
    )
    return 0


if __name__ == "__main__":  # pragma: no cover - console script boundary
    raise SystemExit(cli_main())
