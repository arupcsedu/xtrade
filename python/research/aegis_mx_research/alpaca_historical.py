"""Bounded Alpaca Basic/IEX minute-bar pilot and backfill tooling.

This module is deliberately offline-first.  Network access is possible only
after an exact source policy and its content-addressed approval record pass,
the canonical ticker universe still matches, credentials are held in an
owner-only file, storage admission succeeds, and ``--execute`` is explicit.
It has no trading or order-entry capability.
"""

from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import os
import random
import re
import signal
import ssl
import stat
import sys
import time
from collections import defaultdict
from dataclasses import dataclass, field
from datetime import UTC, date, datetime, timedelta
from datetime import time as datetime_time
from decimal import Decimal, InvalidOperation
from enum import StrEnum
from pathlib import Path
from typing import TYPE_CHECKING, Final, Never, Protocol, Self, cast
from urllib.parse import quote, urlencode
from zoneinfo import ZoneInfo

from aegis_mx_research.data_repository import (
    BINARY_TIB,
    DEFAULT_DATA_ROOT,
    AdmissionLease,
    DataRepository,
    ManifestLineage,
    ManifestTimeRange,
    PartitionManifest,
    QuotaEvidence,
    SourceManifest,
    StorageError,
    StorageRequest,
    deterministic_dataset_id,
)
from aegis_mx_research.forecast_contracts import (
    UniverseSnapshot,
    UnresolvedInstrumentResolver,
    load_ticker_universe,
)
from tools.source_policy_check import SourcePolicyError, validate_policy

if TYPE_CHECKING:
    from collections.abc import Callable, Mapping, Sequence
    from types import FrameType

SOURCE_ID: Final = "alpaca_iex_historical_bars"
DATASET_ID: Final = "alpaca-iex-minute-bars"
SCHEMA_VERSION: Final = "1.0.0"
CANONICAL_SCHEMA_VERSION: Final = "1.0.0"
SOURCE_VERSION: Final = "alpaca-market-data-api-v2-2026-09-11"
DATA_HOST: Final = "data.alpaca.markets"
PAPER_HOST: Final = "paper-api.alpaca.markets"
TIMEFRAME: Final = "1Min"
FEED: Final = "iex"
ADJUSTMENT: Final = "raw"
SESSION_ZONE: Final = ZoneInfo("America/New_York")
PRICE_SCALE: Final = 1_000_000_000
MAX_RESPONSE_BYTES: Final = 16_777_216
MAX_APPROVAL_BYTES: Final = 262_144
MAX_CHECKPOINT_BYTES: Final = 16_777_216
MAX_REPORT_BYTES: Final = 67_108_864
MAX_PAGES_PER_TASK: Final = 64
MAX_RETRY_ATTEMPTS: Final = 4
MAX_RETRY_AFTER_SECONDS: Final = 60
REQUESTS_PER_MINUTE: Final = 180
BAR_LIMIT: Final = 10_000
SYMBOL_BATCH_SIZE: Final = 8
SESSION_BATCH_SIZE: Final = 5
DEFAULT_SAMPLE_SEED: Final = 20260911
DEFAULT_SECRETS_PATH: Final = Path("/scratch/djy8hg/ALPACA/.keys")
DEFAULT_APPROVAL_PATH: Final = (
    DEFAULT_DATA_ROOT / "manifests/approvals/alpaca-iex-academic-approval-v1.json"
)
DEFAULT_POLICY_PATH: Final = (
    DEFAULT_DATA_ROOT / "manifests/approvals/alpaca-iex-academic-policy-v1.json"
)
DEFAULT_PILOT_REPORT: Final = (
    DEFAULT_DATA_ROOT / "reports/alpaca-iex-minute/pilot/report.json"
)
DEFAULT_PILOT_ACCEPTANCE: Final = (
    DEFAULT_DATA_ROOT / "reports/alpaca-iex-minute/pilot/acceptance.json"
)
DEFAULT_BACKFILL_REPORT: Final = (
    DEFAULT_DATA_ROOT / "reports/alpaca-iex-minute/backfill/report.json"
)
_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_RFC3339 = re.compile(
    r"^(?P<date>[0-9]{4}-[0-9]{2}-[0-9]{2})T"
    r"(?P<hour>[0-9]{2}):(?P<minute>[0-9]{2}):(?P<second>[0-9]{2})"
    r"(?:\.(?P<fraction>[0-9]{1,9}))?Z$"
)


class AlpacaDataErrorCode(StrEnum):
    """Stable fail-closed reason codes for the source-specific path."""

    AUTHORIZATION_INVALID = "AUTHORIZATION_INVALID"
    CREDENTIAL_INVALID = "CREDENTIAL_INVALID"
    NETWORK_FAILURE = "NETWORK_FAILURE"
    PROVIDER_REJECTED = "PROVIDER_REJECTED"
    RATE_LIMITED = "RATE_LIMITED"
    RESPONSE_TOO_LARGE = "RESPONSE_TOO_LARGE"
    RESPONSE_MALFORMED = "RESPONSE_MALFORMED"
    CALENDAR_INVALID = "CALENDAR_INVALID"
    PLAN_INVALID = "PLAN_INVALID"
    PILOT_NOT_ACCEPTED = "PILOT_NOT_ACCEPTED"
    STORAGE_REJECTED = "STORAGE_REJECTED"
    CONCURRENT_EPOCH = "CONCURRENT_EPOCH"
    CHECKPOINT_CORRUPT = "CHECKPOINT_CORRUPT"
    DATA_INVALID = "DATA_INVALID"
    INTEGRITY_FAILURE = "INTEGRITY_FAILURE"
    INTERRUPTED = "INTERRUPTED"


class AlpacaDataError(RuntimeError):
    """One bounded, secret-free failure."""

    def __init__(self, code: AlpacaDataErrorCode, message: str) -> None:
        """Retain a stable code and a bounded non-secret explanation."""
        self.code = code
        super().__init__(str(message)[:512])


class RunMode(StrEnum):
    """Supported source acquisition scopes."""

    PILOT = "PILOT"
    BACKFILL = "BACKFILL"


def _canonical_bytes(value: object) -> bytes:
    return json.dumps(
        value, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode("ascii")


def _sha256(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def _now_utc() -> datetime:
    return datetime.now(UTC)


def _utc_ns(value: datetime) -> int:
    if value.tzinfo is None or value.utcoffset() != timedelta(0):
        raise AlpacaDataError(
            AlpacaDataErrorCode.DATA_INVALID, "timestamp is not explicit UTC"
        )
    delta = value - datetime(1970, 1, 1, tzinfo=UTC)
    return (
        delta.days * 86_400_000_000_000
        + delta.seconds * 1_000_000_000
        + delta.microseconds * 1_000
    )


def _parse_rfc3339_ns(value: object) -> tuple[datetime, int]:
    if not isinstance(value, str):
        raise AlpacaDataError(
            AlpacaDataErrorCode.DATA_INVALID, "provider timestamp is not text"
        )
    matched = _RFC3339.fullmatch(value)
    if matched is None:
        raise AlpacaDataError(
            AlpacaDataErrorCode.DATA_INVALID,
            "provider timestamp is not bounded RFC3339 UTC",
        )
    fraction = (matched.group("fraction") or "").ljust(9, "0")
    try:
        year, month, day = (int(part) for part in matched.group("date").split("-"))
        parsed = datetime(
            year=year,
            month=month,
            day=day,
            hour=int(matched.group("hour")),
            minute=int(matched.group("minute")),
            second=int(matched.group("second")),
            microsecond=int(fraction[:6] or "0"),
            tzinfo=UTC,
        )
    except ValueError as error:
        raise AlpacaDataError(
            AlpacaDataErrorCode.DATA_INVALID, "provider timestamp is invalid"
        ) from error
    epoch_ns = _utc_ns(parsed) - parsed.microsecond * 1_000 + int(fraction or "0")
    return parsed, epoch_ns


def _parse_utc_second(value: object, field_name: str) -> datetime:
    if not isinstance(value, str):
        raise AlpacaDataError(
            AlpacaDataErrorCode.AUTHORIZATION_INVALID,
            f"{field_name} must be an explicit UTC timestamp",
        )
    try:
        parsed = datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=UTC)
    except ValueError as error:
        raise AlpacaDataError(
            AlpacaDataErrorCode.AUTHORIZATION_INVALID,
            f"{field_name} must be an explicit UTC timestamp",
        ) from error
    return parsed


def _parse_clock_datetime(value: object) -> datetime:
    if not isinstance(value, str) or len(value) > 64:
        raise AlpacaDataError(
            AlpacaDataErrorCode.CALENDAR_INVALID,
            "clock timestamp is not bounded text",
        )
    try:
        parsed = datetime.fromisoformat(value)
    except ValueError as error:
        raise AlpacaDataError(
            AlpacaDataErrorCode.CALENDAR_INVALID, "clock timestamp is malformed"
        ) from error
    if parsed.tzinfo is None:
        raise AlpacaDataError(
            AlpacaDataErrorCode.CALENDAR_INVALID,
            "clock timestamp lacks an explicit offset",
        )
    return parsed.astimezone(UTC)


def _read_secure(path: Path, maximum_bytes: int, description: str) -> bytes:
    try:
        status = os.lstat(path)
        if stat.S_ISLNK(status.st_mode) or not stat.S_ISREG(status.st_mode):
            raise AlpacaDataError(
                AlpacaDataErrorCode.AUTHORIZATION_INVALID,
                f"{description} must be a regular non-symlink file",
            )
        if status.st_size <= 0 or status.st_size > maximum_bytes:
            raise AlpacaDataError(
                AlpacaDataErrorCode.AUTHORIZATION_INVALID,
                f"{description} is empty or exceeds its size bound",
            )
        descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
        try:
            opened = os.fstat(descriptor)
            if (opened.st_dev, opened.st_ino) != (status.st_dev, status.st_ino):
                raise AlpacaDataError(
                    AlpacaDataErrorCode.AUTHORIZATION_INVALID,
                    f"{description} changed while it was opened",
                )
            payload = b""
            while len(payload) <= maximum_bytes:
                chunk = os.read(
                    descriptor, min(65_536, maximum_bytes + 1 - len(payload))
                )
                if not chunk:
                    break
                payload += chunk
        finally:
            os.close(descriptor)
    except AlpacaDataError:
        raise
    except OSError as error:
        raise AlpacaDataError(
            AlpacaDataErrorCode.AUTHORIZATION_INVALID,
            f"cannot read {description}",
        ) from error
    if not payload or len(payload) > maximum_bytes:
        raise AlpacaDataError(
            AlpacaDataErrorCode.AUTHORIZATION_INVALID,
            f"{description} is empty or exceeds its size bound",
        )
    return payload


def _atomic_write(path: Path, payload: bytes, *, mode: int = 0o600) -> None:
    path.parent.mkdir(parents=True, mode=0o700, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.partial")
    descriptor = os.open(
        temporary,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
        mode,
    )
    try:
        offset = 0
        while offset < len(payload):
            offset += os.write(descriptor, payload[offset:])
        os.fsync(descriptor)
    except Exception:
        os.close(descriptor)
        temporary.unlink(missing_ok=True)
        raise
    else:
        os.close(descriptor)
    temporary.replace(path)
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


def _write_json(path: Path, value: Mapping[str, object]) -> str:
    body = dict(value)
    body.pop("document_sha256", None)
    digest = _sha256(_canonical_bytes(body))
    encoded = _canonical_bytes({**body, "document_sha256": digest}) + b"\n"
    _atomic_write(path, encoded)
    return digest


def _load_hashed_json(path: Path, description: str) -> tuple[dict[str, object], str]:
    payload = _read_secure(path, MAX_REPORT_BYTES, description)
    try:
        value = json.loads(payload)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AlpacaDataError(
            AlpacaDataErrorCode.INTEGRITY_FAILURE,
            f"{description} is malformed JSON",
        ) from error
    if not isinstance(value, dict) or not all(isinstance(key, str) for key in value):
        raise AlpacaDataError(
            AlpacaDataErrorCode.INTEGRITY_FAILURE,
            f"{description} is not a JSON object",
        )
    record = cast("dict[str, object]", value)
    supplied = record.pop("document_sha256", None)
    actual = _sha256(_canonical_bytes(record))
    record["document_sha256"] = supplied
    if supplied != actual:
        raise AlpacaDataError(
            AlpacaDataErrorCode.INTEGRITY_FAILURE,
            f"{description} SHA-256 does not match",
        )
    return record, _sha256(payload)


class AlpacaCredentials:
    """Non-printing Alpaca credential pair."""

    __slots__ = ("_key_id", "_secret_key")

    def __init__(self, key_id: str, secret_key: str) -> None:
        """Retain validated credentials without exposing their values."""
        if not key_id or not secret_key or len(key_id) > 256 or len(secret_key) > 256:
            raise AlpacaDataError(
                AlpacaDataErrorCode.CREDENTIAL_INVALID,
                "Alpaca credentials are absent or malformed",
            )
        self._key_id = key_id
        self._secret_key = secret_key

    @classmethod
    def load(cls, path: Path = DEFAULT_SECRETS_PATH) -> Self:
        """Load the approved owner-only environment file without logging values."""
        try:
            status = os.lstat(path)
        except OSError as error:
            raise AlpacaDataError(
                AlpacaDataErrorCode.CREDENTIAL_INVALID,
                "Alpaca credential file is unavailable",
            ) from error
        if (
            stat.S_ISLNK(status.st_mode)
            or not stat.S_ISREG(status.st_mode)
            or status.st_uid != os.getuid()
            or status.st_mode & 0o077
            or status.st_size > 16_384
        ):
            raise AlpacaDataError(
                AlpacaDataErrorCode.CREDENTIAL_INVALID,
                "Alpaca credential file ownership, mode, type, or size is unsafe",
            )
        payload = _read_secure(path, 16_384, "Alpaca credential file")
        try:
            text = payload.decode("utf-8")
        except UnicodeDecodeError as error:
            raise AlpacaDataError(
                AlpacaDataErrorCode.CREDENTIAL_INVALID,
                "Alpaca credential file is not UTF-8",
            ) from error
        values: dict[str, str] = {}
        for line in text.splitlines():
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            raw_key, separator, raw_value = stripped.partition("=")
            key = raw_key.strip()
            if not separator or not re.fullmatch(r"[A-Z][A-Z0-9_]{2,127}", key):
                raise AlpacaDataError(
                    AlpacaDataErrorCode.CREDENTIAL_INVALID,
                    "Alpaca credential file contains a malformed assignment",
                )
            value = raw_value.strip().strip('"').strip("'")
            if key in values:
                raise AlpacaDataError(
                    AlpacaDataErrorCode.CREDENTIAL_INVALID,
                    "Alpaca credential file contains a duplicate assignment",
                )
            values[key] = value
        endpoint = values.get("AEGIS_ALPACA_PAPER_ENDPOINT")
        if endpoint not in {f"https://{PAPER_HOST}", f"https://{PAPER_HOST}/v2"}:
            raise AlpacaDataError(
                AlpacaDataErrorCode.CREDENTIAL_INVALID,
                "configured Alpaca endpoint is not the fixed PAPER origin",
            )
        return cls(
            values.get("AEGIS_ALPACA_PAPER_KEY_ID", ""),
            values.get("AEGIS_ALPACA_PAPER_SECRET_KEY", ""),
        )

    def headers(self) -> dict[str, str]:
        """Reveal only the two provider-required authentication headers."""
        return {
            "APCA-API-KEY-ID": self._key_id,
            "APCA-API-SECRET-KEY": self._secret_key,
        }

    def __repr__(self) -> str:
        """Render an always-redacted diagnostic representation."""
        return "AlpacaCredentials([REDACTED])"


@dataclass(frozen=True, slots=True)
class HttpResponse:
    """One bounded HTTPS response."""

    status: int
    body: bytes
    received_at_utc: datetime
    retry_after_seconds: int | None = None


class AlpacaTransport(Protocol):
    """Injectable exact-host HTTPS boundary."""

    def get(
        self,
        host: str,
        path: str,
        parameters: Sequence[tuple[str, str]],
        credentials: AlpacaCredentials,
        *,
        timeout_seconds: float,
        maximum_response_bytes: int,
    ) -> HttpResponse:
        """Return one complete bounded response without following redirects."""
        ...


class FixedHostHttpsTransport:
    """TLS-validating transport restricted to Alpaca data and PAPER hosts."""

    def __init__(self, context: ssl.SSLContext | None = None) -> None:
        """Create a fixed-host transport with a validating TLS context."""
        self._context = context or ssl.create_default_context()

    def get(
        self,
        host: str,
        path: str,
        parameters: Sequence[tuple[str, str]],
        credentials: AlpacaCredentials,
        *,
        timeout_seconds: float,
        maximum_response_bytes: int,
    ) -> HttpResponse:
        """Issue one bounded GET to an allowlisted Alpaca host."""
        if host not in {DATA_HOST, PAPER_HOST} or not path.startswith("/v2/"):
            raise AlpacaDataError(
                AlpacaDataErrorCode.NETWORK_FAILURE,
                "request target is outside the fixed Alpaca host allowlist",
            )
        target = path
        if parameters:
            target = f"{path}?{urlencode(parameters)}"
        connection = http.client.HTTPSConnection(
            host, timeout=timeout_seconds, context=self._context
        )
        try:
            connection.request(
                "GET",
                target,
                headers={
                    **credentials.headers(),
                    "Accept": "application/json",
                    "User-Agent": "Aegis-MX-Academic-POC/1.0",
                },
            )
            response = connection.getresponse()
            body = response.read(maximum_response_bytes + 1)
            received = _now_utc()
            retry_after: int | None = None
            raw_retry = response.getheader("Retry-After")
            if raw_retry is not None and raw_retry.isdigit():
                retry_after = min(int(raw_retry), MAX_RETRY_AFTER_SECONDS)
        except (OSError, TimeoutError, http.client.HTTPException) as error:
            raise AlpacaDataError(
                AlpacaDataErrorCode.NETWORK_FAILURE,
                f"Alpaca HTTPS request failed ({type(error).__name__})",
            ) from error
        finally:
            connection.close()
        if len(body) > maximum_response_bytes:
            raise AlpacaDataError(
                AlpacaDataErrorCode.RESPONSE_TOO_LARGE,
                "Alpaca response exceeds the configured byte limit",
            )
        return HttpResponse(response.status, body, received, retry_after)


@dataclass(frozen=True, slots=True)
class ApiResult:
    """Parsed provider response with exact raw bytes and timing."""

    value: object
    body: bytes
    received_at_utc: datetime
    attempts: int
    status: int = 200


class AlpacaApiClient:
    """Bounded GET-only client for historical data and PAPER reference state."""

    def __init__(
        self,
        credentials: AlpacaCredentials,
        *,
        transport: AlpacaTransport | None = None,
        sleep: Callable[[float], None] = time.sleep,
        monotonic: Callable[[], float] = time.monotonic,
    ) -> None:
        """Create a rate-limited client around an injectable transport."""
        self._credentials = credentials
        self._transport = transport or FixedHostHttpsTransport()
        self._sleep = sleep
        self._monotonic = monotonic
        self._minimum_interval = 60.0 / REQUESTS_PER_MINUTE
        self._last_request = 0.0
        self.request_count = 0
        self.retry_count = 0

    def _wait_rate_limit(self) -> None:
        now = float(self._monotonic())
        delay = self._minimum_interval - (now - self._last_request)
        if self._last_request and delay > 0:
            self._sleep(delay)
        self._last_request = float(self._monotonic())

    def _get(
        self,
        host: str,
        path: str,
        parameters: Sequence[tuple[str, str]] = (),
        *,
        accepted_statuses: frozenset[int] = frozenset({200}),
    ) -> ApiResult:
        for attempt in range(1, MAX_RETRY_ATTEMPTS + 1):
            self._wait_rate_limit()
            response = self._transport.get(
                host,
                path,
                parameters,
                self._credentials,
                timeout_seconds=30.0,
                maximum_response_bytes=MAX_RESPONSE_BYTES,
            )
            self.request_count += 1
            if response.status in accepted_statuses:
                try:
                    value = json.loads(response.body)
                except (UnicodeDecodeError, json.JSONDecodeError) as error:
                    raise AlpacaDataError(
                        AlpacaDataErrorCode.RESPONSE_MALFORMED,
                        "Alpaca returned malformed JSON",
                    ) from error
                return ApiResult(
                    value,
                    response.body,
                    response.received_at_utc,
                    attempt,
                    response.status,
                )
            retryable = response.status == 429 or 500 <= response.status <= 599
            if not retryable or attempt == MAX_RETRY_ATTEMPTS:
                code = (
                    AlpacaDataErrorCode.RATE_LIMITED
                    if response.status == 429
                    else AlpacaDataErrorCode.PROVIDER_REJECTED
                )
                raise AlpacaDataError(
                    code,
                    f"Alpaca rejected GET {path} with HTTP {response.status}",
                )
            self.retry_count += 1
            delay = response.retry_after_seconds or min(2 ** (attempt - 1), 8)
            self._sleep(float(delay))
        raise AlpacaDataError(  # pragma: no cover - bounded range always returns/raises
            AlpacaDataErrorCode.NETWORK_FAILURE, "Alpaca retry budget exhausted"
        )

    def clock(self) -> ApiResult:
        """Read the provider account clock without mutating account state."""
        return self._get(PAPER_HOST, "/v2/clock")

    def calendar(self, start: date, end: date) -> ApiResult:
        """Read the provider trading calendar for an inclusive date range."""
        return self._get(
            PAPER_HOST,
            "/v2/calendar",
            (("start", start.isoformat()), ("end", end.isoformat())),
        )

    def asset(self, symbol: str) -> ApiResult:
        """Read one provider asset record for universe resolution."""
        return self._get(
            PAPER_HOST,
            f"/v2/assets/{quote(symbol, safe='')}",
            accepted_statuses=frozenset({200, 404}),
        )

    def bars(
        self,
        symbols: Sequence[str],
        start: datetime,
        end: datetime,
        page_token: str | None,
    ) -> ApiResult:
        """Read one bounded page of raw one-minute IEX bars."""
        parameters = [
            ("symbols", ",".join(symbols)),
            ("timeframe", TIMEFRAME),
            ("start", start.isoformat().replace("+00:00", "Z")),
            ("end", end.isoformat().replace("+00:00", "Z")),
            ("limit", str(BAR_LIMIT)),
            ("adjustment", ADJUSTMENT),
            ("feed", FEED),
            ("sort", "asc"),
        ]
        if page_token is not None:
            if not page_token or len(page_token) > 4096:
                raise AlpacaDataError(
                    AlpacaDataErrorCode.RESPONSE_MALFORMED,
                    "Alpaca page token is empty or oversized",
                )
            parameters.append(("page_token", page_token))
        return self._get(DATA_HOST, "/v2/stocks/bars", tuple(parameters))


@dataclass(frozen=True, slots=True)
class SourceAuthorization:
    """Exact content-bound authorization for this limited source use."""

    policy_sha256: str
    approval_sha256: str
    expires_at_utc: datetime
    universe_source_sha256: str
    data_root: Path
    use_classification: str

    def validate(self, snapshot: UniverseSnapshot, root: Path) -> None:
        """Require unexpired authorization for the exact universe and root."""
        if self.expires_at_utc <= _now_utc():
            raise AlpacaDataError(
                AlpacaDataErrorCode.AUTHORIZATION_INVALID,
                "Alpaca source authorization has expired",
            )
        if self.universe_source_sha256 != snapshot.source_file_sha256.hex():
            raise AlpacaDataError(
                AlpacaDataErrorCode.AUTHORIZATION_INVALID,
                "source authorization is bound to another ticker.txt snapshot",
            )
        if self.data_root != root:
            raise AlpacaDataError(
                AlpacaDataErrorCode.AUTHORIZATION_INVALID,
                "source authorization is bound to another data root",
            )


def prepare_academic_authorization(
    *,
    operator_id: str,
    data_root: Path,
    approval_path: Path,
    policy_path: Path,
    expires_at_utc: datetime,
    example_policy_path: Path,
) -> tuple[str, str]:
    """Create the user's narrow non-commercial approval and derived policy."""
    if not re.fullmatch(r"[A-Za-z0-9_.-]{1,128}", operator_id):
        raise AlpacaDataError(
            AlpacaDataErrorCode.AUTHORIZATION_INVALID, "operator_id is malformed"
        )
    if expires_at_utc.tzinfo is None or expires_at_utc.utcoffset() != timedelta(0):
        raise AlpacaDataError(
            AlpacaDataErrorCode.AUTHORIZATION_INVALID, "approval expiry is not UTC"
        )
    snapshot = load_ticker_universe(UnresolvedInstrumentResolver())
    approval: dict[str, object] = {
        "approval_id": "alpaca-iex-academic-single-user-v1",
        "authorization_basis": (
            "operator-directed application of Alpaca personal/non-commercial "
            "terms to an owner-only internal academic POC"
        ),
        "data_root": str(data_root),
        "dataset": "Market Data API v2 Historical Stock Bars",
        "decision": "APPROVED",
        "distribution": "INTERNAL_SINGLE_USER_ONLY",
        "effective_at_utc": _now_utc().strftime("%Y-%m-%dT%H:%M:%SZ"),
        "evidence": {
            "customer_agreement_sha256": (
                "d086d69f063bba1e9693fffbb3185c44"  # pragma: allowlist secret
                "69b24b47bda79f587053bd771580fcf6"  # pragma: allowlist secret
            ),
            "customer_agreement_url": (
                "https://files.alpaca.markets/disclosures/library/"
                "AcctAppMarginAndCustAgmt.pdf"
            ),
            "market_data_documentation_url": (
                "https://docs.alpaca.markets/us/docs/about-market-data-api"
            ),
            "terms_sha256": (
                "2dc774d4aeeafbe4c7f0565e7842d932"  # pragma: allowlist secret
                "bc8bc10488af805fce43b8734e7b9859"  # pragma: allowlist secret
            ),
            "terms_url": (
                "https://files.alpaca.markets/disclosures/library/"
                "TermsAndConditions.pdf"
            ),
        },
        "expires_at_utc": expires_at_utc.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "feed": FEED,
        "live_trading_capable": False,
        "model_training_authorized": True,
        "operator_id": operator_id,
        "plan": "BASIC",
        "purpose": "INTERNAL_ACADEMIC_NONCOMMERCIAL_RESEARCH",
        "redistribution_authorized": False,
        "restrictions": [
            "no external distribution or publication of raw market data",
            "no commercial use or resale",
            "owner-only data-root permissions",
            "one-minute IEX OHLCV regular-session bars only",
            "maximum two-year window ending at the latest complete session",
            "stop new downloads on account, terms, or entitlement change",
        ],
        "schema_version": SCHEMA_VERSION,
        "source_id": SOURCE_ID,
        "ticker_source_path": snapshot.source_path,
        "ticker_source_sha256": snapshot.source_file_sha256.hex(),
        "timeframe": TIMEFRAME,
        "use_classification": "ACADEMIC",
    }
    approval_sha = _write_json(approval_path, approval)
    try:
        policy_value = json.loads(example_policy_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise AlpacaDataError(
            AlpacaDataErrorCode.AUTHORIZATION_INVALID,
            "example source policy cannot be loaded",
        ) from error
    if not isinstance(policy_value, dict):
        raise AlpacaDataError(
            AlpacaDataErrorCode.AUTHORIZATION_INVALID,
            "example source policy is malformed",
        )
    policy = cast("dict[str, object]", policy_value)
    policy["assessment_date"] = _now_utc().date().isoformat()
    policy["policy_id"] = "forecasting-poc-alpaca-academic-single-user-v1"
    policy["network_downloads_authorized"] = True
    policy["model_training_authorized"] = True
    policy["use_classification"] = "ACADEMIC"
    raw_sources = policy.get("sources")
    if not isinstance(raw_sources, list):
        raise AlpacaDataError(
            AlpacaDataErrorCode.AUTHORIZATION_INVALID,
            "example source inventory is malformed",
        )
    for raw_source in raw_sources:
        if not isinstance(raw_source, dict) or raw_source.get("source_id") != SOURCE_ID:
            continue
        source = cast("dict[str, object]", raw_source)
        source["enabled"] = True
        source["adapter_implementation_authorized"] = True
        source["network_download_authorized"] = True
        source["authorization_status"] = "APPROVED"
        source["approval_record_sha256"] = approval_sha
        source["approval_expires_at_utc"] = expires_at_utc.strftime(
            "%Y-%m-%dT%H:%M:%SZ"
        )
        source["rights"] = {
            "access": "DOCUMENTED",
            "persistent_storage": "DOCUMENTED",
            "model_training": "DOCUMENTED",
            "derived_data": "DOCUMENTED",
            "redistribution": "NOT_APPLICABLE",
            "retention_deletion": "DOCUMENTED",
        }
        break
    else:
        raise AlpacaDataError(
            AlpacaDataErrorCode.AUTHORIZATION_INVALID,
            "example policy does not contain the Alpaca source",
        )
    try:
        validate_policy(policy)
    except SourcePolicyError as error:
        raise AlpacaDataError(
            AlpacaDataErrorCode.AUTHORIZATION_INVALID,
            f"derived source policy is invalid: {error}",
        ) from error
    encoded_policy = _canonical_bytes(policy) + b"\n"
    _atomic_write(policy_path, encoded_policy)
    policy_sha = _sha256(encoded_policy)
    return approval_sha, policy_sha


def load_source_authorization(
    policy_path: Path,
    approval_path: Path,
    *,
    snapshot: UniverseSnapshot,
    data_root: Path,
) -> SourceAuthorization:
    """Validate the policy, approval content hash, scope, and expiry."""
    policy_payload = _read_secure(policy_path, MAX_APPROVAL_BYTES, "source policy")
    approval_payload = _read_secure(
        approval_path, MAX_APPROVAL_BYTES, "source approval record"
    )
    try:
        policy_value = json.loads(policy_payload)
        approval_value = json.loads(approval_payload)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AlpacaDataError(
            AlpacaDataErrorCode.AUTHORIZATION_INVALID,
            "source authorization evidence is malformed JSON",
        ) from error
    try:
        validate_policy(policy_value)
    except SourcePolicyError as error:
        raise AlpacaDataError(
            AlpacaDataErrorCode.AUTHORIZATION_INVALID,
            f"source policy rejected: {error}",
        ) from error
    if not isinstance(policy_value, dict) or not isinstance(approval_value, dict):
        raise AlpacaDataError(
            AlpacaDataErrorCode.AUTHORIZATION_INVALID,
            "source authorization evidence must be JSON objects",
        )
    policy = cast("dict[str, object]", policy_value)
    approval = cast("dict[str, object]", approval_value)
    supplied_document_hash = approval.pop("document_sha256", None)
    calculated_document_hash = _sha256(_canonical_bytes(approval))
    approval["document_sha256"] = supplied_document_hash
    if supplied_document_hash != calculated_document_hash:
        raise AlpacaDataError(
            AlpacaDataErrorCode.AUTHORIZATION_INVALID,
            "source approval document hash does not match",
        )
    policy_file_sha = _sha256(policy_payload)
    sources = policy.get("sources")
    source: Mapping[str, object] | None = None
    if isinstance(sources, list):
        for candidate in sources:
            if isinstance(candidate, dict) and candidate.get("source_id") == SOURCE_ID:
                source = cast("Mapping[str, object]", candidate)
                break
    if (
        source is None
        or source.get("enabled") is not True
        or source.get("network_download_authorized") is not True
        or source.get("adapter_implementation_authorized") is not True
        or source.get("authorization_status") != "APPROVED"
        or source.get("approval_record_sha256") != calculated_document_hash
        or policy.get("network_downloads_authorized") is not True
        or policy.get("model_training_authorized") is not True
        or policy.get("use_classification") != "ACADEMIC"
    ):
        raise AlpacaDataError(
            AlpacaDataErrorCode.AUTHORIZATION_INVALID,
            "source policy does not grant the exact Alpaca academic scope",
        )
    required_approval = {
        "decision": "APPROVED",
        "distribution": "INTERNAL_SINGLE_USER_ONLY",
        "feed": FEED,
        "live_trading_capable": False,
        "model_training_authorized": True,
        "plan": "BASIC",
        "redistribution_authorized": False,
        "source_id": SOURCE_ID,
        "ticker_source_path": snapshot.source_path,
        "ticker_source_sha256": snapshot.source_file_sha256.hex(),
        "timeframe": TIMEFRAME,
        "use_classification": "ACADEMIC",
    }
    if any(approval.get(key) != value for key, value in required_approval.items()):
        raise AlpacaDataError(
            AlpacaDataErrorCode.AUTHORIZATION_INVALID,
            "source approval scope does not match this execution",
        )
    expiry = _parse_utc_second(approval.get("expires_at_utc"), "expires_at_utc")
    authorization = SourceAuthorization(
        policy_sha256=policy_file_sha,
        approval_sha256=calculated_document_hash,
        expires_at_utc=expiry,
        universe_source_sha256=snapshot.source_file_sha256.hex(),
        data_root=Path(cast("str", approval.get("data_root"))),
        use_classification="ACADEMIC",
    )
    authorization.validate(snapshot, data_root)
    return authorization


@dataclass(frozen=True, slots=True)
class TradingSession:
    """One provider calendar session with explicit UTC boundaries."""

    session_date: date
    open_utc: datetime
    close_utc: datetime

    def __post_init__(self) -> None:
        """Require an ordered UTC session interval."""
        if (
            self.open_utc.tzinfo is None
            or self.close_utc.tzinfo is None
            or self.open_utc.utcoffset() != timedelta(0)
            or self.close_utc.utcoffset() != timedelta(0)
            or self.close_utc <= self.open_utc
            or self.close_utc - self.open_utc > timedelta(hours=7)
        ):
            raise AlpacaDataError(
                AlpacaDataErrorCode.CALENDAR_INVALID,
                "calendar session boundaries are invalid",
            )

    def to_dict(self) -> dict[str, str]:
        """Return an explicit UTC representation for hashing and reports."""
        return {
            "close_utc": self.close_utc.isoformat().replace("+00:00", "Z"),
            "date": self.session_date.isoformat(),
            "open_utc": self.open_utc.isoformat().replace("+00:00", "Z"),
        }


def _calendar_time(session_date: date, value: object) -> datetime:
    if not isinstance(value, str):
        raise AlpacaDataError(
            AlpacaDataErrorCode.CALENDAR_INVALID, "calendar time is not text"
        )
    try:
        parsed_time = datetime_time.fromisoformat(value)
    except ValueError as error:
        raise AlpacaDataError(
            AlpacaDataErrorCode.CALENDAR_INVALID, "calendar time is malformed"
        ) from error
    local = datetime.combine(session_date, parsed_time, tzinfo=SESSION_ZONE)
    return local.astimezone(UTC)


def parse_calendar(value: object) -> tuple[TradingSession, ...]:
    """Strictly parse and order an Alpaca calendar response."""
    if not isinstance(value, list) or len(value) > 3_660:
        raise AlpacaDataError(
            AlpacaDataErrorCode.CALENDAR_INVALID,
            "calendar response is not a bounded array",
        )
    sessions: list[TradingSession] = []
    for raw in value:
        if not isinstance(raw, dict):
            raise AlpacaDataError(
                AlpacaDataErrorCode.CALENDAR_INVALID,
                "calendar entry is not an object",
            )
        try:
            session_date = date.fromisoformat(cast("str", raw["date"]))
            opened = _calendar_time(session_date, raw["open"])
            closed = _calendar_time(session_date, raw["close"])
        except (KeyError, TypeError, ValueError) as error:
            raise AlpacaDataError(
                AlpacaDataErrorCode.CALENDAR_INVALID,
                "calendar entry fields are malformed",
            ) from error
        sessions.append(TradingSession(session_date, opened, closed))
    ordered = tuple(sorted(sessions, key=lambda item: item.session_date))
    if len({item.session_date for item in ordered}) != len(ordered):
        raise AlpacaDataError(
            AlpacaDataErrorCode.CALENDAR_INVALID,
            "calendar response contains duplicate sessions",
        )
    return ordered


def _two_year_start(end: date) -> date:
    try:
        anniversary = end.replace(year=end.year - 2)
    except ValueError:
        anniversary = end.replace(year=end.year - 2, day=28)
    return anniversary + timedelta(days=1)


def resolve_run_sessions(
    client: AlpacaApiClient, mode: RunMode
) -> tuple[date, tuple[TradingSession, ...], str, str]:
    """Resolve the latest complete session and exact pilot/backfill window."""
    clock = client.clock()
    if not isinstance(clock.value, dict):
        raise AlpacaDataError(
            AlpacaDataErrorCode.CALENDAR_INVALID, "clock response is malformed"
        )
    clock_value = cast("dict[str, object]", clock.value)
    timestamp = _parse_clock_datetime(clock_value.get("timestamp"))
    end_search = timestamp.date()
    start_search = (
        end_search - timedelta(days=30)
        if mode is RunMode.PILOT
        else _two_year_start(end_search - timedelta(days=1)) - timedelta(days=10)
    )
    calendar = client.calendar(start_search, end_search)
    sessions = parse_calendar(calendar.value)
    complete = tuple(item for item in sessions if item.close_utc <= timestamp)
    if not complete:
        raise AlpacaDataError(
            AlpacaDataErrorCode.CALENDAR_INVALID,
            "provider calendar contains no complete session",
        )
    latest = complete[-1].session_date
    if mode is RunMode.PILOT:
        selected = complete[-5:]
        if len(selected) != 5:
            raise AlpacaDataError(
                AlpacaDataErrorCode.CALENDAR_INVALID,
                "five complete pilot sessions are unavailable",
            )
    else:
        start = _two_year_start(latest)
        selected = tuple(
            item for item in complete if start <= item.session_date <= latest
        )
        has_pre_start_coverage = any(item.session_date < start for item in complete)
        if not selected or not has_pre_start_coverage:
            raise AlpacaDataError(
                AlpacaDataErrorCode.CALENDAR_INVALID,
                "two-year calendar response does not cover the requested boundary",
            )
    calendar_sha = _sha256(
        _canonical_bytes([session.to_dict() for session in selected])
    )
    source_hash = _sha256(clock.body + b"\n" + calendar.body)
    return latest, selected, calendar_sha, source_hash


@dataclass(frozen=True, slots=True)
class FetchTask:
    """One bounded symbol/session request group."""

    symbols: tuple[str, ...]
    sessions: tuple[TradingSession, ...]

    @property
    def task_id(self) -> str:
        """Derive the immutable task ID from symbols and session bounds."""
        value = {
            "sessions": [item.to_dict() for item in self.sessions],
            "symbols": list(self.symbols),
        }
        return _sha256(_canonical_bytes(value))[:24]


def _chunks[T](values: Sequence[T], size: int) -> tuple[tuple[T, ...], ...]:
    return tuple(
        tuple(values[index : index + size]) for index in range(0, len(values), size)
    )


def make_tasks(
    symbols: Sequence[str], sessions: Sequence[TradingSession]
) -> tuple[FetchTask, ...]:
    """Build deterministic bounded tasks."""
    tasks = [
        FetchTask(symbol_batch, session_batch)
        for session_batch in _chunks(tuple(sessions), SESSION_BATCH_SIZE)
        for symbol_batch in _chunks(tuple(symbols), SYMBOL_BATCH_SIZE)
    ]
    return tuple(tasks)


def _cluster_quota_evidence(root: Path) -> QuotaEvidence:
    """Mirror the cluster's hdquota scratch calculation with exact bytes."""
    observed = _now_utc()
    status = os.statvfs(root)
    total = status.f_bsize * status.f_blocks
    free = status.f_bsize * status.f_bfree
    used = total - free
    limit = 10 * BINARY_TIB if total == 12 * BINARY_TIB else total
    return QuotaEvidence(
        limit_bytes=limit,
        used_bytes=used,
        source="/opt/rci/bin/hdquota -s exact statvfs calculation",
        observed_at_utc=observed.isoformat(),
        authoritative=True,
    )


def _price_nanos(value: object) -> int:
    if isinstance(value, bool) or not isinstance(value, (int, float, str)):
        raise AlpacaDataError(
            AlpacaDataErrorCode.DATA_INVALID, "bar price is not numeric"
        )
    try:
        decimal = Decimal(str(value))
        scaled = decimal * PRICE_SCALE
    except InvalidOperation as error:
        raise AlpacaDataError(
            AlpacaDataErrorCode.DATA_INVALID, "bar price is malformed"
        ) from error
    integral = scaled.to_integral_value()
    if not decimal.is_finite() or integral != scaled or not 0 < integral < (1 << 63):
        raise AlpacaDataError(
            AlpacaDataErrorCode.DATA_INVALID,
            "bar price cannot be represented as positive currency nanos",
        )
    return int(integral)


def _bar_count(value: object) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise AlpacaDataError(
            AlpacaDataErrorCode.DATA_INVALID, "bar volume is not a nonnegative integer"
        )
    return value


def _instrument_id(asset_id: str) -> str:
    digest = hashlib.sha256(f"ALPACA-ASSET-V1:{asset_id}".encode("ascii")).digest()
    return digest[:16].hex()


@dataclass(slots=True)
class TaskStatistics:
    """Bounded task result aggregated into the run report."""

    source_manifest_ids: list[str] = field(default_factory=list)
    partition_manifest_ids: list[str] = field(default_factory=list)
    source_bytes: int = 0
    canonical_bytes: int = 0
    raw_records: int = 0
    canonical_records: int = 0
    duplicates: int = 0
    out_of_session: int = 0
    missing_pairs: list[str] = field(default_factory=list)

    def to_dict(self) -> dict[str, object]:
        """Return bounded counts and immutable manifest references."""
        return {
            "canonical_bytes": self.canonical_bytes,
            "canonical_records": self.canonical_records,
            "duplicates": self.duplicates,
            "missing_pairs": sorted(self.missing_pairs),
            "out_of_session": self.out_of_session,
            "partition_manifest_ids": sorted(self.partition_manifest_ids),
            "raw_records": self.raw_records,
            "source_bytes": self.source_bytes,
            "source_manifest_ids": sorted(self.source_manifest_ids),
        }


def _manifest_times(
    event_min_ns: int,
    event_max_ns: int,
    receive_min_ns: int,
    receive_max_ns: int,
    processing_ns: int,
) -> ManifestTimeRange:
    processing = max(processing_ns, receive_max_ns)
    return ManifestTimeRange(
        event_time_min_ns=event_min_ns,
        event_time_max_ns=event_max_ns,
        publication_time_min_ns=receive_min_ns,
        publication_time_max_ns=receive_max_ns,
        receive_time_min_ns=receive_min_ns,
        receive_time_max_ns=receive_max_ns,
        processing_time_min_ns=max(processing_ns, receive_min_ns),
        processing_time_max_ns=processing,
        revision_time_min_ns=receive_min_ns,
        revision_time_max_ns=receive_max_ns,
    )


def _stage_payload(
    repository: DataRepository,
    lease: AdmissionLease,
    payload: bytes,
    final_relative: str,
) -> tuple[str, int]:
    digest = _sha256(payload)
    staged_relative = f"tmp/alpaca-{digest}.partial"
    staged_path = repository.root / staged_relative
    if staged_path.exists():
        existing = _read_secure(staged_path, max(len(payload), 1), "staged object")
        if existing != payload:
            raise AlpacaDataError(
                AlpacaDataErrorCode.INTEGRITY_FAILURE,
                "staged object conflicts with the expected payload",
            )
    else:
        _atomic_write(staged_path, payload, mode=0o600)
    repository.publish_staged_object(
        staged_relative,
        final_relative,
        expected_sha256=digest,
        expected_size_bytes=len(payload),
        lease=lease,
    )
    return digest, len(payload)


def _page_bars(
    value: object,
) -> tuple[list[tuple[str, Mapping[str, object]]], str | None]:
    if not isinstance(value, dict):
        raise AlpacaDataError(
            AlpacaDataErrorCode.RESPONSE_MALFORMED,
            "historical-bars response is not an object",
        )
    bars_value = value.get("bars")
    if not isinstance(bars_value, dict):
        raise AlpacaDataError(
            AlpacaDataErrorCode.RESPONSE_MALFORMED,
            "historical-bars response lacks a bars object",
        )
    entries: list[tuple[str, Mapping[str, object]]] = []
    for symbol, raw_bars in bars_value.items():
        if not isinstance(symbol, str) or not isinstance(raw_bars, list):
            raise AlpacaDataError(
                AlpacaDataErrorCode.RESPONSE_MALFORMED,
                "historical-bars symbol payload is malformed",
            )
        for raw_bar in raw_bars:
            if not isinstance(raw_bar, dict):
                raise AlpacaDataError(
                    AlpacaDataErrorCode.RESPONSE_MALFORMED,
                    "historical bar is not an object",
                )
            entries.append((symbol, cast("Mapping[str, object]", raw_bar)))
    token = value.get("next_page_token")
    if token is not None and (
        not isinstance(token, str) or not token or len(token) > 4096
    ):
        raise AlpacaDataError(
            AlpacaDataErrorCode.RESPONSE_MALFORMED,
            "historical-bars page token is malformed",
        )
    return entries, token


def _publish_source_page(
    repository: DataRepository,
    lease: AdmissionLease,
    *,
    run_id: str,
    task: FetchTask,
    page_index: int,
    result: ApiResult,
    universe_sha256: str,
) -> SourceManifest:
    entries, _ = _page_bars(result.value)
    timestamps = [_parse_rfc3339_ns(raw.get("t"))[1] for _, raw in entries]
    event_min = min(timestamps, default=_utc_ns(task.sessions[0].open_utc))
    event_max = max(timestamps, default=_utc_ns(task.sessions[-1].close_utc))
    received = _utc_ns(result.received_at_utc)
    processed = _utc_ns(_now_utc())
    digest = _sha256(result.body)
    relative = f"raw/alpaca-iex-minute/{digest[:2]}/{digest}.json"
    _, size = _stage_payload(repository, lease, result.body, relative)
    manifest = SourceManifest(
        source_name=SOURCE_ID,
        source_version=SOURCE_VERSION,
        source_object_id=(f"run:{run_id}:task:{task.task_id}:page:{page_index:04d}"),
        storage_path=relative,
        object_sha256=digest,
        size_bytes=size,
        record_count=len(entries),
        schema_name="alpaca-market-data-v2-bars-response",
        schema_version=SCHEMA_VERSION,
        times=_manifest_times(event_min, event_max, received, received, processed),
        universe_snapshot_sha256=universe_sha256,
        lineage=ManifestLineage(),
    )
    repository.publish_manifest(manifest, lease)
    return manifest


def _load_source_page(
    repository: DataRepository, manifest_id: str
) -> tuple[SourceManifest, object]:
    manifest = repository.load_manifest(manifest_id)
    if not isinstance(manifest, SourceManifest) or manifest.source_name != SOURCE_ID:
        raise AlpacaDataError(
            AlpacaDataErrorCode.CHECKPOINT_CORRUPT,
            "checkpoint references an incompatible source manifest",
        )
    payload = _read_secure(
        repository.root / manifest.storage_path,
        MAX_RESPONSE_BYTES,
        "raw Alpaca source page",
    )
    if (
        len(payload) != manifest.size_bytes
        or _sha256(payload) != manifest.object_sha256
    ):
        raise AlpacaDataError(
            AlpacaDataErrorCode.INTEGRITY_FAILURE,
            "raw Alpaca source page does not match its manifest",
        )
    try:
        return manifest, json.loads(payload)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AlpacaDataError(
            AlpacaDataErrorCode.RESPONSE_MALFORMED,
            "raw Alpaca source page is malformed",
        ) from error


def _normalize_bar(
    symbol: str,
    raw: Mapping[str, object],
    source_manifest_id: str,
    sessions_by_date: Mapping[date, TradingSession],
    assets: Mapping[str, Mapping[str, object]],
) -> tuple[dict[str, object] | None, str]:
    parsed, timestamp_ns = _parse_rfc3339_ns(raw.get("t"))
    local_date = parsed.astimezone(SESSION_ZONE).date()
    session = sessions_by_date.get(local_date)
    if session is None or not session.open_utc <= parsed < session.close_utc:
        return None, "OUT_OF_SESSION"
    if timestamp_ns % 60_000_000_000:
        raise AlpacaDataError(
            AlpacaDataErrorCode.DATA_INVALID,
            "minute-bar timestamp is not minute aligned",
        )
    opened = _price_nanos(raw.get("o"))
    high = _price_nanos(raw.get("h"))
    low = _price_nanos(raw.get("l"))
    closed = _price_nanos(raw.get("c"))
    volume = _bar_count(raw.get("v"))
    if high < max(opened, closed, low) or low > min(opened, closed, high):
        raise AlpacaDataError(
            AlpacaDataErrorCode.DATA_INVALID, "minute-bar OHLC invariants fail"
        )
    asset = assets.get(symbol, {})
    raw_asset_id = asset.get("asset_id")
    instrument_id = (
        _instrument_id(raw_asset_id) if isinstance(raw_asset_id, str) else None
    )
    row = {
        "adjustment": ADJUSTMENT,
        "bar_start_exchange_event_time_ns": timestamp_ns,
        "close_price_currency_nanos": closed,
        "feed": FEED,
        "high_price_currency_nanos": high,
        "historical_ingest_limitation": (
            "original publication and local receive time were not observed; "
            "source receipt is retained separately"
        ),
        "instrument_id": instrument_id,
        "instrument_resolution": asset.get("status", "UNRESOLVED"),
        "low_price_currency_nanos": low,
        "open_price_currency_nanos": opened,
        "price_unit": "USD_NANOS",
        "quantity_unit": "SHARES",
        "schema_version": CANONICAL_SCHEMA_VERSION,
        "session_date": local_date.isoformat(),
        "source_manifest_id": source_manifest_id,
        "symbol": symbol,
        "volume_shares": volume,
    }
    return row, "ACCEPTED"


def _publish_partitions(
    repository: DataRepository,
    lease: AdmissionLease,
    *,
    task: FetchTask,
    manifest_ids: Sequence[str],
    universe_sha256: str,
    assets: Mapping[str, Mapping[str, object]],
) -> TaskStatistics:
    statistics = TaskStatistics(source_manifest_ids=list(manifest_ids))
    sessions_by_date = {item.session_date: item for item in task.sessions}
    grouped: dict[tuple[str, date], dict[int, dict[str, object]]] = defaultdict(dict)
    source_manifests: dict[str, SourceManifest] = {}
    allowed_symbols = set(task.symbols)
    for manifest_id in manifest_ids:
        manifest, value = _load_source_page(repository, manifest_id)
        source_manifests[manifest_id] = manifest
        entries, _ = _page_bars(value)
        statistics.source_bytes += manifest.size_bytes
        statistics.raw_records += len(entries)
        for symbol, raw in entries:
            if symbol not in allowed_symbols:
                raise AlpacaDataError(
                    AlpacaDataErrorCode.DATA_INVALID,
                    "provider returned an out-of-scope symbol",
                )
            row, disposition = _normalize_bar(
                symbol, raw, manifest_id, sessions_by_date, assets
            )
            if disposition == "OUT_OF_SESSION":
                statistics.out_of_session += 1
                continue
            accepted = cast("dict[str, object]", row)
            session_date = date.fromisoformat(cast("str", accepted["session_date"]))
            timestamp = cast("int", accepted["bar_start_exchange_event_time_ns"])
            existing = grouped[(symbol, session_date)].get(timestamp)
            if existing is not None:
                comparable_existing = {
                    k: v for k, v in existing.items() if k != "source_manifest_id"
                }
                comparable_new = {
                    k: v for k, v in accepted.items() if k != "source_manifest_id"
                }
                if comparable_existing != comparable_new:
                    raise AlpacaDataError(
                        AlpacaDataErrorCode.DATA_INVALID,
                        "conflicting duplicate minute bar detected",
                    )
                statistics.duplicates += 1
                continue
            grouped[(symbol, session_date)][timestamp] = accepted

    for symbol in task.symbols:
        for session in task.sessions:
            rows_by_time = grouped.get((symbol, session.session_date), {})
            if not rows_by_time:
                statistics.missing_pairs.append(
                    f"{symbol}:{session.session_date.isoformat()}"
                )
                continue
            rows = [rows_by_time[key] for key in sorted(rows_by_time)]
            payload = b"".join(_canonical_bytes(row) + b"\n" for row in rows)
            digest = _sha256(payload)
            relative = (
                f"canonical/alpaca-iex-minute/symbol={symbol}/"
                f"session_date={session.session_date.isoformat()}/{digest}.jsonl"
            )
            _, size = _stage_payload(repository, lease, payload, relative)
            sources = tuple(
                sorted({cast("str", row["source_manifest_id"]) for row in rows})
            )
            source_ranges = [source_manifests[item].times for item in sources]
            event_values = [
                cast("int", row["bar_start_exchange_event_time_ns"]) for row in rows
            ]
            received_min = min(item.receive_time_min_ns for item in source_ranges)
            received_max = max(item.receive_time_max_ns for item in source_ranges)
            partition = PartitionManifest(
                dataset_name=DATASET_ID,
                partition_key=(
                    f"symbol={symbol}/session={session.session_date.isoformat()}"
                ),
                storage_path=relative,
                object_sha256=digest,
                size_bytes=size,
                record_count=len(rows),
                schema_name="aegis-alpaca-minute-bar",
                schema_version=CANONICAL_SCHEMA_VERSION,
                times=_manifest_times(
                    min(event_values),
                    max(event_values),
                    received_min,
                    received_max,
                    _utc_ns(_now_utc()),
                ),
                universe_snapshot_sha256=universe_sha256,
                source_manifest_ids=sources,
                lineage=ManifestLineage(),
            )
            repository.publish_manifest(partition, lease)
            statistics.partition_manifest_ids.append(partition.manifest_id)
            statistics.canonical_bytes += size
            statistics.canonical_records += len(rows)
    return statistics


def _checkpoint_path(repository: DataRepository, mode: RunMode, run_id: str) -> Path:
    return (
        repository.root
        / "tmp/checkpoints"
        / f"alpaca-iex-{mode.value.lower()}-{run_id}.json"
    )


def _initial_checkpoint(
    mode: RunMode, run_id: str, plan_sha256: str
) -> dict[str, object]:
    return {
        "active_task": None,
        "completed_task_ids": [],
        "mode": mode.value,
        "plan_sha256": plan_sha256,
        "run_id": run_id,
        "schema_version": SCHEMA_VERSION,
    }


def _load_checkpoint(
    path: Path, mode: RunMode, run_id: str, plan_sha256: str
) -> dict[str, object]:
    if not path.exists():
        return _initial_checkpoint(mode, run_id, plan_sha256)
    value, _ = _load_hashed_json(path, "Alpaca backfill checkpoint")
    if (
        value.get("schema_version") != SCHEMA_VERSION
        or value.get("mode") != mode.value
        or value.get("run_id") != run_id
        or value.get("plan_sha256") != plan_sha256
        or not isinstance(value.get("completed_task_ids"), list)
    ):
        raise AlpacaDataError(
            AlpacaDataErrorCode.CHECKPOINT_CORRUPT,
            "checkpoint does not match the immutable run plan",
        )
    return value


def _task_report_path(
    repository: DataRepository, mode: RunMode, run_id: str, task_id: str
) -> Path:
    return (
        repository.root
        / "reports/alpaca-iex-minute"
        / mode.value.lower()
        / "tasks"
        / run_id
        / f"{task_id}.json"
    )


def _load_task_statistics(path: Path) -> TaskStatistics:
    value, _ = _load_hashed_json(path, "Alpaca task report")
    statistics = value.get("statistics")
    if not isinstance(statistics, dict):
        raise AlpacaDataError(
            AlpacaDataErrorCode.INTEGRITY_FAILURE,
            "task report statistics are malformed",
        )
    try:
        return TaskStatistics(
            source_manifest_ids=list(
                cast("list[str]", statistics["source_manifest_ids"])
            ),
            partition_manifest_ids=list(
                cast("list[str]", statistics["partition_manifest_ids"])
            ),
            source_bytes=cast("int", statistics["source_bytes"]),
            canonical_bytes=cast("int", statistics["canonical_bytes"]),
            raw_records=cast("int", statistics["raw_records"]),
            canonical_records=cast("int", statistics["canonical_records"]),
            duplicates=cast("int", statistics["duplicates"]),
            out_of_session=cast("int", statistics["out_of_session"]),
            missing_pairs=list(cast("list[str]", statistics["missing_pairs"])),
        )
    except (KeyError, TypeError, ValueError) as error:
        raise AlpacaDataError(
            AlpacaDataErrorCode.INTEGRITY_FAILURE,
            "task report statistics contain invalid fields",
        ) from error


def _save_checkpoint(path: Path, checkpoint: dict[str, object]) -> None:
    _write_json(path, checkpoint)
    if path.stat().st_size > MAX_CHECKPOINT_BYTES:
        raise AlpacaDataError(
            AlpacaDataErrorCode.CHECKPOINT_CORRUPT,
            "checkpoint exceeds its configured size bound",
        )


def _execute_task(
    client: AlpacaApiClient,
    repository: DataRepository,
    lease: AdmissionLease,
    *,
    task: FetchTask,
    run_id: str,
    universe_sha256: str,
    assets: Mapping[str, Mapping[str, object]],
    checkpoint: dict[str, object],
    checkpoint_path: Path,
) -> TaskStatistics:
    active = checkpoint.get("active_task")
    page_ids: list[str] = []
    next_token: str | None = None
    page_index = 0
    download_complete = False
    if active is not None:
        if not isinstance(active, dict) or active.get("task_id") != task.task_id:
            raise AlpacaDataError(
                AlpacaDataErrorCode.CHECKPOINT_CORRUPT,
                "checkpoint active task conflicts with deterministic ordering",
            )
        raw_ids = active.get("source_manifest_ids")
        raw_token = active.get("next_page_token")
        raw_page_index = active.get("next_page_index")
        raw_download_complete = active.get("download_complete")
        if (
            not isinstance(raw_ids, list)
            or not all(isinstance(item, str) for item in raw_ids)
            or (raw_token is not None and not isinstance(raw_token, str))
            or not isinstance(raw_page_index, int)
            or raw_page_index != len(raw_ids)
            or (
                raw_download_complete is not None
                and not isinstance(raw_download_complete, bool)
            )
        ):
            raise AlpacaDataError(
                AlpacaDataErrorCode.CHECKPOINT_CORRUPT,
                "checkpoint active task fields are malformed",
            )
        page_ids = list(cast("list[str]", raw_ids))
        next_token = raw_token
        page_index = raw_page_index
        # Legacy checkpoints are unambiguous: at least one durable page and no
        # continuation token means that final-page acquisition completed.
        download_complete = (
            raw_download_complete
            if isinstance(raw_download_complete, bool)
            else page_index > 0 and next_token is None
        )
    else:
        checkpoint["active_task"] = {
            "download_complete": False,
            "next_page_index": 0,
            "next_page_token": None,
            "source_manifest_ids": [],
            "task_id": task.task_id,
        }
        _save_checkpoint(checkpoint_path, checkpoint)

    while not download_complete:
        if page_index >= MAX_PAGES_PER_TASK:
            raise AlpacaDataError(
                AlpacaDataErrorCode.RESPONSE_TOO_LARGE,
                "historical-bars task exceeded its page bound",
            )
        result = client.bars(
            task.symbols,
            task.sessions[0].open_utc,
            task.sessions[-1].close_utc,
            next_token,
        )
        _, returned_token = _page_bars(result.value)
        manifest = _publish_source_page(
            repository,
            lease,
            run_id=run_id,
            task=task,
            page_index=page_index,
            result=result,
            universe_sha256=universe_sha256,
        )
        page_ids.append(manifest.manifest_id)
        page_index += 1
        checkpoint["active_task"] = {
            "download_complete": returned_token is None,
            "next_page_index": page_index,
            "next_page_token": returned_token,
            "source_manifest_ids": page_ids,
            "task_id": task.task_id,
        }
        _save_checkpoint(checkpoint_path, checkpoint)
        if returned_token is None:
            download_complete = True
            break
        next_token = returned_token

    statistics = _publish_partitions(
        repository,
        lease,
        task=task,
        manifest_ids=page_ids,
        universe_sha256=universe_sha256,
        assets=assets,
    )
    task_report = {
        "run_id": run_id,
        "schema_version": SCHEMA_VERSION,
        "statistics": statistics.to_dict(),
        "task_id": task.task_id,
    }
    _write_json(
        _task_report_path(
            repository, RunMode(cast("str", checkpoint["mode"])), run_id, task.task_id
        ),
        task_report,
    )
    completed = cast("list[str]", checkpoint["completed_task_ids"])
    completed.append(task.task_id)
    checkpoint["completed_task_ids"] = sorted(set(completed))
    checkpoint["active_task"] = None
    _save_checkpoint(checkpoint_path, checkpoint)
    return statistics


def _asset_status(client: AlpacaApiClient, symbol: str) -> dict[str, object]:
    result = client.asset(symbol)
    if result.status == 404:
        return {
            "asset_id": None,
            "asset_class": None,
            "exchange": None,
            "reason": "PROVIDER_ASSET_NOT_FOUND",
            "status": "UNSUPPORTED",
            "symbol": symbol,
        }
    if not isinstance(result.value, dict):
        raise AlpacaDataError(
            AlpacaDataErrorCode.RESPONSE_MALFORMED,
            "asset response is not an object",
        )
    value = cast("dict[str, object]", result.value)
    if value.get("symbol") != symbol:
        raise AlpacaDataError(
            AlpacaDataErrorCode.DATA_INVALID,
            "asset response symbol does not match the request",
        )
    asset_id = value.get("id")
    asset_class = value.get("class")
    status = value.get("status")
    exchange = value.get("exchange")
    if not all(
        isinstance(item, str) and item
        for item in (asset_id, asset_class, status, exchange)
    ):
        raise AlpacaDataError(
            AlpacaDataErrorCode.RESPONSE_MALFORMED,
            "asset response identity fields are malformed",
        )
    supported = asset_class == "us_equity" and status == "active" and exchange != "OTC"
    return {
        "asset_id": asset_id,
        "asset_class": asset_class,
        "exchange": exchange,
        "reason": "SUPPORTED" if supported else "UNSUPPORTED_ASSET",
        "status": "RESOLVED" if supported else "UNSUPPORTED",
        "symbol": symbol,
    }


def _aggregate(statistics: Sequence[TaskStatistics]) -> TaskStatistics:
    total = TaskStatistics()
    for item in statistics:
        total.source_manifest_ids.extend(item.source_manifest_ids)
        total.partition_manifest_ids.extend(item.partition_manifest_ids)
        total.source_bytes += item.source_bytes
        total.canonical_bytes += item.canonical_bytes
        total.raw_records += item.raw_records
        total.canonical_records += item.canonical_records
        total.duplicates += item.duplicates
        total.out_of_session += item.out_of_session
        total.missing_pairs.extend(item.missing_pairs)
    total.source_manifest_ids = sorted(set(total.source_manifest_ids))
    total.partition_manifest_ids = sorted(set(total.partition_manifest_ids))
    total.missing_pairs = sorted(set(total.missing_pairs))
    return total


def _load_pilot_acceptance(
    acceptance_path: Path,
    *,
    authorization: SourceAuthorization,
    snapshot: UniverseSnapshot,
) -> dict[str, object]:
    acceptance, _ = _load_hashed_json(acceptance_path, "pilot acceptance")
    if (
        acceptance.get("accepted") is not True
        or acceptance.get("mode") != RunMode.PILOT.value
        or acceptance.get("source_policy_sha256") != authorization.policy_sha256
        or acceptance.get("approval_record_sha256") != authorization.approval_sha256
        or acceptance.get("universe_snapshot_sha256")
        != snapshot.universe_snapshot_sha256.hex()
        or acceptance.get("repository_verification_passed") is not True
    ):
        raise AlpacaDataError(
            AlpacaDataErrorCode.PILOT_NOT_ACCEPTED,
            "pilot acceptance does not authorize this backfill",
        )
    return acceptance


def _storage_projection(
    mode: RunMode,
    sessions: Sequence[TradingSession],
    symbol_count: int,
    pilot_acceptance: Mapping[str, object] | None,
) -> tuple[int, int, dict[str, object]]:
    maximum_records = (
        sum(
            int((item.close_utc - item.open_utc).total_seconds() // 60)
            for item in sessions
        )
        * symbol_count
    )
    if mode is RunMode.PILOT:
        output = 1_000_000_000
        temporary = 200_000_000
        basis = {"method": "CONSERVATIVE_PILOT_CAP", "maximum_records": maximum_records}
        return output, temporary, basis
    if pilot_acceptance is None:
        raise AlpacaDataError(
            AlpacaDataErrorCode.PILOT_NOT_ACCEPTED,
            "backfill projection requires an accepted pilot",
        )
    metrics = pilot_acceptance.get("pilot_metrics")
    if not isinstance(metrics, dict):
        raise AlpacaDataError(
            AlpacaDataErrorCode.PILOT_NOT_ACCEPTED,
            "pilot acceptance lacks storage metrics",
        )
    raw_records = metrics.get("raw_records")
    raw_bytes = metrics.get("source_bytes")
    canonical_records = metrics.get("canonical_records")
    canonical_bytes = metrics.get("canonical_bytes")
    if not all(
        isinstance(value, int) and not isinstance(value, bool) and value > 0
        for value in (raw_records, raw_bytes, canonical_records, canonical_bytes)
    ):
        raise AlpacaDataError(
            AlpacaDataErrorCode.PILOT_NOT_ACCEPTED,
            "pilot storage metrics are invalid",
        )
    raw_rate = cast("int", raw_bytes) / cast("int", raw_records)
    canonical_rate = cast("int", canonical_bytes) / cast("int", canonical_records)
    payload_estimate = int(maximum_records * (raw_rate + canonical_rate) * 1.5)
    partition_count = len(sessions) * symbol_count
    manifest_estimate = partition_count * 8_192 + 100_000_000
    output = max(1_000_000_000, payload_estimate + manifest_estimate)
    temporary = 200_000_000
    return (
        output,
        temporary,
        {
            "canonical_bytes_per_record": canonical_rate,
            "manifest_and_report_bytes": manifest_estimate,
            "maximum_records": maximum_records,
            "method": "PILOT_RATES_X_MAXIMUM_RECORDS_X_1_5",
            "raw_bytes_per_record": raw_rate,
        },
    )


def _report_path(root: Path, mode: RunMode) -> Path:
    return root / "reports/alpaca-iex-minute" / mode.value.lower() / "report.json"


def _write_human_report(path: Path, report: Mapping[str, object]) -> None:
    counts = cast("Mapping[str, object]", report["counts"])
    storage = cast("Mapping[str, object]", report["storage"])
    assets = cast("Mapping[str, object]", report["assets"])
    unsupported = sorted(
        symbol
        for symbol, state in assets.items()
        if not isinstance(state, dict) or state.get("status") != "RESOLVED"
    )
    lines = [
        f"# Alpaca IEX minute {str(report['mode']).lower()} report",
        "",
        f"- Status: `{report['status']}`",
        f"- Date range: `{report['start_date']}` through `{report['end_date']}`",
        f"- Latest complete market date: `{report['latest_complete_market_date']}`",
        f"- Sessions: `{report['session_count']}`",
        f"- Requested symbols: `{counts['requested_symbols']}`",
        f"- Unsupported symbols: `{','.join(unsupported) if unsupported else 'none'}`",
        f"- Source objects: `{counts['source_objects']}`",
        f"- Raw records: `{counts['raw_records']}`",
        f"- Canonical partitions: `{counts['canonical_partitions']}`",
        f"- Canonical records: `{counts['canonical_records']}`",
        f"- Identical duplicate bars removed: `{counts['duplicate_bars']}`",
        f"- Out-of-session bars rejected: `{counts['out_of_session_bars_rejected']}`",
        f"- Missing symbol/session pairs: `{counts['missing_symbol_session_pairs']}`",
        f"- Source bytes: `{storage['source_bytes']}`",
        f"- Canonical bytes: `{storage['canonical_bytes']}`",
        (
            "- Provider requests/retries: "
            f"`{report['provider_request_count']}` / "
            f"`{report['provider_retry_count']}`"
        ),
        f"- Duration seconds: `{report['duration_seconds']}`",
        f"- Dataset ID: `{report['dataset_id']}`",
        f"- Plan SHA-256: `{report['plan_sha256']}`",
        f"- Universe SHA-256: `{report['universe_snapshot_sha256']}`",
        f"- Source policy SHA-256: `{report['source_policy_sha256']}`",
        f"- Adjustment/timeframe: `{report['adjustment']}` / `{report['timeframe']}`",
        "- Feed: `IEX`; these bars are not consolidated SIP market coverage.",
        "- Missing minutes and unsupported symbols are never synthetically filled.",
        (
            "- Historical publication latency is unavailable; acquisition receipt "
            "and processing times are retained."
        ),
        "- Use: owner-only internal academic/non-commercial research.",
        "- Trading capability: none.",
        "",
    ]
    _atomic_write(path, "\n".join(lines).encode("utf-8"), mode=0o600)


def execute_run(
    *,
    mode: RunMode,
    data_root: Path,
    policy_path: Path,
    approval_path: Path,
    secrets_path: Path,
    pilot_acceptance_path: Path = DEFAULT_PILOT_ACCEPTANCE,
    transport: AlpacaTransport | None = None,
) -> dict[str, object]:
    """Execute one admitted pilot or backfill with durable task checkpoints."""
    started = _now_utc()
    repository = DataRepository(data_root)
    repository.initialize()
    snapshot = load_ticker_universe(UnresolvedInstrumentResolver())
    authorization = load_source_authorization(
        policy_path,
        approval_path,
        snapshot=snapshot,
        data_root=data_root,
    )
    credentials = AlpacaCredentials.load(secrets_path)
    client = AlpacaApiClient(credentials, transport=transport)
    latest, sessions, calendar_sha, calendar_source_sha = resolve_run_sessions(
        client, mode
    )
    pilot_acceptance: Mapping[str, object] | None = None
    if mode is RunMode.BACKFILL:
        pilot_acceptance = _load_pilot_acceptance(
            pilot_acceptance_path,
            authorization=authorization,
            snapshot=snapshot,
        )
    output_bytes, temporary_bytes, projection_basis = _storage_projection(
        mode, sessions, len(snapshot.ordered_entries), pilot_acceptance
    )
    plan_fields = {
        "adjustment": ADJUSTMENT,
        "approval_record_sha256": authorization.approval_sha256,
        "calendar_sha256": calendar_sha,
        "end_date": sessions[-1].session_date.isoformat(),
        "feed": FEED,
        "mode": mode.value,
        "projected_output_bytes": output_bytes,
        "projected_temporary_bytes": temporary_bytes,
        "session_count": len(sessions),
        "source_policy_sha256": authorization.policy_sha256,
        "start_date": sessions[0].session_date.isoformat(),
        "symbols": list(snapshot.canonical_symbols),
        "timeframe": TIMEFRAME,
        "universe_snapshot_sha256": snapshot.universe_snapshot_sha256.hex(),
    }
    plan_sha = _sha256(_canonical_bytes(plan_fields))
    run_id = plan_sha[:24]
    existing_report_path = _report_path(data_root, mode)
    if existing_report_path.exists():
        existing, _ = _load_hashed_json(existing_report_path, "existing run report")
        if (
            existing.get("plan_sha256") == plan_sha
            and existing.get("status") == "COMPLETED"
        ):
            return existing
        raise AlpacaDataError(
            AlpacaDataErrorCode.INTEGRITY_FAILURE,
            "a different immutable run report already occupies the target path",
        )

    quota = _cluster_quota_evidence(data_root)
    storage_request = StorageRequest(
        operation_id=f"alpaca-iex-{mode.value.lower()}-{run_id}",
        output_bytes=output_bytes,
        temporary_bytes=temporary_bytes,
        retry_overhead_bytes=100_000_000,
    )
    tasks = make_tasks(snapshot.canonical_symbols, sessions)
    checkpoint_path = _checkpoint_path(repository, mode, run_id)
    checkpoint = _load_checkpoint(checkpoint_path, mode, run_id, plan_sha)
    completed = set(cast("list[str]", checkpoint["completed_task_ids"]))
    task_statistics: list[TaskStatistics] = []
    assets: dict[str, Mapping[str, object]] = {}
    if mode is RunMode.PILOT:
        for symbol in snapshot.ordered_entries:
            assets[symbol.symbol] = _asset_status(client, symbol.symbol)
    else:
        raw_assets = cast("Mapping[str, object]", pilot_acceptance)["assets"]
        if not isinstance(raw_assets, dict):
            raise AlpacaDataError(
                AlpacaDataErrorCode.PILOT_NOT_ACCEPTED,
                "pilot acceptance asset map is malformed",
            )
        assets = {
            str(symbol): cast("Mapping[str, object]", value)
            for symbol, value in raw_assets.items()
            if isinstance(value, dict)
        }
        if set(assets) != set(snapshot.canonical_symbols):
            raise AlpacaDataError(
                AlpacaDataErrorCode.PILOT_NOT_ACCEPTED,
                "pilot asset coverage does not match ticker.txt",
            )

    try:
        lease_context = repository.acquire_admission(storage_request, quota)
    except StorageError as error:
        raise AlpacaDataError(
            AlpacaDataErrorCode.STORAGE_REJECTED,
            f"storage admission rejected: {error}",
        ) from error
    with lease_context as lease:
        for task in tasks:
            task_path = _task_report_path(repository, mode, run_id, task.task_id)
            if task.task_id in completed:
                task_statistics.append(_load_task_statistics(task_path))
                continue
            statistics = _execute_task(
                client,
                repository,
                lease,
                task=task,
                run_id=run_id,
                universe_sha256=snapshot.universe_snapshot_sha256.hex(),
                assets=assets,
                checkpoint=checkpoint,
                checkpoint_path=checkpoint_path,
            )
            task_statistics.append(statistics)
        total = _aggregate(task_statistics)
        dataset_id = deterministic_dataset_id(
            total.partition_manifest_ids,
            universe_snapshot_sha256=snapshot.universe_snapshot_sha256.hex(),
            calendar_version=calendar_sha,
            schema_version=CANONICAL_SCHEMA_VERSION,
        )
        finished = _now_utc()
        report: dict[str, object] = {
            "adjustment": ADJUSTMENT,
            "approval_record_sha256": authorization.approval_sha256,
            "assets": assets,
            "calendar_sha256": calendar_sha,
            "calendar_source_response_sha256": calendar_source_sha,
            "counts": {
                "canonical_partitions": len(total.partition_manifest_ids),
                "canonical_records": total.canonical_records,
                "duplicate_bars": total.duplicates,
                "missing_symbol_session_pairs": len(total.missing_pairs),
                "out_of_session_bars_rejected": total.out_of_session,
                "raw_records": total.raw_records,
                "requested_symbols": len(snapshot.canonical_symbols),
                "source_objects": len(total.source_manifest_ids),
                "tasks": len(tasks),
            },
            "dataset_id": dataset_id,
            "duration_seconds": (finished - started).total_seconds(),
            "end_date": sessions[-1].session_date.isoformat(),
            "feed": FEED,
            "finished_at_utc": finished.isoformat().replace("+00:00", "Z"),
            "latest_complete_market_date": latest.isoformat(),
            "live_trading_capable": False,
            "missing_symbol_session_pairs": total.missing_pairs,
            "mode": mode.value,
            "network_access_performed": True,
            "partition_manifest_ids": total.partition_manifest_ids,
            "plan": plan_fields,
            "plan_sha256": plan_sha,
            "projection_basis": projection_basis,
            "provider": "AlpacaDB, Inc.",
            "provider_request_count": client.request_count,
            "provider_retry_count": client.retry_count,
            "quota": quota.to_dict(),
            "run_id": run_id,
            "schema_version": SCHEMA_VERSION,
            "session_count": len(sessions),
            "sessions": [item.to_dict() for item in sessions],
            "source_manifest_ids": total.source_manifest_ids,
            "source_policy_sha256": authorization.policy_sha256,
            "start_date": sessions[0].session_date.isoformat(),
            "started_at_utc": started.isoformat().replace("+00:00", "Z"),
            "status": "COMPLETED",
            "storage": {
                "canonical_bytes": total.canonical_bytes,
                "projected_output_bytes": output_bytes,
                "projected_temporary_bytes": temporary_bytes,
                "source_bytes": total.source_bytes,
            },
            "timeframe": TIMEFRAME,
            "ticker_source_path": snapshot.source_path,
            "ticker_source_sha256": snapshot.source_file_sha256.hex(),
            "universe_snapshot_sha256": snapshot.universe_snapshot_sha256.hex(),
        }
        _write_json(existing_report_path, report)
        _write_human_report(existing_report_path.with_suffix(".md"), report)
    if checkpoint_path.exists():
        checkpoint_path.unlink()
    return _load_hashed_json(existing_report_path, "run report")[0]


def _expected_coverage_keys(
    raw_sessions: list[object],
    raw_assets: Mapping[object, object],
    missing_pairs: list[object],
) -> tuple[set[str], set[str]]:
    """Validate and return exact expected and explicitly missing pair keys."""
    session_dates: list[str] = []
    for raw_session in raw_sessions:
        if not isinstance(raw_session, dict):
            raise AlpacaDataError(
                AlpacaDataErrorCode.INTEGRITY_FAILURE,
                "run report session coverage is malformed",
            )
        raw_date = raw_session.get("date")
        if not isinstance(raw_date, str):
            raise AlpacaDataError(
                AlpacaDataErrorCode.INTEGRITY_FAILURE,
                "run report session date is malformed",
            )
        try:
            parsed_date = date.fromisoformat(raw_date)
        except ValueError as error:
            raise AlpacaDataError(
                AlpacaDataErrorCode.INTEGRITY_FAILURE,
                "run report session date is malformed",
            ) from error
        if parsed_date.isoformat() != raw_date:
            raise AlpacaDataError(
                AlpacaDataErrorCode.INTEGRITY_FAILURE,
                "run report session date is not canonical",
            )
        session_dates.append(raw_date)
    if len(session_dates) != len(set(session_dates)):
        raise AlpacaDataError(
            AlpacaDataErrorCode.INTEGRITY_FAILURE,
            "run report contains duplicate session dates",
        )

    symbols = list(raw_assets)
    if not all(isinstance(symbol, str) and symbol for symbol in symbols):
        raise AlpacaDataError(
            AlpacaDataErrorCode.INTEGRITY_FAILURE,
            "run report asset coverage is malformed",
        )
    expected = {
        f"symbol={symbol}/session={session_date}"
        for symbol in cast("list[str]", symbols)
        for session_date in session_dates
    }
    missing_keys: set[str] = set()
    for raw_pair in missing_pairs:
        if not isinstance(raw_pair, str) or raw_pair.count(":") != 1:
            raise AlpacaDataError(
                AlpacaDataErrorCode.INTEGRITY_FAILURE,
                "run report missing-pair coverage is malformed",
            )
        symbol, session_date = raw_pair.split(":", 1)
        key = f"symbol={symbol}/session={session_date}"
        if key not in expected or key in missing_keys:
            raise AlpacaDataError(
                AlpacaDataErrorCode.INTEGRITY_FAILURE,
                "run report missing-pair coverage is invalid or duplicated",
            )
        missing_keys.add(key)
    return expected, missing_keys


def verify_run(
    report_path: Path,
    *,
    data_root: Path,
    sample_seed: int = DEFAULT_SAMPLE_SEED,
    sample_size: int = 64,
) -> dict[str, object]:
    """Verify manifests, partitions, coverage, duplicates, and a seeded sample."""
    report, report_file_sha = _load_hashed_json(report_path, "run report")
    if (
        report.get("status") != "COMPLETED"
        or report.get("live_trading_capable") is not False
    ):
        raise AlpacaDataError(
            AlpacaDataErrorCode.INTEGRITY_FAILURE,
            "run report is not complete and non-live",
        )
    repository = DataRepository(data_root)
    repository.initialize()
    repository_verification = repository.verify()
    if not repository_verification.passed:
        raise AlpacaDataError(
            AlpacaDataErrorCode.INTEGRITY_FAILURE,
            "repository manifest or corruption verification failed",
        )
    raw_ids = report.get("partition_manifest_ids")
    raw_sessions = report.get("sessions")
    raw_assets = report.get("assets")
    missing = report.get("missing_symbol_session_pairs")
    if (
        not isinstance(raw_ids, list)
        or not all(isinstance(item, str) for item in raw_ids)
        or not isinstance(raw_sessions, list)
        or not isinstance(raw_assets, dict)
        or not isinstance(missing, list)
    ):
        raise AlpacaDataError(
            AlpacaDataErrorCode.INTEGRITY_FAILURE,
            "run report coverage fields are malformed",
        )
    partition_ids = cast("list[str]", raw_ids)
    partition_keys: set[str] = set()
    duplicate_rows = 0
    total_records = 0
    for manifest_id in partition_ids:
        manifest = repository.load_manifest(manifest_id)
        if (
            not isinstance(manifest, PartitionManifest)
            or manifest.dataset_name != DATASET_ID
        ):
            raise AlpacaDataError(
                AlpacaDataErrorCode.INTEGRITY_FAILURE,
                "report references an incompatible canonical partition",
            )
        if manifest.partition_key in partition_keys:
            raise AlpacaDataError(
                AlpacaDataErrorCode.INTEGRITY_FAILURE,
                "duplicate canonical partition key detected",
            )
        partition_keys.add(manifest.partition_key)
        total_records += manifest.record_count
    expected_keys, missing_keys = _expected_coverage_keys(
        cast("list[object]", raw_sessions),
        cast("Mapping[object, object]", raw_assets),
        cast("list[object]", missing),
    )
    if partition_keys & missing_keys or partition_keys | missing_keys != expected_keys:
        raise AlpacaDataError(
            AlpacaDataErrorCode.INTEGRITY_FAILURE,
            "exact partition and missing-pair coverage does not match "
            "the requested universe",
        )
    sample_count = min(sample_size, len(partition_ids))
    generator = random.Random(sample_seed)
    sample = sorted(generator.sample(partition_ids, sample_count))
    sample_hashes: list[str] = []
    for manifest_id in sample:
        manifest = cast("PartitionManifest", repository.load_manifest(manifest_id))
        payload = _read_secure(
            data_root / manifest.storage_path,
            max(manifest.size_bytes, 1),
            "sampled canonical partition",
        )
        if _sha256(payload) != manifest.object_sha256:
            raise AlpacaDataError(
                AlpacaDataErrorCode.INTEGRITY_FAILURE,
                "sampled canonical partition hash mismatch",
            )
        seen: set[int] = set()
        lines = payload.splitlines()
        if len(lines) != manifest.record_count:
            raise AlpacaDataError(
                AlpacaDataErrorCode.INTEGRITY_FAILURE,
                "sampled canonical partition record count mismatch",
            )
        for encoded in lines:
            try:
                row = json.loads(encoded)
            except (UnicodeDecodeError, json.JSONDecodeError) as error:
                raise AlpacaDataError(
                    AlpacaDataErrorCode.INTEGRITY_FAILURE,
                    "sampled canonical row is malformed",
                ) from error
            if (
                not isinstance(row, dict)
                or row.get("schema_version") != CANONICAL_SCHEMA_VERSION
            ):
                raise AlpacaDataError(
                    AlpacaDataErrorCode.INTEGRITY_FAILURE,
                    "sampled canonical row has an incompatible schema",
                )
            timestamp = row.get("bar_start_exchange_event_time_ns")
            if not isinstance(timestamp, int) or timestamp in seen:
                duplicate_rows += 1
            else:
                seen.add(timestamp)
        sample_hashes.append(manifest.object_sha256)
    if duplicate_rows:
        raise AlpacaDataError(
            AlpacaDataErrorCode.INTEGRITY_FAILURE,
            "duplicate rows detected in sampled canonical partitions",
        )
    deterministic_hash = _sha256(
        _canonical_bytes(
            {
                "partition_manifest_ids": sorted(partition_ids),
                "report_file_sha256": report_file_sha,
                "sample_hashes": sample_hashes,
                "sample_seed": sample_seed,
            }
        )
    )
    return {
        "corruption_scan_passed": True,
        "deterministic_reinspection_sha256": deterministic_hash,
        "duplicate_rows": duplicate_rows,
        "manifest_verification": repository_verification.to_dict(),
        "partition_count": len(partition_ids),
        "record_count": total_records,
        "report_file_sha256": report_file_sha,
        "sample_seed": sample_seed,
        "sampled_partition_count": sample_count,
        "sampled_partition_ids": sample,
        "ticker_coverage_passed": True,
    }


def accept_pilot(
    report_path: Path,
    *,
    data_root: Path,
    acceptance_path: Path,
    sample_seed: int = DEFAULT_SAMPLE_SEED,
) -> dict[str, object]:
    """Verify and issue a content-bound acceptance record for Prompt 51."""
    report, report_file_sha = _load_hashed_json(report_path, "pilot report")
    if report.get("mode") != RunMode.PILOT.value:
        raise AlpacaDataError(
            AlpacaDataErrorCode.PILOT_NOT_ACCEPTED, "report is not a pilot"
        )
    verification = verify_run(report_path, data_root=data_root, sample_seed=sample_seed)
    acceptance = {
        "accepted": True,
        "approval_record_sha256": report["approval_record_sha256"],
        "assets": report["assets"],
        "mode": RunMode.PILOT.value,
        "pilot_metrics": {
            "canonical_bytes": cast("Mapping[str, object]", report["storage"])[
                "canonical_bytes"
            ],
            "canonical_records": cast("Mapping[str, object]", report["counts"])[
                "canonical_records"
            ],
            "raw_records": cast("Mapping[str, object]", report["counts"])[
                "raw_records"
            ],
            "source_bytes": cast("Mapping[str, object]", report["storage"])[
                "source_bytes"
            ],
        },
        "pilot_report_file_sha256": report_file_sha,
        "repository_verification_passed": True,
        "schema_version": SCHEMA_VERSION,
        "source_policy_sha256": report["source_policy_sha256"],
        "universe_snapshot_sha256": report["universe_snapshot_sha256"],
        "verification": verification,
    }
    _write_json(acceptance_path, acceptance)
    return _load_hashed_json(acceptance_path, "pilot acceptance")[0]


def _dry_run(command: str, data_root: Path) -> dict[str, object]:
    snapshot = load_ticker_universe(UnresolvedInstrumentResolver())
    return {
        "command": command,
        "data_root": str(data_root),
        "dry_run": True,
        "execute_required": True,
        "network_access_performed": False,
        "requested_symbol_count": len(snapshot.ordered_entries),
        "ticker_source_sha256": snapshot.source_file_sha256.hex(),
    }


def _execution_summary(
    report: Mapping[str, object], *, data_root: Path
) -> dict[str, object]:
    """Return bounded console output while the immutable report keeps full evidence."""
    assets = cast("Mapping[str, object]", report["assets"])
    unsupported = sorted(
        symbol
        for symbol, raw_state in assets.items()
        if not isinstance(raw_state, dict) or raw_state.get("status") != "RESOLVED"
    )
    mode = RunMode(str(report["mode"]))
    return {
        "counts": report["counts"],
        "date_range": {"end": report["end_date"], "start": report["start_date"]},
        "duration_seconds": report["duration_seconds"],
        "latest_complete_market_date": report["latest_complete_market_date"],
        "live_trading_capable": False,
        "mode": mode.value,
        "provider_request_count": report["provider_request_count"],
        "provider_retry_count": report["provider_retry_count"],
        "report_path": str(_report_path(data_root, mode)),
        "run_id": report["run_id"],
        "status": report["status"],
        "storage": report["storage"],
        "unsupported_symbols": unsupported,
    }


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="aegis-alpaca-data",
        description="Bounded GET-only Alpaca Basic/IEX minute-data POC",
    )
    parser.add_argument("--data-root", type=Path, default=DEFAULT_DATA_ROOT)
    commands = parser.add_subparsers(dest="command", required=True)

    authorize = commands.add_parser("authorize-academic-scope")
    authorize.add_argument("--operator-id", required=True)
    authorize.add_argument("--expires-at-utc", required=True)
    authorize.add_argument(
        "--approval-output", type=Path, default=DEFAULT_APPROVAL_PATH
    )
    authorize.add_argument("--policy-output", type=Path, default=DEFAULT_POLICY_PATH)
    authorize.add_argument(
        "--accept-personal-noncommercial-terms", action="store_true", required=True
    )

    for name in ("pilot", "backfill"):
        command = commands.add_parser(name)
        command.add_argument("--execute", action="store_true")
        command.add_argument("--policy", type=Path, default=DEFAULT_POLICY_PATH)
        command.add_argument("--approval", type=Path, default=DEFAULT_APPROVAL_PATH)
        command.add_argument("--secrets-file", type=Path, default=DEFAULT_SECRETS_PATH)
        if name == "backfill":
            command.add_argument(
                "--pilot-acceptance", type=Path, default=DEFAULT_PILOT_ACCEPTANCE
            )

    verify = commands.add_parser("verify")
    verify.add_argument("--report", type=Path, required=True)
    verify.add_argument("--sample-seed", type=int, default=DEFAULT_SAMPLE_SEED)
    verify.add_argument("--sample-size", type=int, default=64)
    verify.add_argument("--accept-pilot", action="store_true")
    verify.add_argument("--verification-output", type=Path)
    verify.add_argument(
        "--acceptance-output", type=Path, default=DEFAULT_PILOT_ACCEPTANCE
    )
    return parser


def main(arguments: Sequence[str] | None = None) -> int:
    """Run the bounded source-specific CLI with structured failure output."""
    parsed = _parser().parse_args(arguments)
    try:
        if parsed.command == "authorize-academic-scope":
            expiry = _parse_utc_second(parsed.expires_at_utc, "expires_at_utc")
            approval_sha, policy_sha = prepare_academic_authorization(
                operator_id=parsed.operator_id,
                data_root=parsed.data_root,
                approval_path=parsed.approval_output,
                policy_path=parsed.policy_output,
                expires_at_utc=expiry,
                example_policy_path=(
                    Path(__file__).resolve().parents[3]
                    / "infra/data_poc/source-policy.example.json"
                ),
            )
            output: object = {
                "approval_record_sha256": approval_sha,
                "network_access_performed": False,
                "policy_file_sha256": policy_sha,
                "status": "AUTHORIZED",
            }
        elif parsed.command in {"pilot", "backfill"}:
            if not parsed.execute:
                output = _dry_run(parsed.command, parsed.data_root)
            else:
                report = execute_run(
                    mode=(
                        RunMode.PILOT if parsed.command == "pilot" else RunMode.BACKFILL
                    ),
                    data_root=parsed.data_root,
                    policy_path=parsed.policy,
                    approval_path=parsed.approval,
                    secrets_path=parsed.secrets_file,
                    pilot_acceptance_path=getattr(
                        parsed, "pilot_acceptance", DEFAULT_PILOT_ACCEPTANCE
                    ),
                )
                output = _execution_summary(report, data_root=parsed.data_root)
        elif parsed.accept_pilot:
            output = accept_pilot(
                parsed.report,
                data_root=parsed.data_root,
                acceptance_path=parsed.acceptance_output,
                sample_seed=parsed.sample_seed,
            )
        else:
            output = verify_run(
                parsed.report,
                data_root=parsed.data_root,
                sample_seed=parsed.sample_seed,
                sample_size=parsed.sample_size,
            )
            if parsed.verification_output is not None:
                _write_json(parsed.verification_output, output)
                output = _load_hashed_json(
                    parsed.verification_output, "verification report"
                )[0]
    except (AlpacaDataError, StorageError) as error:
        code = error.code.value if hasattr(error.code, "value") else str(error.code)
        sys.stdout.write(
            json.dumps(
                {"error": {"code": code, "message": str(error)}, "status": "REJECTED"},
                indent=2,
                sort_keys=True,
            )
            + "\n"
        )
        return 1
    sys.stdout.write(json.dumps(output, indent=2, sort_keys=True) + "\n")
    return 0


def cli_main() -> int:
    """Installed entry point."""
    signal.signal(signal.SIGINT, _termination_signal)
    signal.signal(signal.SIGTERM, _termination_signal)
    return main()


def _termination_signal(_signum: int, _frame: FrameType | None) -> Never:
    """Convert scheduler/operator termination into normal stack unwinding."""
    raise AlpacaDataError(
        AlpacaDataErrorCode.INTERRUPTED,
        "acquisition interrupted; the durable checkpoint is retained for resume",
    )


if __name__ == "__main__":  # pragma: no cover - exercised through cli_main
    raise SystemExit(cli_main())
