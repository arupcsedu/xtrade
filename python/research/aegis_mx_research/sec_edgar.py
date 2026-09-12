"""Bounded, point-in-time SEC EDGAR ingestion for the forecasting POC."""

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
import zlib
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import contextmanager
from dataclasses import dataclass, field
from datetime import UTC, date, datetime
from decimal import Decimal
from enum import StrEnum
from html.parser import HTMLParser
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, Final, Protocol, Self, cast, override
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener

from aegis_mx_intelligence.news_sandbox import (
    DocumentSanitizer,
    ProcessDocumentSanitizer,
)
from aegis_mx_intelligence.news_security import UnsafeDocumentError, sanitize_document
from aegis_mx_intelligence.news_types import (
    AuthenticationEvidence,
    SourceAuthentication,
    SourceDocument,
    sha256_bytes,
)

from aegis_mx_research.data_repository import (
    DECIMAL_GB,
    AdmissionLease,
    DataRepository,
    QuotaEvidence,
    StorageRequest,
)
from aegis_mx_research.forecast_contracts import (
    TICKER_UNIVERSE_PATH,
    UniverseSnapshot,
    UnresolvedInstrumentResolver,
    load_ticker_universe,
)

if TYPE_CHECKING:
    from collections.abc import Callable, Iterator, Mapping, Sequence

SEC_SCHEMA_VERSION: Final = "1.0.0"
SEC_REPORT_SCHEMA_VERSION: Final = "1.1.0"
SEC_SOURCE_REVISION: Final = "sec-edgar-public-api-reviewed-2026-09-11"
DEFAULT_DATA_ROOT: Final = Path("/scratch/djy8hg/aegis_mx_poc_data")
DEFAULT_REPORT_DIRECTORY: Final = DEFAULT_DATA_ROOT / "reports/sec-edgar/prompt-53"
DEFAULT_REQUEST_RATE_PER_SECOND: Final = 8
SEC_PUBLISHED_MAX_REQUEST_RATE: Final = 10
MAX_CONCURRENCY: Final = 4
MAX_RETRY_ATTEMPTS: Final = 4
MAX_RETRY_DELAY_SECONDS: Final = 30
MAX_COMPRESSED_BYTES: Final = 32 * 1024 * 1024
MAX_DECOMPRESSED_BYTES: Final = 64 * 1024 * 1024
MAX_DECOMPRESSION_RATIO: Final = 100
MAX_JSON_DEPTH: Final = 32
MAX_JSON_NODES: Final = 2_000_000
MAX_FILINGS_PER_ISSUER: Final = 20_000
MAX_FACTS_PER_ISSUER: Final = 1_000_000
MAX_DOCUMENT_BYTES: Final = 1 * 1024 * 1024
MAX_DOCUMENT_CHARACTERS: Final = 1_000_000
MAX_DOCUMENT_NODES: Final = 250_000
MAX_DOCUMENT_DEPTH: Final = 128
MAX_SANITIZED_CHARACTERS: Final = 240_000
MAX_EVIDENCE_EXCERPTS: Final = 32
MAX_EVIDENCE_CHARACTERS: Final = 4_096
MAX_PARSER_SECONDS: Final = 2.0
MAX_SEC_STORAGE_BYTES: Final = 8 * DECIMAL_GB
MAX_TEXT_BYTES: Final = 512
MAX_USER_AGENT_BYTES: Final = 256
MAX_APPROVAL_BYTES: Final = 32_768
MAX_URL_BYTES: Final = 1_024
MIN_HTTP_STATUS: Final = 100
MAX_HTTP_STATUS: Final = 599
HTTP_OK: Final = 200
HTTP_TOO_MANY_REQUESTS: Final = 429
HTTP_SERVER_ERROR_MIN: Final = 500
MAX_CIK_VALUE: Final = 9_999_999_999
MAX_HISTORICAL_SHARDS: Final = 1_000
MAX_DECIMAL_PRECISION: Final = 128
MANIFEST_BUDGET_BYTES: Final = 16_384
NANOSECONDS_PER_SECOND: Final = 1_000_000_000
_SEC_HOSTS: Final = frozenset({"data.sec.gov", "www.sec.gov"})
_ALLOWED_FORMS: Final = frozenset({"8-K", "10-Q", "10-K", "6-K", "20-F"})
_ACCESSION = re.compile(r"^[0-9]{10}-[0-9]{2}-[0-9]{6}$")
_CIK = re.compile(r"^[0-9]{10}$")
_SYMBOL = re.compile(r"^[A-Z][A-Z0-9]*(?:[.-][A-Z0-9]+)?$")
_SAFE_DOCUMENT = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,254}$")
_CONTACT = re.compile(r"(?:[^\s@]+@[^\s@]+\.[^\s@]+|https://[^\s]+)")
_WHITESPACE = re.compile(r"\s+")
_ITEM_HEADING = re.compile(
    r"(?i)\bitem\s+(?:1\.01|1\.05|2\.02|5\.02|7\.01|8\.01|9\.01)\b"
)


class SecErrorCode(StrEnum):
    """Stable, fail-closed SEC ingestion outcomes."""

    INVALID_CONFIGURATION = "INVALID_CONFIGURATION"
    UNAUTHORIZED = "UNAUTHORIZED"
    INVALID_URL = "INVALID_URL"
    RATE_LIMITED = "RATE_LIMITED"
    RETRY_EXHAUSTED = "RETRY_EXHAUSTED"
    REMOTE_FAILURE = "REMOTE_FAILURE"
    RESPONSE_TOO_LARGE = "RESPONSE_TOO_LARGE"
    DECOMPRESSION_LIMIT = "DECOMPRESSION_LIMIT"
    MALFORMED_RESPONSE = "MALFORMED_RESPONSE"
    TIMESTAMP_DISORDER = "TIMESTAMP_DISORDER"
    AMBIGUOUS_ISSUER = "AMBIGUOUS_ISSUER"
    UNSUPPORTED_ISSUER = "UNSUPPORTED_ISSUER"
    PARSER_LIMIT = "PARSER_LIMIT"
    PROMPT_INJECTION = "PROMPT_INJECTION"
    STORAGE_LIMIT = "STORAGE_LIMIT"
    SHUTDOWN = "SHUTDOWN"


class SecDataset(StrEnum):
    """Exact SEC content classes with independent authorization."""

    COMPANY_TICKERS = "COMPANY_TICKERS"
    SUBMISSIONS = "SUBMISSIONS"
    COMPANY_FACTS = "COMPANY_FACTS"
    PRIMARY_FILING_DOCUMENT = "PRIMARY_FILING_DOCUMENT"


class IssuerResolutionStatus(StrEnum):
    """Ticker-to-CIK resolution outcome."""

    RESOLVED_CURRENT_ONLY = "RESOLVED_CURRENT_ONLY"
    UNRESOLVED = "UNRESOLVED"
    AMBIGUOUS = "AMBIGUOUS"


class LineageStatus(StrEnum):
    """Whether an amendment parent is known from explicit evidence."""

    NOT_AMENDMENT = "NOT_AMENDMENT"
    EXPLICIT_PARENT = "EXPLICIT_PARENT"
    UNRESOLVED_SOURCE_HAS_NO_PARENT = "UNRESOLVED_SOURCE_HAS_NO_PARENT"


class SecEdgarError(RuntimeError):
    """SEC boundary failure with a machine-readable reason."""

    def __init__(self, code: SecErrorCode, message: str) -> None:
        """Retain a bounded non-secret diagnostic."""
        self.code = code
        normalized = _WHITESPACE.sub(" ", str(message)).strip()[:MAX_TEXT_BYTES]
        super().__init__(f"{code.value}: {normalized}")


def _canonical_bytes(value: object) -> bytes:
    return json.dumps(
        value, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode("ascii")


def _sha256(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _positive_ns(value: int, field_name: str) -> None:
    if isinstance(value, bool) or not 0 < value < (1 << 63):
        raise SecEdgarError(
            SecErrorCode.INVALID_CONFIGURATION,
            f"{field_name} must be a positive signed 64-bit nanosecond timestamp",
        )


def _parse_utc_ns(value: str, field_name: str) -> int:
    if not isinstance(value, str) or not value:
        raise SecEdgarError(SecErrorCode.MALFORMED_RESPONSE, f"{field_name} is missing")
    normalized = f"{value[:-1]}+00:00" if value.endswith("Z") else value
    try:
        parsed = datetime.fromisoformat(normalized)
    except ValueError as error:
        raise SecEdgarError(
            SecErrorCode.MALFORMED_RESPONSE,
            f"{field_name} is not an ISO-8601 timestamp",
        ) from error
    if parsed.tzinfo is None or parsed.utcoffset() != UTC.utcoffset(parsed):
        raise SecEdgarError(
            SecErrorCode.MALFORMED_RESPONSE,
            f"{field_name} must contain an explicit UTC offset",
        )
    epoch = datetime(1970, 1, 1, tzinfo=UTC)
    delta = parsed - epoch
    result = (
        delta.days * 86_400 * NANOSECONDS_PER_SECOND
        + delta.seconds * NANOSECONDS_PER_SECOND
        + delta.microseconds * 1_000
    )
    _positive_ns(result, field_name)
    return result


def _parse_date(value: str, field_name: str) -> date | None:
    if value == "":
        return None
    try:
        return date.fromisoformat(value)
    except (TypeError, ValueError) as error:
        raise SecEdgarError(
            SecErrorCode.MALFORMED_RESPONSE,
            f"{field_name} is not an ISO-8601 date",
        ) from error


@dataclass(frozen=True, slots=True)
class SecAuthorization:
    """Immutable content-class authorization loaded outside the worktree."""

    approval_id: str
    approval_sha256: str
    allowed_datasets: frozenset[SecDataset]
    allowed_forms: frozenset[str]
    distribution: str
    expires_at_utc_ns: int
    model_training_on_documents: bool = False

    def __post_init__(self) -> None:
        """Require the narrow, non-redistributed POC boundary."""
        if (
            not self.approval_id
            or not re.fullmatch(r"[A-Za-z0-9._-]{3,128}", self.approval_id)
            or not re.fullmatch(r"[0-9a-f]{64}", self.approval_sha256)
            or not self.allowed_datasets
            or not self.allowed_forms
            or not self.allowed_forms <= _ALLOWED_FORMS
            or self.distribution != "OWNER_ONLY_INTERNAL"
            or self.model_training_on_documents
        ):
            raise SecEdgarError(
                SecErrorCode.UNAUTHORIZED,
                "SEC authorization exceeds the bounded internal POC policy",
            )
        _positive_ns(self.expires_at_utc_ns, "authorization expiry")

    def permits(self, dataset: SecDataset, now_utc_ns: int) -> bool:
        """Return whether a content class remains authorized now."""
        _positive_ns(now_utc_ns, "authorization evaluation time")
        return dataset in self.allowed_datasets and now_utc_ns <= self.expires_at_utc_ns

    @classmethod
    def load(cls, path: Path) -> Self:
        """Load and self-hash a private, non-symlink approval record."""
        try:
            status = os.lstat(path)
            if (
                not stat.S_ISREG(status.st_mode)
                or stat.S_IMODE(status.st_mode) & 0o077
                or status.st_size > MAX_APPROVAL_BYTES
            ):
                raise SecEdgarError(
                    SecErrorCode.UNAUTHORIZED,
                    "approval record must be a bounded owner-only regular file",
                )
            descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
            try:
                payload = os.read(descriptor, status.st_size + 1)
            finally:
                os.close(descriptor)
            value = json.loads(payload)
        except SecEdgarError:
            raise
        except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
            raise SecEdgarError(
                SecErrorCode.UNAUTHORIZED, "approval record cannot be verified"
            ) from error
        if not isinstance(value, dict):
            raise SecEdgarError(SecErrorCode.UNAUTHORIZED, "approval must be an object")
        supplied = value.get("approval_sha256")
        body = dict(value)
        body.pop("approval_sha256", None)
        actual = _sha256(_canonical_bytes(body))
        if supplied != actual:
            raise SecEdgarError(
                SecErrorCode.UNAUTHORIZED, "approval record hash does not match"
            )
        try:
            return cls(
                approval_id=cast("str", value["approval_id"]),
                approval_sha256=actual,
                allowed_datasets=frozenset(
                    SecDataset(item)
                    for item in cast("list[str]", value["allowed_datasets"])
                ),
                allowed_forms=frozenset(cast("list[str]", value["allowed_forms"])),
                distribution=cast("str", value["distribution"]),
                expires_at_utc_ns=_parse_utc_ns(
                    cast("str", value["expires_at_utc"]), "expires_at_utc"
                ),
                model_training_on_documents=cast(
                    "bool", value["model_training_on_documents"]
                ),
            )
        except (KeyError, TypeError, ValueError) as error:
            raise SecEdgarError(
                SecErrorCode.UNAUTHORIZED, "approval fields are invalid"
            ) from error


@dataclass(frozen=True, slots=True)
class SecConfig:
    """Bounded SEC network, parser, and storage policy."""

    user_agent: str
    requests_per_second: int = DEFAULT_REQUEST_RATE_PER_SECOND
    concurrency: int = 2
    retry_attempts: int = 3
    parser_timeout_seconds: float = MAX_PARSER_SECONDS
    storage_limit_bytes: int = MAX_SEC_STORAGE_BYTES
    download_primary_documents: bool = False
    filing_start_date: date | None = None
    filing_end_date: date | None = None

    def __post_init__(self) -> None:
        """Reject anonymous automation and values above SEC/project limits."""
        encoded = self.user_agent.encode("utf-8")
        if (
            not self.user_agent
            or len(encoded) > MAX_USER_AGENT_BYTES
            or "\r" in self.user_agent
            or "\n" in self.user_agent
            or _CONTACT.search(self.user_agent) is None
            or not 1 <= self.requests_per_second <= SEC_PUBLISHED_MAX_REQUEST_RATE
            or not 1 <= self.concurrency <= MAX_CONCURRENCY
            or not 1 <= self.retry_attempts <= MAX_RETRY_ATTEMPTS
            or not 0 < self.parser_timeout_seconds <= MAX_PARSER_SECONDS
            or not 0 < self.storage_limit_bytes <= MAX_SEC_STORAGE_BYTES
            or (self.filing_start_date is None) != (self.filing_end_date is None)
            or (
                self.filing_start_date is not None
                and self.filing_end_date is not None
                and self.filing_start_date > self.filing_end_date
            )
        ):
            raise SecEdgarError(
                SecErrorCode.INVALID_CONFIGURATION,
                "SEC configuration is anonymous, malformed, or outside bounds",
            )


@dataclass(frozen=True, slots=True)
class SecHttpResponse:
    """Bounded HTTP response supplied by production or mock transports."""

    status: int
    headers: Mapping[str, str]
    body: bytes
    received_at_utc_ns: int

    def __post_init__(self) -> None:
        """Validate response metadata before parsing."""
        if (
            not MIN_HTTP_STATUS <= self.status <= MAX_HTTP_STATUS
            or len(self.body) > MAX_COMPRESSED_BYTES
        ):
            raise SecEdgarError(
                SecErrorCode.RESPONSE_TOO_LARGE,
                "HTTP response status or compressed byte count is invalid",
            )
        _positive_ns(self.received_at_utc_ns, "HTTP receipt time")


class SecTransport(Protocol):
    """Injectable read-only transport; tests never need remote access."""

    def get(
        self, url: str, headers: Mapping[str, str], timeout_seconds: float
    ) -> SecHttpResponse:
        """Return one complete, bounded GET response."""


class _NoRedirect(HTTPRedirectHandler):
    @override
    def redirect_request(
        self,
        req: Request,
        fp: object,
        code: int,
        msg: str,
        headers: object,
        newurl: str,
    ) -> Request | None:
        del req, fp, code, msg, headers, newurl
        return None


class UrllibSecTransport:
    """Production read-only HTTPS transport with redirects disabled."""

    def __init__(self, *, wall_clock_ns: Callable[[], int] = time.time_ns) -> None:
        """Create a redirect-denying transport with an injectable wall clock."""
        self._wall_clock_ns = wall_clock_ns
        self._opener = build_opener(_NoRedirect())

    def get(
        self, url: str, headers: Mapping[str, str], timeout_seconds: float
    ) -> SecHttpResponse:
        """Issue a fixed-host GET and cap the body before returning it."""
        _validate_sec_url(url)
        request = Request(url, headers=dict(headers), method="GET")  # noqa: S310
        try:
            with self._opener.open(request, timeout=timeout_seconds) as response:
                body = response.read(MAX_COMPRESSED_BYTES + 1)
                if len(body) > MAX_COMPRESSED_BYTES:
                    raise SecEdgarError(
                        SecErrorCode.RESPONSE_TOO_LARGE,
                        "SEC response exceeds the compressed input limit",
                    )
                response_headers = {
                    key.casefold(): value for key, value in response.headers.items()
                }
                return SecHttpResponse(
                    status=response.status,
                    headers=response_headers,
                    body=body,
                    received_at_utc_ns=self._wall_clock_ns(),
                )
        except HTTPError as error:
            body = error.read(MAX_COMPRESSED_BYTES + 1)
            return SecHttpResponse(
                status=error.code,
                headers={key.casefold(): value for key, value in error.headers.items()},
                body=body[:MAX_COMPRESSED_BYTES],
                received_at_utc_ns=self._wall_clock_ns(),
            )
        except (TimeoutError, URLError, OSError) as error:
            raise SecEdgarError(
                SecErrorCode.REMOTE_FAILURE, "SEC HTTPS request failed"
            ) from error


@dataclass(slots=True)
class MockSecServer:
    """Socket-free deterministic mock server with scripted HTTP responses."""

    responses: dict[str, list[SecHttpResponse]]
    requests: list[tuple[str, Mapping[str, str]]] = field(default_factory=list)

    def get(
        self, url: str, headers: Mapping[str, str], timeout_seconds: float
    ) -> SecHttpResponse:
        """Return a scripted response and retain redaction-safe request evidence."""
        del timeout_seconds
        self.requests.append((url, dict(headers)))
        scripted = self.responses.get(url)
        if not scripted:
            raise SecEdgarError(
                SecErrorCode.REMOTE_FAILURE, "mock route is unavailable"
            )
        return scripted.pop(0)


def _validate_sec_url(url: str) -> None:
    if len(url.encode("utf-8")) > MAX_URL_BYTES:
        raise SecEdgarError(SecErrorCode.INVALID_URL, "SEC URL exceeds its bound")
    split = urlsplit(url)
    if (
        split.scheme != "https"
        or split.hostname not in _SEC_HOSTS
        or split.port is not None
        or split.username is not None
        or split.password is not None
        or split.query
        or split.fragment
        or not split.path.startswith("/")
        or "//" in split.path
        or any(part in {".", ".."} for part in PurePosixPath(split.path).parts)
    ):
        raise SecEdgarError(
            SecErrorCode.INVALID_URL, "only exact SEC HTTPS hosts and paths are allowed"
        )


class _RateLimiter:
    def __init__(
        self,
        requests_per_second: int,
        *,
        monotonic_ns: Callable[[], int],
        sleep: Callable[[float], None],
    ) -> None:
        self._interval_ns = NANOSECONDS_PER_SECOND // requests_per_second
        self._monotonic_ns = monotonic_ns
        self._sleep = sleep
        self._next_ns = 0
        self._lock = threading.Lock()

    def acquire(self) -> None:
        with self._lock:
            now = self._monotonic_ns()
            wait_ns = max(self._next_ns - now, 0)
            if wait_ns:
                self._sleep(wait_ns / NANOSECONDS_PER_SECOND)
                now = self._monotonic_ns()
            self._next_ns = max(now, self._next_ns) + self._interval_ns


def _decode_body(response: SecHttpResponse) -> bytes:
    encoding = response.headers.get("content-encoding", "identity").casefold().strip()
    content_length = response.headers.get("content-length")
    if content_length is not None:
        try:
            declared = int(content_length)
        except ValueError as error:
            raise SecEdgarError(
                SecErrorCode.MALFORMED_RESPONSE, "Content-Length is malformed"
            ) from error
        if (
            declared < 0
            or declared > MAX_COMPRESSED_BYTES
            or declared != len(response.body)
        ):
            raise SecEdgarError(
                SecErrorCode.RESPONSE_TOO_LARGE,
                "Content-Length exceeds bounds or does not match the body",
            )
    try:
        if encoding == "identity" or not encoding:
            decoded = response.body
        elif encoding == "gzip":
            decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
            decoded = decoder.decompress(response.body, MAX_DECOMPRESSED_BYTES + 1)
            decoded += decoder.flush(MAX_DECOMPRESSED_BYTES + 1 - len(decoded))
            if not decoder.eof or decoder.unused_data:
                raise SecEdgarError(
                    SecErrorCode.MALFORMED_RESPONSE,
                    "gzip response is incomplete or concatenated",
                )
        elif encoding == "deflate":
            decoder = zlib.decompressobj()
            decoded = decoder.decompress(response.body, MAX_DECOMPRESSED_BYTES + 1)
            decoded += decoder.flush(MAX_DECOMPRESSED_BYTES + 1 - len(decoded))
            if not decoder.eof or decoder.unused_data:
                raise SecEdgarError(
                    SecErrorCode.MALFORMED_RESPONSE,
                    "deflate response is incomplete or concatenated",
                )
        else:
            raise SecEdgarError(
                SecErrorCode.MALFORMED_RESPONSE, "unsupported Content-Encoding"
            )
    except zlib.error as error:
        raise SecEdgarError(
            SecErrorCode.MALFORMED_RESPONSE, "compressed response is malformed"
        ) from error
    if len(decoded) > MAX_DECOMPRESSED_BYTES or (
        response.body and len(decoded) > len(response.body) * MAX_DECOMPRESSION_RATIO
    ):
        raise SecEdgarError(
            SecErrorCode.DECOMPRESSION_LIMIT,
            "response exceeds decompression size or ratio limits",
        )
    return decoded


class SecClient:
    """Rate-limited, retry-bounded SEC client with no write methods."""

    def __init__(
        self,
        config: SecConfig,
        transport: SecTransport,
        *,
        monotonic_ns: Callable[[], int] = time.monotonic_ns,
        sleep: Callable[[float], None] = time.sleep,
        shutdown_requested: Callable[[], bool] = lambda: False,
    ) -> None:
        """Create a bounded client using injected clock, sleep, and shutdown hooks."""
        self.config = config
        self._transport = transport
        self._limiter = _RateLimiter(
            config.requests_per_second, monotonic_ns=monotonic_ns, sleep=sleep
        )
        self._sleep = sleep
        self._shutdown_requested = shutdown_requested

    def get(self, url: str) -> SecHttpResponse:
        """Fetch one object, honoring 429/5xx retry bounds and shutdown."""
        _validate_sec_url(url)
        last_status = 0
        for attempt in range(self.config.retry_attempts):
            if self._shutdown_requested():
                raise SecEdgarError(SecErrorCode.SHUTDOWN, "shutdown requested")
            self._limiter.acquire()
            response = self._transport.get(
                url,
                {
                    "Accept": "application/json,text/html;q=0.9",
                    "Accept-Encoding": "gzip, deflate",
                    "User-Agent": self.config.user_agent,
                },
                30.0,
            )
            last_status = response.status
            if response.status == HTTP_OK:
                return SecHttpResponse(
                    status=response.status,
                    headers=response.headers,
                    body=_decode_body(response),
                    received_at_utc_ns=response.received_at_utc_ns,
                )
            retryable = (
                response.status == HTTP_TOO_MANY_REQUESTS
                or HTTP_SERVER_ERROR_MIN <= response.status <= MAX_HTTP_STATUS
            )
            if not retryable:
                raise SecEdgarError(
                    SecErrorCode.REMOTE_FAILURE,
                    f"SEC returned non-retryable HTTP status {response.status}",
                )
            if attempt + 1 < self.config.retry_attempts:
                raw_retry = response.headers.get("retry-after", "")
                try:
                    retry_seconds = int(raw_retry) if raw_retry else 2**attempt
                except ValueError:
                    retry_seconds = 2**attempt
                self._sleep(float(min(max(retry_seconds, 0), MAX_RETRY_DELAY_SECONDS)))
        code = (
            SecErrorCode.RATE_LIMITED
            if last_status == HTTP_TOO_MANY_REQUESTS
            else SecErrorCode.RETRY_EXHAUSTED
        )
        raise SecEdgarError(
            code, f"SEC retry budget exhausted after HTTP {last_status}"
        )


def company_tickers_url() -> str:
    """Return the current SEC ticker/CIK association file."""
    return "https://www.sec.gov/files/company_tickers.json"


def submissions_url(cik: str) -> str:
    """Return the exact current submissions endpoint for a 10-digit CIK."""
    if _CIK.fullmatch(cik) is None:
        raise SecEdgarError(
            SecErrorCode.INVALID_URL, "CIK must contain exactly 10 digits"
        )
    return f"https://data.sec.gov/submissions/CIK{cik}.json"


def historical_submissions_url(filename: str) -> str:
    """Return a bounded SEC-named historical submissions shard URL."""
    if re.fullmatch(r"CIK[0-9]{10}-submissions-[0-9]{3}\.json", filename) is None:
        raise SecEdgarError(
            SecErrorCode.INVALID_URL, "historical submissions filename is invalid"
        )
    return f"https://data.sec.gov/submissions/{filename}"


def company_facts_url(cik: str) -> str:
    """Return the exact company-facts endpoint for a 10-digit CIK."""
    if _CIK.fullmatch(cik) is None:
        raise SecEdgarError(
            SecErrorCode.INVALID_URL, "CIK must contain exactly 10 digits"
        )
    return f"https://data.sec.gov/api/xbrl/companyfacts/CIK{cik}.json"


def primary_document_url(cik: str, accession: str, document: str) -> str:
    """Build a fixed-host primary-document URL; arbitrary attachments are excluded."""
    if (
        _CIK.fullmatch(cik) is None
        or _ACCESSION.fullmatch(accession) is None
        or _SAFE_DOCUMENT.fullmatch(document) is None
        or document.casefold().endswith((".exe", ".zip", ".js", ".svg", ".pdf"))
    ):
        raise SecEdgarError(
            SecErrorCode.INVALID_URL, "primary filing document identity is invalid"
        )
    return (
        "https://www.sec.gov/Archives/edgar/data/"
        f"{int(cik)}/{accession.replace('-', '')}/{document}"
    )


def _validate_json_tree(value: object) -> None:
    nodes = 0
    stack: list[tuple[object, int]] = [(value, 1)]
    while stack:
        current, depth = stack.pop()
        nodes += 1
        if nodes > MAX_JSON_NODES or depth > MAX_JSON_DEPTH:
            raise SecEdgarError(
                SecErrorCode.PARSER_LIMIT, "JSON exceeds node or nesting limits"
            )
        if isinstance(current, dict):
            stack.extend((item, depth + 1) for item in current.values())
        elif isinstance(current, list):
            stack.extend((item, depth + 1) for item in current)


def parse_json(payload: bytes) -> object:
    """Parse exact numeric JSON under byte, depth, and node limits."""
    if not payload or len(payload) > MAX_DECOMPRESSED_BYTES:
        raise SecEdgarError(
            SecErrorCode.RESPONSE_TOO_LARGE, "JSON payload is empty or oversized"
        )
    try:
        value = json.loads(payload, parse_float=Decimal, parse_int=int)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise SecEdgarError(
            SecErrorCode.MALFORMED_RESPONSE, "SEC response is not valid JSON"
        ) from error
    _validate_json_tree(value)
    return value


@dataclass(frozen=True, slots=True)
class IssuerResolution:
    """Current SEC association, never represented as historical truth."""

    ticker: str
    status: IssuerResolutionStatus
    cik: str | None
    issuer_name: str | None
    reason: str
    source_sha256: str
    received_at_utc_ns: int


def parse_company_tickers(
    payload: bytes, universe: UniverseSnapshot, *, received_at_utc_ns: int
) -> tuple[IssuerResolution, ...]:
    """Resolve the universe against SEC's explicitly current association file."""
    _positive_ns(received_at_utc_ns, "ticker-map receipt time")
    value = parse_json(payload)
    if not isinstance(value, dict):
        raise SecEdgarError(
            SecErrorCode.MALFORMED_RESPONSE, "company ticker map must be an object"
        )
    requested = {entry.symbol for entry in universe.ordered_entries}
    matches: dict[str, list[tuple[str, str]]] = {symbol: [] for symbol in requested}
    for record in value.values():
        if not isinstance(record, dict):
            raise SecEdgarError(
                SecErrorCode.MALFORMED_RESPONSE, "ticker-map row must be an object"
            )
        ticker = record.get("ticker")
        cik_value = record.get("cik_str")
        title = record.get("title")
        if not isinstance(ticker, str) or ticker not in requested:
            continue
        if (
            _SYMBOL.fullmatch(ticker) is None
            or isinstance(cik_value, bool)
            or not isinstance(cik_value, int)
            or not 0 < cik_value <= MAX_CIK_VALUE
            or not isinstance(title, str)
            or not title
            or len(title.encode("utf-8")) > MAX_TEXT_BYTES
        ):
            raise SecEdgarError(
                SecErrorCode.MALFORMED_RESPONSE, "ticker-map row fields are invalid"
            )
        matches[ticker].append((f"{cik_value:010d}", title))
    digest = _sha256(payload)
    resolutions: list[IssuerResolution] = []
    for entry in universe.ordered_entries:
        options = sorted(set(matches[entry.symbol]))
        if not options:
            resolutions.append(
                IssuerResolution(
                    ticker=entry.symbol,
                    status=IssuerResolutionStatus.UNRESOLVED,
                    cik=None,
                    issuer_name=None,
                    reason="NO_CURRENT_SEC_TICKER_ASSOCIATION",
                    source_sha256=digest,
                    received_at_utc_ns=received_at_utc_ns,
                )
            )
        elif len(options) > 1:
            resolutions.append(
                IssuerResolution(
                    ticker=entry.symbol,
                    status=IssuerResolutionStatus.AMBIGUOUS,
                    cik=None,
                    issuer_name=None,
                    reason="MULTIPLE_CURRENT_SEC_ASSOCIATIONS",
                    source_sha256=digest,
                    received_at_utc_ns=received_at_utc_ns,
                )
            )
        else:
            cik, name = options[0]
            resolutions.append(
                IssuerResolution(
                    ticker=entry.symbol,
                    status=IssuerResolutionStatus.RESOLVED_CURRENT_ONLY,
                    cik=cik,
                    issuer_name=name,
                    reason="SEC_CURRENT_MAPPING_NOT_HISTORICAL_TRUTH",
                    source_sha256=digest,
                    received_at_utc_ns=received_at_utc_ns,
                )
            )
    return tuple(resolutions)


@dataclass(frozen=True, slots=True)
class FilingMetadata:
    """Immutable filing metadata with explicit point-in-time provenance."""

    cik: str
    accession_number: str
    form: str
    base_form: str
    is_amendment: bool
    filing_date: date
    report_date: date | None
    acceptance_time_utc_ns: int
    publication_time_utc_ns: int | None
    publication_time_reason: str
    receipt_time_utc_ns: int
    processing_time_utc_ns: int
    primary_document: str
    source_sha256: str
    amendment_parent_accession: str | None
    lineage_status: LineageStatus


def _column(value: Mapping[str, object], name: str, count: int) -> list[object]:
    column = value.get(name)
    if not isinstance(column, list) or len(column) != count:
        raise SecEdgarError(
            SecErrorCode.MALFORMED_RESPONSE,
            f"submissions column {name} is missing or has inconsistent length",
        )
    return column


def parse_submissions(
    payload: bytes,
    *,
    expected_cik: str,
    received_at_utc_ns: int,
    processing_time_utc_ns: int,
    allowed_forms: frozenset[str] = _ALLOWED_FORMS,
    filing_start_date: date | None = None,
    filing_end_date: date | None = None,
) -> tuple[FilingMetadata, ...]:
    """Parse current or historical columnar submission metadata."""
    _positive_ns(received_at_utc_ns, "submissions receipt time")
    _positive_ns(processing_time_utc_ns, "submissions processing time")
    if processing_time_utc_ns < received_at_utc_ns:
        raise SecEdgarError(
            SecErrorCode.TIMESTAMP_DISORDER, "processing precedes receipt"
        )
    if (filing_start_date is None) != (filing_end_date is None) or (
        filing_start_date is not None
        and filing_end_date is not None
        and filing_start_date > filing_end_date
    ):
        raise SecEdgarError(
            SecErrorCode.INVALID_CONFIGURATION,
            "filing date window is incomplete or disordered",
        )
    value = parse_json(payload)
    if not isinstance(value, dict):
        raise SecEdgarError(
            SecErrorCode.MALFORMED_RESPONSE, "submissions payload must be an object"
        )
    recent: object
    if "filings" in value:
        filings = value.get("filings")
        if not isinstance(filings, dict):
            raise SecEdgarError(
                SecErrorCode.MALFORMED_RESPONSE, "filings container is invalid"
            )
        recent = filings.get("recent")
        cik_value = value.get("cik")
        if cik_value not in {expected_cik, str(int(expected_cik))}:
            raise SecEdgarError(
                SecErrorCode.MALFORMED_RESPONSE,
                "submissions CIK does not match request",
            )
    else:
        recent = value
    if not isinstance(recent, dict):
        raise SecEdgarError(
            SecErrorCode.MALFORMED_RESPONSE, "recent submissions must be an object"
        )
    accession = recent.get("accessionNumber")
    if not isinstance(accession, list) or len(accession) > MAX_FILINGS_PER_ISSUER:
        raise SecEdgarError(
            SecErrorCode.PARSER_LIMIT, "filing count is missing or exceeds its bound"
        )
    count = len(accession)
    forms = _column(recent, "form", count)
    filing_dates = _column(recent, "filingDate", count)
    report_dates = _column(recent, "reportDate", count)
    acceptance_times = _column(recent, "acceptanceDateTime", count)
    primary_documents = _column(recent, "primaryDocument", count)
    digest = _sha256(payload)
    records: list[FilingMetadata] = []
    seen: set[str] = set()
    for index in range(count):
        accession_number = accession[index]
        raw_form = forms[index]
        if not isinstance(raw_form, str):
            raise SecEdgarError(
                SecErrorCode.MALFORMED_RESPONSE, "filing form must be text"
            )
        is_amendment = raw_form.endswith("/A")
        base_form = raw_form.removesuffix("/A")
        if base_form not in allowed_forms:
            continue
        primary_document = primary_documents[index]
        if (
            not isinstance(accession_number, str)
            or _ACCESSION.fullmatch(accession_number) is None
            or accession_number in seen
            or not isinstance(primary_document, str)
            or _SAFE_DOCUMENT.fullmatch(primary_document) is None
        ):
            raise SecEdgarError(
                SecErrorCode.MALFORMED_RESPONSE,
                "accession or primary-document identity is invalid or duplicated",
            )
        seen.add(accession_number)
        if not all(
            isinstance(item, str)
            for item in (
                filing_dates[index],
                report_dates[index],
                acceptance_times[index],
            )
        ):
            raise SecEdgarError(
                SecErrorCode.MALFORMED_RESPONSE, "filing timestamp fields must be text"
            )
        acceptance_ns = _parse_utc_ns(
            cast("str", acceptance_times[index]), "acceptanceDateTime"
        )
        if acceptance_ns > received_at_utc_ns:
            raise SecEdgarError(
                SecErrorCode.TIMESTAMP_DISORDER,
                "historical retrieval receipt precedes filing acceptance",
            )
        parsed_filing_date = _parse_date(cast("str", filing_dates[index]), "filingDate")
        if parsed_filing_date is None:
            raise SecEdgarError(
                SecErrorCode.MALFORMED_RESPONSE, "filingDate cannot be empty"
            )
        if (
            filing_start_date is not None
            and filing_end_date is not None
            and not filing_start_date <= parsed_filing_date <= filing_end_date
        ):
            continue
        records.append(
            FilingMetadata(
                cik=expected_cik,
                accession_number=accession_number,
                form=raw_form,
                base_form=base_form,
                is_amendment=is_amendment,
                filing_date=parsed_filing_date,
                report_date=_parse_date(cast("str", report_dates[index]), "reportDate"),
                acceptance_time_utc_ns=acceptance_ns,
                publication_time_utc_ns=None,
                publication_time_reason="NOT_EXPOSED_BY_SUBMISSIONS_API",
                receipt_time_utc_ns=received_at_utc_ns,
                processing_time_utc_ns=processing_time_utc_ns,
                primary_document=primary_document,
                source_sha256=digest,
                amendment_parent_accession=None,
                lineage_status=(
                    LineageStatus.UNRESOLVED_SOURCE_HAS_NO_PARENT
                    if is_amendment
                    else LineageStatus.NOT_AMENDMENT
                ),
            )
        )
    return tuple(records)


@dataclass(frozen=True, slots=True)
class HistoricalSubmissionShard:
    """One SEC-declared immutable historical submissions partition."""

    filename: str
    filing_count: int
    filing_from: date
    filing_to: date

    def overlaps(self, start: date, end: date) -> bool:
        """Return whether the source-declared interval intersects a window."""
        return self.filing_from <= end and self.filing_to >= start


def historical_submission_shards(
    payload: bytes, *, expected_cik: str | None = None
) -> tuple[HistoricalSubmissionShard, ...]:
    """Parse bounded SEC historical-shard metadata with declared date ranges."""
    value = parse_json(payload)
    if not isinstance(value, dict) or not isinstance(value.get("filings"), dict):
        raise SecEdgarError(
            SecErrorCode.MALFORMED_RESPONSE, "submissions files container is invalid"
        )
    files = cast("dict[str, object]", value["filings"]).get("files")
    if not isinstance(files, list) or len(files) > MAX_HISTORICAL_SHARDS:
        raise SecEdgarError(
            SecErrorCode.PARSER_LIMIT, "historical shard list is invalid or oversized"
        )
    if expected_cik is None:
        cik_value = value.get("cik")
        if isinstance(cik_value, str) and cik_value.isdigit():
            expected_cik = f"{int(cik_value):010d}"
    if expected_cik is not None and _CIK.fullmatch(expected_cik) is None:
        raise SecEdgarError(
            SecErrorCode.MALFORMED_RESPONSE, "historical shard CIK is invalid"
        )
    result: list[HistoricalSubmissionShard] = []
    for item in files:
        if not isinstance(item, dict) or not all(
            field in item for field in ("name", "filingCount", "filingFrom", "filingTo")
        ):
            raise SecEdgarError(
                SecErrorCode.MALFORMED_RESPONSE, "historical shard row is invalid"
            )
        name = item["name"]
        count = item["filingCount"]
        filing_from_value = item["filingFrom"]
        filing_to_value = item["filingTo"]
        if (
            not isinstance(name, str)
            or isinstance(count, bool)
            or not isinstance(count, int)
            or not 0 <= count <= MAX_FILINGS_PER_ISSUER
            or not isinstance(filing_from_value, str)
            or not isinstance(filing_to_value, str)
        ):
            raise SecEdgarError(
                SecErrorCode.MALFORMED_RESPONSE,
                "historical shard metadata types or bounds are invalid",
            )
        historical_submissions_url(name)
        if expected_cik is not None and not name.startswith(f"CIK{expected_cik}-"):
            raise SecEdgarError(
                SecErrorCode.MALFORMED_RESPONSE,
                "historical shard CIK does not match the issuer",
            )
        filing_from = _parse_date(filing_from_value, "historical filingFrom")
        filing_to = _parse_date(filing_to_value, "historical filingTo")
        if filing_from is None or filing_to is None or filing_from > filing_to:
            raise SecEdgarError(
                SecErrorCode.MALFORMED_RESPONSE,
                "historical shard date range is empty or disordered",
            )
        result.append(HistoricalSubmissionShard(name, count, filing_from, filing_to))
    if len({item.filename for item in result}) != len(result):
        raise SecEdgarError(
            SecErrorCode.MALFORMED_RESPONSE, "historical shard names are duplicated"
        )
    return tuple(result)


def historical_submission_files(payload: bytes) -> tuple[str, ...]:
    """Return exact filenames for compatibility with the original v1 API."""
    return tuple(item.filename for item in historical_submission_shards(payload))


@dataclass(frozen=True, slots=True)
class CompanyFact:
    """Exact decimal XBRL observation without binary floating-point conversion."""

    cik: str
    taxonomy: str
    concept: str
    unit: str
    value_coefficient: int
    value_scale: int
    start_date: date | None
    end_date: date
    filed_date: date
    form: str
    accession_number: str
    fiscal_year: int | None
    fiscal_period: str | None
    frame: str | None
    source_sha256: str
    receipt_time_utc_ns: int
    processing_time_utc_ns: int


def _decimal_parts(value: object) -> tuple[int, int]:
    if isinstance(value, bool) or not isinstance(value, (int, Decimal)):
        raise SecEdgarError(
            SecErrorCode.MALFORMED_RESPONSE, "XBRL fact value must be numeric"
        )
    decimal_value = Decimal(value)
    if not decimal_value.is_finite():
        raise SecEdgarError(
            SecErrorCode.MALFORMED_RESPONSE, "XBRL fact value must be finite"
        )
    sign, digits, exponent = decimal_value.as_tuple()
    if (
        not isinstance(exponent, int)
        or len(digits) > MAX_DECIMAL_PRECISION
        or not -MAX_DECIMAL_PRECISION <= exponent <= MAX_DECIMAL_PRECISION
    ):
        raise SecEdgarError(
            SecErrorCode.PARSER_LIMIT, "XBRL numeric precision exceeds its bound"
        )
    coefficient = int("".join(str(digit) for digit in digits) or "0")
    if sign:
        coefficient = -coefficient
    return coefficient, exponent


def parse_company_facts(
    payload: bytes,
    *,
    expected_cik: str,
    received_at_utc_ns: int,
    processing_time_utc_ns: int,
    allowed_forms: frozenset[str] = _ALLOWED_FORMS,
    filing_start_date: date | None = None,
    filing_end_date: date | None = None,
) -> tuple[CompanyFact, ...]:
    """Parse bounded entity-wide standard-taxonomy company facts."""
    _positive_ns(received_at_utc_ns, "company-facts receipt time")
    _positive_ns(processing_time_utc_ns, "company-facts processing time")
    if processing_time_utc_ns < received_at_utc_ns:
        raise SecEdgarError(
            SecErrorCode.TIMESTAMP_DISORDER, "processing precedes receipt"
        )
    if (filing_start_date is None) != (filing_end_date is None) or (
        filing_start_date is not None
        and filing_end_date is not None
        and filing_start_date > filing_end_date
    ):
        raise SecEdgarError(
            SecErrorCode.INVALID_CONFIGURATION,
            "fact filing-date window is incomplete or disordered",
        )
    value = parse_json(payload)
    if not isinstance(value, dict) or value.get("cik") not in {
        int(expected_cik),
        expected_cik,
    }:
        raise SecEdgarError(
            SecErrorCode.MALFORMED_RESPONSE, "company-facts CIK does not match request"
        )
    facts = value.get("facts")
    if not isinstance(facts, dict):
        raise SecEdgarError(
            SecErrorCode.MALFORMED_RESPONSE, "company-facts facts map is invalid"
        )
    digest = _sha256(payload)
    result: list[CompanyFact] = []
    for taxonomy, concepts in sorted(facts.items()):
        if taxonomy not in {"us-gaap", "ifrs-full", "dei", "srt"}:
            continue
        if not isinstance(concepts, dict):
            raise SecEdgarError(
                SecErrorCode.MALFORMED_RESPONSE, "XBRL taxonomy is invalid"
            )
        for concept, definition in sorted(concepts.items()):
            if not isinstance(concept, str) or not isinstance(definition, dict):
                raise SecEdgarError(
                    SecErrorCode.MALFORMED_RESPONSE, "XBRL concept is invalid"
                )
            units = definition.get("units")
            if not isinstance(units, dict):
                raise SecEdgarError(
                    SecErrorCode.MALFORMED_RESPONSE, "XBRL unit map is invalid"
                )
            for unit, observations in sorted(units.items()):
                if not isinstance(unit, str) or not isinstance(observations, list):
                    raise SecEdgarError(
                        SecErrorCode.MALFORMED_RESPONSE,
                        "XBRL observation list is invalid",
                    )
                for observation in observations:
                    if not isinstance(observation, dict):
                        raise SecEdgarError(
                            SecErrorCode.MALFORMED_RESPONSE,
                            "XBRL observation is invalid",
                        )
                    raw_form = observation.get("form")
                    if not isinstance(raw_form, str):
                        raise SecEdgarError(
                            SecErrorCode.MALFORMED_RESPONSE, "XBRL form is invalid"
                        )
                    base_form = raw_form.removesuffix("/A")
                    if base_form not in allowed_forms:
                        continue
                    accession = observation.get("accn")
                    if (
                        not isinstance(accession, str)
                        or _ACCESSION.fullmatch(accession) is None
                    ):
                        raise SecEdgarError(
                            SecErrorCode.MALFORMED_RESPONSE, "XBRL accession is invalid"
                        )
                    filed = observation.get("filed")
                    end = observation.get("end")
                    start = observation.get("start", "")
                    if not all(isinstance(item, str) for item in (filed, end, start)):
                        raise SecEdgarError(
                            SecErrorCode.MALFORMED_RESPONSE, "XBRL dates are invalid"
                        )
                    fy = observation.get("fy")
                    fp = observation.get("fp")
                    frame = observation.get("frame")
                    if fy is not None and (
                        isinstance(fy, bool) or not isinstance(fy, int)
                    ):
                        raise SecEdgarError(
                            SecErrorCode.MALFORMED_RESPONSE, "XBRL fy is invalid"
                        )
                    if fp is not None and not isinstance(fp, str):
                        raise SecEdgarError(
                            SecErrorCode.MALFORMED_RESPONSE, "XBRL fp is invalid"
                        )
                    if frame is not None and not isinstance(frame, str):
                        raise SecEdgarError(
                            SecErrorCode.MALFORMED_RESPONSE, "XBRL frame is invalid"
                        )
                    coefficient, scale = _decimal_parts(observation.get("val"))
                    parsed_end = _parse_date(cast("str", end), "fact end")
                    parsed_filed = _parse_date(cast("str", filed), "fact filed")
                    if parsed_end is None or parsed_filed is None:
                        raise SecEdgarError(
                            SecErrorCode.MALFORMED_RESPONSE,
                            "XBRL end and filed dates cannot be empty",
                        )
                    if (
                        filing_start_date is not None
                        and filing_end_date is not None
                        and not filing_start_date <= parsed_filed <= filing_end_date
                    ):
                        continue
                    result.append(
                        CompanyFact(
                            cik=expected_cik,
                            taxonomy=taxonomy,
                            concept=concept,
                            unit=unit,
                            value_coefficient=coefficient,
                            value_scale=scale,
                            start_date=_parse_date(cast("str", start), "fact start"),
                            end_date=parsed_end,
                            filed_date=parsed_filed,
                            form=raw_form,
                            accession_number=accession,
                            fiscal_year=fy,
                            fiscal_period=fp,
                            frame=frame,
                            source_sha256=digest,
                            receipt_time_utc_ns=received_at_utc_ns,
                            processing_time_utc_ns=processing_time_utc_ns,
                        )
                    )
                    if len(result) > MAX_FACTS_PER_ISSUER:
                        raise SecEdgarError(
                            SecErrorCode.PARSER_LIMIT,
                            "company facts exceed their bound",
                        )
    return tuple(result)


_BLOCKED_TAGS: Final = frozenset(
    {"script", "style", "iframe", "object", "embed", "form", "svg", "math", "template"}
)
_BREAK_TAGS: Final = frozenset(
    {"article", "br", "div", "h1", "h2", "h3", "li", "p", "section", "td", "tr"}
)


class _SecHtmlExtractor(HTMLParser):
    def __init__(self, *, deadline_ns: int, monotonic_ns: Callable[[], int]) -> None:
        super().__init__(convert_charrefs=True)
        self._deadline_ns = deadline_ns
        self._monotonic_ns = monotonic_ns
        self._blocked_depth = 0
        self._depth = 0
        self._nodes = 0
        self._characters = 0
        self._parts: list[str] = []

    def _check(self) -> None:
        self._nodes += 1
        if (
            self._nodes > MAX_DOCUMENT_NODES
            or self._depth > MAX_DOCUMENT_DEPTH
            or self._characters > MAX_DOCUMENT_CHARACTERS
            or self._monotonic_ns() > self._deadline_ns
        ):
            raise SecEdgarError(
                SecErrorCode.PARSER_LIMIT, "filing document exceeds parser limits"
            )

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        del attrs
        self._depth += 1
        normalized = tag.casefold()
        if normalized in _BLOCKED_TAGS:
            self._blocked_depth += 1
        elif self._blocked_depth == 0 and normalized in _BREAK_TAGS:
            self._parts.append("\n")
        self._check()

    def handle_startendtag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        del attrs
        if self._blocked_depth == 0 and tag.casefold() in _BREAK_TAGS:
            self._parts.append("\n")
        self._check()

    def handle_endtag(self, tag: str) -> None:
        normalized = tag.casefold()
        if normalized in _BLOCKED_TAGS and self._blocked_depth:
            self._blocked_depth -= 1
        elif self._blocked_depth == 0 and normalized in _BREAK_TAGS:
            self._parts.append("\n")
        self._depth = max(self._depth - 1, 0)
        self._check()

    def handle_data(self, data: str) -> None:
        self._characters += len(data)
        if self._blocked_depth == 0:
            self._parts.append(data)
        self._check()

    def text(self) -> str:
        return "".join(self._parts)


@dataclass(frozen=True, slots=True)
class EvidenceExcerpt:
    """Exact slice of inert sanitized text."""

    start: int
    end: int
    text: str
    sha256: str


@dataclass(frozen=True, slots=True)
class SanitizedFiling:
    """Bounded inert filing representation separated from raw bytes."""

    source_sha256: str
    sanitized_sha256: str
    retained_text: str
    analysis_text: str
    prompt_injection_detected: bool
    excerpts: tuple[EvidenceExcerpt, ...]


def apply_explicit_amendment_lineage(
    filings: Sequence[FilingMetadata], explicit_links: Mapping[str, str]
) -> tuple[FilingMetadata, ...]:
    """Apply only externally verified amendment-parent accession links."""
    by_accession = {item.accession_number: item for item in filings}
    if len(by_accession) != len(filings):
        raise SecEdgarError(
            SecErrorCode.MALFORMED_RESPONSE, "filing accessions are duplicated"
        )
    unknown = set(explicit_links) - set(by_accession)
    unknown.update(set(explicit_links.values()) - set(by_accession))
    if unknown:
        raise SecEdgarError(
            SecErrorCode.MALFORMED_RESPONSE, "amendment lineage names unknown accession"
        )
    result: list[FilingMetadata] = []
    for item in filings:
        parent_accession = explicit_links.get(item.accession_number)
        if parent_accession is None:
            result.append(item)
            continue
        parent = by_accession[parent_accession]
        if (
            not item.is_amendment
            or parent.is_amendment
            or parent.cik != item.cik
            or parent.base_form != item.base_form
            or parent.acceptance_time_utc_ns >= item.acceptance_time_utc_ns
        ):
            raise SecEdgarError(
                SecErrorCode.MALFORMED_RESPONSE,
                "amendment lineage is incompatible or temporally invalid",
            )
        result.append(
            FilingMetadata(
                cik=item.cik,
                accession_number=item.accession_number,
                form=item.form,
                base_form=item.base_form,
                is_amendment=item.is_amendment,
                filing_date=item.filing_date,
                report_date=item.report_date,
                acceptance_time_utc_ns=item.acceptance_time_utc_ns,
                publication_time_utc_ns=item.publication_time_utc_ns,
                publication_time_reason=item.publication_time_reason,
                receipt_time_utc_ns=item.receipt_time_utc_ns,
                processing_time_utc_ns=item.processing_time_utc_ns,
                primary_document=item.primary_document,
                source_sha256=item.source_sha256,
                amendment_parent_accession=parent_accession,
                lineage_status=LineageStatus.EXPLICIT_PARENT,
            )
        )
    return tuple(result)


def sanitize_filing_document(
    payload: bytes,
    *,
    source_uri: str,
    receipt_time_utc_ns: int,
    parser_timeout_seconds: float = MAX_PARSER_SECONDS,
    monotonic_ns: Callable[[], int] = time.monotonic_ns,
) -> SanitizedFiling:
    """Strip active content, enforce parser limits, and isolate prompt text."""
    _validate_sec_url(source_uri)
    _positive_ns(receipt_time_utc_ns, "filing receipt time")
    if not payload or len(payload) > MAX_DOCUMENT_BYTES:
        raise SecEdgarError(
            SecErrorCode.RESPONSE_TOO_LARGE, "filing document is empty or oversized"
        )
    if not 0 < parser_timeout_seconds <= MAX_PARSER_SECONDS:
        raise SecEdgarError(
            SecErrorCode.INVALID_CONFIGURATION, "parser timeout is outside bounds"
        )
    try:
        decoded = payload.decode("utf-8", "strict")
    except UnicodeDecodeError as error:
        raise SecEdgarError(
            SecErrorCode.MALFORMED_RESPONSE, "filing document is not valid UTF-8"
        ) from error
    start_ns = monotonic_ns()
    parser = _SecHtmlExtractor(
        deadline_ns=start_ns + int(parser_timeout_seconds * NANOSECONDS_PER_SECOND),
        monotonic_ns=monotonic_ns,
    )
    try:
        parser.feed(decoded)
        parser.close()
    except (AssertionError, ValueError) as error:
        raise SecEdgarError(
            SecErrorCode.MALFORMED_RESPONSE, "filing HTML parser rejected the document"
        ) from error
    visible = "\n".join(
        normalized
        for line in parser.text().replace("\x00", " ").splitlines()
        if (normalized := _WHITESPACE.sub(" ", line).strip())
    )
    if not visible:
        raise SecEdgarError(
            SecErrorCode.MALFORMED_RESPONSE, "filing has no inert visible text"
        )
    retained = visible[:MAX_SANITIZED_CHARACTERS]
    source = SourceDocument(
        provider_id="sec-edgar",
        document_id=_sha256(payload),
        source_uri=source_uri,
        content_type="text/plain; charset=utf-8",
        payload=retained.encode("utf-8"),
        provider_event_time_utc_ns=receipt_time_utc_ns,
        received_wall_clock_utc_ns=receipt_time_utc_ns,
        authentication=AuthenticationEvidence(
            status=SourceAuthentication.PUBLIC_TLS_VERIFIED,
            method="SEC_FIXED_HOST_TLS",
            evidence_sha256=sha256_bytes(source_uri),
        ),
    )
    sanitized = sanitize_document(source)
    excerpts: list[EvidenceExcerpt] = []
    headings = list(_ITEM_HEADING.finditer(sanitized.retained_text))
    for index, heading in enumerate(headings[:MAX_EVIDENCE_EXCERPTS]):
        end = min(
            headings[index + 1].start()
            if index + 1 < len(headings)
            else len(sanitized.retained_text),
            heading.start() + MAX_EVIDENCE_CHARACTERS,
        )
        text = sanitized.retained_text[heading.start() : end]
        excerpts.append(
            EvidenceExcerpt(
                start=heading.start(), end=end, text=text, sha256=_sha256(text.encode())
            )
        )
    return SanitizedFiling(
        source_sha256=_sha256(payload),
        sanitized_sha256=sanitized.sanitized_sha256.hex(),
        retained_text=sanitized.retained_text,
        analysis_text=sanitized.analysis_text,
        prompt_injection_detected=sanitized.prompt_injection_detected,
        excerpts=tuple(excerpts),
    )


def sanitize_filing_document_isolated(
    payload: bytes,
    *,
    source_uri: str,
    receipt_time_utc_ns: int,
    sanitizer: DocumentSanitizer | None = None,
) -> SanitizedFiling:
    """Sanitize a filing in the network-denied, resource-limited worker."""
    _validate_sec_url(source_uri)
    _positive_ns(receipt_time_utc_ns, "filing receipt time")
    if not payload or len(payload) > MAX_DOCUMENT_BYTES:
        raise SecEdgarError(
            SecErrorCode.RESPONSE_TOO_LARGE, "filing document is empty or oversized"
        )
    source = SourceDocument(
        provider_id="sec-edgar",
        document_id=_sha256(payload),
        source_uri=source_uri,
        content_type="text/html; charset=utf-8",
        payload=payload,
        provider_event_time_utc_ns=receipt_time_utc_ns,
        received_wall_clock_utc_ns=receipt_time_utc_ns,
        authentication=AuthenticationEvidence(
            status=SourceAuthentication.PUBLIC_TLS_VERIFIED,
            method="SEC_FIXED_HOST_TLS",
            evidence_sha256=sha256_bytes(source_uri),
        ),
    )
    boundary = sanitizer or ProcessDocumentSanitizer()
    try:
        document = boundary.sanitize(source)
    except UnsafeDocumentError as error:
        raise SecEdgarError(
            SecErrorCode.PARSER_LIMIT, "isolated filing sanitizer rejected the document"
        ) from error
    excerpts: list[EvidenceExcerpt] = []
    headings = list(_ITEM_HEADING.finditer(document.retained_text))
    for index, heading in enumerate(headings[:MAX_EVIDENCE_EXCERPTS]):
        end = min(
            headings[index + 1].start()
            if index + 1 < len(headings)
            else len(document.retained_text),
            heading.start() + MAX_EVIDENCE_CHARACTERS,
        )
        text = document.retained_text[heading.start() : end]
        excerpts.append(
            EvidenceExcerpt(
                start=heading.start(), end=end, text=text, sha256=_sha256(text.encode())
            )
        )
    return SanitizedFiling(
        source_sha256=_sha256(payload),
        sanitized_sha256=document.sanitized_sha256.hex(),
        retained_text=document.retained_text,
        analysis_text=document.analysis_text,
        prompt_injection_detected=document.prompt_injection_detected,
        excerpts=tuple(excerpts),
    )


@dataclass(frozen=True, slots=True)
class SecPublication:
    """Raw object plus explicit nullable source-time semantics."""

    dataset: SecDataset
    source_url: str
    payload: bytes
    receipt_time_utc_ns: int
    processing_time_utc_ns: int
    record_count: int
    schema_name: str
    acceptance_time_min_utc_ns: int | None = None
    acceptance_time_max_utc_ns: int | None = None
    publication_time_min_utc_ns: int | None = None
    publication_time_max_utc_ns: int | None = None
    publication_time_reason: str = "NOT_PROVIDED_BY_SOURCE"
    parent_manifest_id: str | None = None

    def validate(self) -> None:
        """Reject ambiguous, disordered, or incomplete timestamp pairs."""
        _validate_sec_url(self.source_url)
        if (
            not self.payload
            or self.record_count < 0
            or not self.schema_name
            or self.processing_time_utc_ns < self.receipt_time_utc_ns
            or (self.acceptance_time_min_utc_ns is None)
            != (self.acceptance_time_max_utc_ns is None)
            or (self.publication_time_min_utc_ns is None)
            != (self.publication_time_max_utc_ns is None)
        ):
            raise SecEdgarError(
                SecErrorCode.TIMESTAMP_DISORDER, "SEC publication metadata is invalid"
            )
        _positive_ns(self.receipt_time_utc_ns, "publication receipt time")
        _positive_ns(self.processing_time_utc_ns, "publication processing time")
        for minimum, maximum, name in (
            (
                self.acceptance_time_min_utc_ns,
                self.acceptance_time_max_utc_ns,
                "acceptance",
            ),
            (
                self.publication_time_min_utc_ns,
                self.publication_time_max_utc_ns,
                "publication",
            ),
        ):
            if minimum is not None and maximum is not None:
                _positive_ns(minimum, f"{name} minimum")
                _positive_ns(maximum, f"{name} maximum")
                if maximum < minimum or maximum > self.receipt_time_utc_ns:
                    raise SecEdgarError(
                        SecErrorCode.TIMESTAMP_DISORDER,
                        f"{name} range is disordered",
                    )


@dataclass(frozen=True, slots=True)
class SecArtifactManifest:
    """SEC-specific immutable manifest preserving unavailable timestamps."""

    dataset: SecDataset
    source_url_sha256: str
    storage_path: str
    object_sha256: str
    size_bytes: int
    record_count: int
    schema_name: str
    universe_snapshot_sha256: str
    acceptance_time_min_utc_ns: int | None
    acceptance_time_max_utc_ns: int | None
    publication_time_min_utc_ns: int | None
    publication_time_max_utc_ns: int | None
    publication_time_reason: str
    receipt_time_utc_ns: int
    processing_time_utc_ns: int
    parent_manifest_id: str | None

    def payload(self) -> dict[str, object]:
        """Return canonical manifest fields."""
        return {
            "acceptance_time_max_utc_ns": self.acceptance_time_max_utc_ns,
            "acceptance_time_min_utc_ns": self.acceptance_time_min_utc_ns,
            "dataset": self.dataset.value,
            "object_sha256": self.object_sha256,
            "parent_manifest_id": self.parent_manifest_id,
            "processing_time_utc_ns": self.processing_time_utc_ns,
            "publication_time_max_utc_ns": self.publication_time_max_utc_ns,
            "publication_time_min_utc_ns": self.publication_time_min_utc_ns,
            "publication_time_reason": self.publication_time_reason,
            "receipt_time_utc_ns": self.receipt_time_utc_ns,
            "record_count": self.record_count,
            "schema_name": self.schema_name,
            "schema_version": SEC_SCHEMA_VERSION,
            "size_bytes_decimal": self.size_bytes,
            "source_revision": SEC_SOURCE_REVISION,
            "source_url_sha256": self.source_url_sha256,
            "storage_path": self.storage_path,
            "universe_snapshot_sha256": self.universe_snapshot_sha256,
        }

    @property
    def manifest_id(self) -> str:
        """Derive the immutable identity from all source-manifest fields."""
        return f"sec-{_sha256(_canonical_bytes(self.payload()))}"

    def encode(self) -> bytes:
        """Encode a self-hashed canonical manifest."""
        body = self.payload()
        return (
            _canonical_bytes(
                {
                    **body,
                    "manifest_id": self.manifest_id,
                    "manifest_sha256": _sha256(_canonical_bytes(body)),
                }
            )
            + b"\n"
        )


def _write_all(descriptor: int, payload: bytes) -> None:
    offset = 0
    while offset < len(payload):
        written = os.write(descriptor, payload[offset:])
        if written <= 0:
            raise SecEdgarError(SecErrorCode.STORAGE_LIMIT, "SEC write was partial")
        offset += written


class SecRepository:
    """Immutable SEC object publisher layered over global storage admission."""

    def __init__(
        self,
        repository: DataRepository,
        quota: QuotaEvidence,
        *,
        universe_sha256: str,
        storage_limit_bytes: int = MAX_SEC_STORAGE_BYTES,
    ) -> None:
        """Bind a global repository, quota evidence, and SEC-specific cap."""
        if not re.fullmatch(r"[0-9a-f]{64}", universe_sha256):
            raise SecEdgarError(
                SecErrorCode.INVALID_CONFIGURATION, "universe SHA-256 is invalid"
            )
        if not 0 < storage_limit_bytes <= MAX_SEC_STORAGE_BYTES:
            raise SecEdgarError(
                SecErrorCode.INVALID_CONFIGURATION, "SEC storage limit is invalid"
            )
        self.repository = repository
        self.quota = quota
        self.universe_sha256 = universe_sha256
        self.storage_limit_bytes = storage_limit_bytes
        # DataRepository intentionally exposes a non-blocking single-writer
        # admission fence. SEC fetch workers may run concurrently, but their
        # local publication phase must enter that fence one at a time.
        self._publication_lock = threading.Lock()
        self._batch_context_lock = threading.Lock()
        self._batch_lease: AdmissionLease | None = None

    @contextmanager
    def publication_batch(self) -> Iterator[None]:
        """Reserve the bounded SEC ceiling once for a multi-object operation."""
        with self._batch_context_lock:
            current = self.usage_bytes()
            remaining = self.storage_limit_bytes - current
            if remaining <= 0:
                raise SecEdgarError(
                    SecErrorCode.STORAGE_LIMIT,
                    "SEC storage has no remaining admitted capacity",
                )
            request = StorageRequest(
                operation_id="sec-edgar-bounded-publication-batch",
                output_bytes=remaining,
                temporary_bytes=MAX_COMPRESSED_BYTES,
            )
            with self.repository.acquire_admission(request, self.quota) as lease:
                self._batch_lease = lease
                try:
                    yield
                finally:
                    self._batch_lease = None

    def usage_bytes(self) -> int:
        """Return committed SEC source bytes without following symlinks."""
        total = 0
        roots = (
            *(
                self.repository.root / area / "sec-edgar"
                for area in ("raw", "canonical", "derived", "datasets", "quarantine")
            ),
            self.repository.root / "manifests/sec-edgar",
        )
        for root in roots:
            if not root.exists():
                continue
            if root.is_symlink():
                raise SecEdgarError(
                    SecErrorCode.STORAGE_LIMIT, "SEC storage root is a symlink"
                )
            for directory, names, files in os.walk(root, followlinks=False):
                if any((Path(directory) / name).is_symlink() for name in names):
                    raise SecEdgarError(
                        SecErrorCode.STORAGE_LIMIT, "SEC tree contains symlink"
                    )
                for filename in files:
                    path = Path(directory) / filename
                    status = os.lstat(path)
                    if not stat.S_ISREG(status.st_mode) or stat.S_ISLNK(status.st_mode):
                        raise SecEdgarError(
                            SecErrorCode.STORAGE_LIMIT,
                            "SEC tree contains unsafe object",
                        )
                    total += status.st_size
                    if total > self.storage_limit_bytes:
                        raise SecEdgarError(
                            SecErrorCode.STORAGE_LIMIT,
                            "SEC storage already exceeds its cap",
                        )
        return total

    def publish(self, publication: SecPublication) -> SecArtifactManifest:
        """Atomically publish raw bytes and an SEC-specific source manifest."""
        with self._publication_lock:
            return self._publish_serialized(publication)

    def _publish_serialized(self, publication: SecPublication) -> SecArtifactManifest:
        """Publish while holding the SEC-local single-writer boundary."""
        publication.validate()
        payload = publication.payload
        current = self.usage_bytes()
        if len(payload) > self.storage_limit_bytes - current:
            raise SecEdgarError(
                SecErrorCode.STORAGE_LIMIT, "SEC object would exceed the 8 GB cap"
            )
        digest = _sha256(payload)
        suffix = (
            ".json"
            if publication.dataset is not SecDataset.PRIMARY_FILING_DOCUMENT
            else ".html"
        )
        final_relative = (
            f"raw/sec-edgar/{publication.dataset.value.casefold()}/{digest}{suffix}"
        )
        staged_relative = f"tmp/sec-edgar-{digest}.partial"
        request = StorageRequest(
            operation_id=(f"sec-{publication.dataset.value.casefold()}-{digest[:24]}"),
            output_bytes=len(payload) + MANIFEST_BUDGET_BYTES,
            temporary_bytes=len(payload),
        )
        if self._batch_lease is not None:
            return self._publish_admitted(
                publication,
                payload,
                digest,
                staged_relative,
                final_relative,
                self._batch_lease,
            )
        with self.repository.acquire_admission(request, self.quota) as lease:
            return self._publish_admitted(
                publication,
                payload,
                digest,
                staged_relative,
                final_relative,
                lease,
            )

    def _publish_admitted(
        self,
        publication: SecPublication,
        payload: bytes,
        digest: str,
        staged_relative: str,
        final_relative: str,
        lease: AdmissionLease,
    ) -> SecArtifactManifest:
        """Publish one object under an active per-object or batch lease."""
        staged = self.repository.root / staged_relative
        staged.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        try:
            descriptor = os.open(
                staged,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
                0o600,
            )
        except FileExistsError:
            if staged.is_symlink() or staged.read_bytes() != payload:
                raise SecEdgarError(
                    SecErrorCode.STORAGE_LIMIT, "staged SEC object conflicts"
                ) from None
        else:
            try:
                _write_all(descriptor, payload)
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
        self.repository.publish_staged_object(
            staged_relative,
            final_relative,
            expected_sha256=digest,
            expected_size_bytes=len(payload),
            lease=lease,
        )
        manifest = SecArtifactManifest(
            dataset=publication.dataset,
            source_url_sha256=_sha256(publication.source_url.encode()),
            storage_path=final_relative,
            object_sha256=digest,
            size_bytes=len(payload),
            record_count=publication.record_count,
            schema_name=publication.schema_name,
            universe_snapshot_sha256=self.universe_sha256,
            acceptance_time_min_utc_ns=publication.acceptance_time_min_utc_ns,
            acceptance_time_max_utc_ns=publication.acceptance_time_max_utc_ns,
            publication_time_min_utc_ns=publication.publication_time_min_utc_ns,
            publication_time_max_utc_ns=publication.publication_time_max_utc_ns,
            publication_time_reason=publication.publication_time_reason,
            receipt_time_utc_ns=publication.receipt_time_utc_ns,
            processing_time_utc_ns=publication.processing_time_utc_ns,
            parent_manifest_id=publication.parent_manifest_id,
        )
        self._publish_manifest(manifest)
        return manifest

    def _publish_manifest(self, manifest: SecArtifactManifest) -> None:
        encoded = manifest.encode()
        directory = self.repository.root / "manifests/sec-edgar"
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        final = directory / f"{manifest.manifest_id}.json"
        if final.exists():
            if final.is_symlink() or final.read_bytes() != encoded:
                raise SecEdgarError(
                    SecErrorCode.STORAGE_LIMIT,
                    "SEC manifest conflicts with immutable data",
                )
            return
        staged = self.repository.root / f"tmp/{manifest.manifest_id}.partial"
        descriptor = os.open(
            staged,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
            0o600,
        )
        try:
            _write_all(descriptor, encoded)
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
        try:
            os.link(staged, final, follow_symlinks=False)
            staged.unlink()
        except FileExistsError:
            if final.is_symlink() or final.read_bytes() != encoded:
                raise SecEdgarError(
                    SecErrorCode.STORAGE_LIMIT, "SEC manifest publication raced"
                ) from None
            staged.unlink()


@dataclass(frozen=True, slots=True)
class IssuerCoverage:
    """One ticker's deterministic SEC coverage outcome."""

    ticker: str
    cik: str | None
    issuer_name: str | None
    resolution_status: IssuerResolutionStatus
    filing_count: int
    amendment_count: int
    fact_count: int
    forms: Mapping[str, int]
    errors: tuple[str, ...]
    historical_shard_count: int = 0

    def __post_init__(self) -> None:
        """Reject negative coverage counts."""
        if (
            min(
                self.filing_count,
                self.amendment_count,
                self.fact_count,
                self.historical_shard_count,
            )
            < 0
        ):
            raise SecEdgarError(
                SecErrorCode.MALFORMED_RESPONSE,
                "issuer coverage counts cannot be negative",
            )


@dataclass(frozen=True, slots=True)
class SecCoverageReport:
    """Machine-readable issuer and filing coverage with no filing text."""

    universe_sha256: str
    generated_at_utc_ns: int
    network_access_performed: bool
    issuer_coverage: tuple[IssuerCoverage, ...]
    request_count: int
    filing_start_date: date | None = None
    filing_end_date: date | None = None

    def __post_init__(self) -> None:
        """Require an absent or complete ordered filing window."""
        if (
            (self.filing_start_date is None) != (self.filing_end_date is None)
            or (
                self.filing_start_date is not None
                and self.filing_end_date is not None
                and self.filing_start_date > self.filing_end_date
            )
            or self.request_count < 0
        ):
            raise SecEdgarError(
                SecErrorCode.MALFORMED_RESPONSE,
                "coverage report window or request count is invalid",
            )

    def to_dict(self) -> dict[str, object]:
        """Return a deterministic, content-free report with an integrity hash."""
        resolved = sum(
            item.resolution_status is IssuerResolutionStatus.RESOLVED_CURRENT_ONLY
            for item in self.issuer_coverage
        )
        body: dict[str, object] = {
            "filing_end_date": (
                self.filing_end_date.isoformat()
                if self.filing_end_date is not None
                else None
            ),
            "filing_start_date": (
                self.filing_start_date.isoformat()
                if self.filing_start_date is not None
                else None
            ),
            "generated_at_utc_ns": self.generated_at_utc_ns,
            "issuer_coverage": [
                {
                    "amendment_count": item.amendment_count,
                    "cik": item.cik,
                    "errors": list(item.errors),
                    "fact_count": item.fact_count,
                    "filing_count": item.filing_count,
                    "forms": dict(sorted(item.forms.items())),
                    "historical_shard_count": item.historical_shard_count,
                    "issuer_name": item.issuer_name,
                    "resolution_status": item.resolution_status.value,
                    "ticker": item.ticker,
                }
                for item in self.issuer_coverage
            ],
            "network_access_performed": self.network_access_performed,
            "request_count": self.request_count,
            "schema_version": SEC_REPORT_SCHEMA_VERSION,
            "summary": {
                "amendment_count": sum(
                    item.amendment_count for item in self.issuer_coverage
                ),
                "fact_count": sum(item.fact_count for item in self.issuer_coverage),
                "filing_count": sum(item.filing_count for item in self.issuer_coverage),
                "historical_shard_count": sum(
                    item.historical_shard_count for item in self.issuer_coverage
                ),
                "requested_ticker_count": len(self.issuer_coverage),
                "resolved_ticker_count": resolved,
                "unresolved_ticker_count": len(self.issuer_coverage) - resolved,
            },
            "universe_sha256": self.universe_sha256,
        }
        return {**body, "report_sha256": _sha256(_canonical_bytes(body))}


class SecEdgarAdapter:
    """Bounded coordinator for current issuer mapping, filings, and facts."""

    def __init__(
        self,
        client: SecClient,
        authorization: SecAuthorization,
        *,
        repository: SecRepository | None = None,
        document_sanitizer: DocumentSanitizer | None = None,
        wall_clock_ns: Callable[[], int] = time.time_ns,
    ) -> None:
        """Bind the client and approval to a wall-clock evidence source."""
        self.client = client
        self.authorization = authorization
        self.repository = repository
        self._document_sanitizer = document_sanitizer or ProcessDocumentSanitizer()
        self._wall_clock_ns = wall_clock_ns

    def _require(self, dataset: SecDataset) -> None:
        if not self.authorization.permits(dataset, self._wall_clock_ns()):
            raise SecEdgarError(
                SecErrorCode.UNAUTHORIZED, f"dataset {dataset.value} is not authorized"
            )

    def resolve_issuers(
        self, universe: UniverseSnapshot
    ) -> tuple[IssuerResolution, ...]:
        """Fetch and parse the official current ticker association."""
        self._require(SecDataset.COMPANY_TICKERS)
        response = self.client.get(company_tickers_url())
        resolutions = parse_company_tickers(
            response.body, universe, received_at_utc_ns=response.received_at_utc_ns
        )
        if self.repository is not None:
            self.repository.publish(
                SecPublication(
                    dataset=SecDataset.COMPANY_TICKERS,
                    source_url=company_tickers_url(),
                    payload=response.body,
                    receipt_time_utc_ns=response.received_at_utc_ns,
                    processing_time_utc_ns=self._wall_clock_ns(),
                    record_count=len(resolutions),
                    schema_name="sec-company-tickers-current-json",
                )
            )
        return resolutions

    def ingest_issuer(
        self, resolution: IssuerResolution
    ) -> tuple[tuple[FilingMetadata, ...], tuple[CompanyFact, ...], int]:
        """Fetch one resolved issuer's submissions and company facts."""
        if (
            resolution.status is not IssuerResolutionStatus.RESOLVED_CURRENT_ONLY
            or resolution.cik is None
        ):
            raise SecEdgarError(
                SecErrorCode.UNSUPPORTED_ISSUER,
                "issuer must be resolved before ingestion",
            )
        self._require(SecDataset.SUBMISSIONS)
        self._require(SecDataset.COMPANY_FACTS)
        submission_response = self.client.get(submissions_url(resolution.cik))
        submission_processed = self._wall_clock_ns()
        filings = parse_submissions(
            submission_response.body,
            expected_cik=resolution.cik,
            received_at_utc_ns=submission_response.received_at_utc_ns,
            processing_time_utc_ns=submission_processed,
            allowed_forms=self.authorization.allowed_forms,
            filing_start_date=self.client.config.filing_start_date,
            filing_end_date=self.client.config.filing_end_date,
        )
        if self.repository is not None:
            acceptance_times = [item.acceptance_time_utc_ns for item in filings]
            self.repository.publish(
                SecPublication(
                    dataset=SecDataset.SUBMISSIONS,
                    source_url=submissions_url(resolution.cik),
                    payload=submission_response.body,
                    receipt_time_utc_ns=submission_response.received_at_utc_ns,
                    processing_time_utc_ns=submission_processed,
                    record_count=len(filings),
                    schema_name="sec-submissions-json",
                    acceptance_time_min_utc_ns=(
                        min(acceptance_times) if acceptance_times else None
                    ),
                    acceptance_time_max_utc_ns=(
                        max(acceptance_times) if acceptance_times else None
                    ),
                )
            )
        request_count = 1
        historical_filings: list[FilingMetadata] = []
        if (
            self.client.config.filing_start_date is not None
            and self.client.config.filing_end_date is not None
        ):
            shards = historical_submission_shards(
                submission_response.body, expected_cik=resolution.cik
            )
            selected_shards = tuple(
                shard
                for shard in shards
                if shard.overlaps(
                    self.client.config.filing_start_date,
                    self.client.config.filing_end_date,
                )
            )
            for shard in selected_shards:
                shard_url = historical_submissions_url(shard.filename)
                shard_response = self.client.get(shard_url)
                shard_processed = self._wall_clock_ns()
                parsed = parse_submissions(
                    shard_response.body,
                    expected_cik=resolution.cik,
                    received_at_utc_ns=shard_response.received_at_utc_ns,
                    processing_time_utc_ns=shard_processed,
                    allowed_forms=self.authorization.allowed_forms,
                    filing_start_date=self.client.config.filing_start_date,
                    filing_end_date=self.client.config.filing_end_date,
                )
                historical_filings.extend(parsed)
                request_count += 1
                if self.repository is not None:
                    shard_acceptance_times = [
                        item.acceptance_time_utc_ns for item in parsed
                    ]
                    self.repository.publish(
                        SecPublication(
                            dataset=SecDataset.SUBMISSIONS,
                            source_url=shard_url,
                            payload=shard_response.body,
                            receipt_time_utc_ns=shard_response.received_at_utc_ns,
                            processing_time_utc_ns=shard_processed,
                            record_count=len(parsed),
                            schema_name="sec-submissions-historical-json",
                            acceptance_time_min_utc_ns=(
                                min(shard_acceptance_times)
                                if shard_acceptance_times
                                else None
                            ),
                            acceptance_time_max_utc_ns=(
                                max(shard_acceptance_times)
                                if shard_acceptance_times
                                else None
                            ),
                        )
                    )
        combined_filings = (*filings, *historical_filings)
        accessions = [item.accession_number for item in combined_filings]
        if len(set(accessions)) != len(accessions):
            raise SecEdgarError(
                SecErrorCode.MALFORMED_RESPONSE,
                "current and historical submissions contain duplicate accessions",
            )
        fact_response = self.client.get(company_facts_url(resolution.cik))
        fact_processed = self._wall_clock_ns()
        facts = parse_company_facts(
            fact_response.body,
            expected_cik=resolution.cik,
            received_at_utc_ns=fact_response.received_at_utc_ns,
            processing_time_utc_ns=fact_processed,
            allowed_forms=self.authorization.allowed_forms,
            filing_start_date=self.client.config.filing_start_date,
            filing_end_date=self.client.config.filing_end_date,
        )
        if self.repository is not None:
            self.repository.publish(
                SecPublication(
                    dataset=SecDataset.COMPANY_FACTS,
                    source_url=company_facts_url(resolution.cik),
                    payload=fact_response.body,
                    receipt_time_utc_ns=fact_response.received_at_utc_ns,
                    processing_time_utc_ns=fact_processed,
                    record_count=len(facts),
                    schema_name="sec-company-facts-json",
                )
            )
        return tuple(combined_filings), facts, request_count + 1

    def ingest_primary_document(self, filing: FilingMetadata) -> SanitizedFiling:
        """Fetch and sanitize only the declared primary document for one filing."""
        self._require(SecDataset.PRIMARY_FILING_DOCUMENT)
        if not self.client.config.download_primary_documents:
            raise SecEdgarError(
                SecErrorCode.UNAUTHORIZED,
                "primary-document retrieval requires an explicit configuration flag",
            )
        if filing.base_form not in self.authorization.allowed_forms:
            raise SecEdgarError(
                SecErrorCode.UNAUTHORIZED, "filing form is not authorized"
            )
        url = primary_document_url(
            filing.cik, filing.accession_number, filing.primary_document
        )
        response = self.client.get(url)
        sanitized = sanitize_filing_document_isolated(
            response.body,
            source_uri=url,
            receipt_time_utc_ns=response.received_at_utc_ns,
            sanitizer=self._document_sanitizer,
        )
        if self.repository is not None:
            self.repository.publish(
                SecPublication(
                    dataset=SecDataset.PRIMARY_FILING_DOCUMENT,
                    source_url=url,
                    payload=response.body,
                    receipt_time_utc_ns=response.received_at_utc_ns,
                    processing_time_utc_ns=self._wall_clock_ns(),
                    record_count=len(sanitized.excerpts),
                    schema_name="sec-primary-filing-html",
                    acceptance_time_min_utc_ns=filing.acceptance_time_utc_ns,
                    acceptance_time_max_utc_ns=filing.acceptance_time_utc_ns,
                    publication_time_reason=filing.publication_time_reason,
                )
            )
        return sanitized

    def coverage(self, universe: UniverseSnapshot) -> SecCoverageReport:
        """Collect coverage under one fail-closed global storage reservation."""
        if self.repository is None:
            return self._coverage_impl(universe)
        with self.repository.publication_batch():
            return self._coverage_impl(universe)

    def _coverage_impl(self, universe: UniverseSnapshot) -> SecCoverageReport:
        """Collect bounded per-issuer coverage with explicit partial failures."""
        resolutions = self.resolve_issuers(universe)
        by_symbol: dict[str, IssuerCoverage] = {}
        request_count = 1

        def ingest(
            item: IssuerResolution,
        ) -> tuple[
            IssuerResolution, tuple[FilingMetadata, ...], tuple[CompanyFact, ...], int
        ]:
            filings, facts, requests = self.ingest_issuer(item)
            return item, filings, facts, requests

        resolved = [
            item
            for item in resolutions
            if item.status is IssuerResolutionStatus.RESOLVED_CURRENT_ONLY
        ]
        with ThreadPoolExecutor(max_workers=self.client.config.concurrency) as executor:
            futures = {executor.submit(ingest, item): item for item in resolved}
            for future in as_completed(futures):
                item = futures[future]
                try:
                    _, filings, facts, requests = future.result()
                    request_count += requests
                    forms: dict[str, int] = {}
                    for filing in filings:
                        forms[filing.form] = forms.get(filing.form, 0) + 1
                    by_symbol[item.ticker] = IssuerCoverage(
                        ticker=item.ticker,
                        cik=item.cik,
                        issuer_name=item.issuer_name,
                        resolution_status=item.status,
                        filing_count=len(filings),
                        amendment_count=sum(filing.is_amendment for filing in filings),
                        fact_count=len(facts),
                        forms=forms,
                        errors=(),
                        historical_shard_count=max(requests - 2, 0),
                    )
                except SecEdgarError as error:
                    by_symbol[item.ticker] = IssuerCoverage(
                        ticker=item.ticker,
                        cik=item.cik,
                        issuer_name=item.issuer_name,
                        resolution_status=item.status,
                        filing_count=0,
                        amendment_count=0,
                        fact_count=0,
                        forms={},
                        errors=(error.code.value,),
                    )
        for item in resolutions:
            if item.ticker not in by_symbol:
                by_symbol[item.ticker] = IssuerCoverage(
                    ticker=item.ticker,
                    cik=None,
                    issuer_name=None,
                    resolution_status=item.status,
                    filing_count=0,
                    amendment_count=0,
                    fact_count=0,
                    forms={},
                    errors=(item.reason,),
                )
        return SecCoverageReport(
            universe_sha256=universe.universe_snapshot_sha256.hex(),
            generated_at_utc_ns=self._wall_clock_ns(),
            network_access_performed=True,
            issuer_coverage=tuple(
                by_symbol[entry.symbol] for entry in universe.ordered_entries
            ),
            request_count=request_count,
            filing_start_date=self.client.config.filing_start_date,
            filing_end_date=self.client.config.filing_end_date,
        )


def _write_report(report: SecCoverageReport, directory: Path) -> tuple[Path, Path]:
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    machine = directory / "issuer-filing-coverage-v1_1.json"
    human = directory / "issuer-filing-coverage-v1_1.md"
    document = report.to_dict()
    encoded = _canonical_bytes(document) + b"\n"
    machine.write_bytes(encoded)
    machine.chmod(0o600)
    summary = cast("dict[str, object]", document["summary"])
    lines = [
        "# SEC EDGAR issuer and filing coverage",
        "",
        f"- Universe SHA-256: `{report.universe_sha256}`",
        f"- Requested tickers: {summary['requested_ticker_count']}",
        f"- Current SEC mappings resolved: {summary['resolved_ticker_count']}",
        f"- Unresolved or ambiguous: {summary['unresolved_ticker_count']}",
        f"- Selected filings: {summary['filing_count']}",
        f"- Selected amendments: {summary['amendment_count']}",
        f"- Selected XBRL observations: {summary['fact_count']}",
        f"- Historical submission shards: {summary['historical_shard_count']}",
        (
            "- Filing-date window: "
            f"{document['filing_start_date']} through {document['filing_end_date']}"
        ),
        f"- SEC requests: {report.request_count}",
        "",
        (
            "Current ticker/CIK associations are not historical symbology. Filing "
            "publication time remains explicit as unavailable when the submissions "
            "API exposes only acceptance time."
        ),
    ]
    human.write_text("\n".join(lines) + "\n", encoding="utf-8")
    human.chmod(0o600)
    return machine, human


def cli_main(argv: Sequence[str] | None = None) -> int:
    """Run an explicit SEC coverage retrieval; dry-run is the default."""
    parser = argparse.ArgumentParser(prog="aegis-sec-edgar")
    parser.add_argument("coverage", nargs="?", choices=("coverage",))
    parser.add_argument("--ticker-file", type=Path, default=TICKER_UNIVERSE_PATH)
    parser.add_argument("--approval", type=Path, required=True)
    parser.add_argument("--data-root", type=Path, default=DEFAULT_DATA_ROOT)
    parser.add_argument(
        "--report-directory", type=Path, default=DEFAULT_REPORT_DIRECTORY
    )
    parser.add_argument("--filing-start-date", type=date.fromisoformat)
    parser.add_argument("--filing-end-date", type=date.fromisoformat)
    parser.add_argument("--quota-limit-bytes", type=int)
    parser.add_argument("--quota-used-bytes", type=int)
    parser.add_argument("--quota-source")
    parser.add_argument("--quota-observed-at-utc")
    parser.add_argument("--quota-authoritative", action="store_true")
    parser.add_argument("--execute", action="store_true")
    arguments = parser.parse_args(argv)
    if arguments.ticker_file != TICKER_UNIVERSE_PATH:
        raise SecEdgarError(
            SecErrorCode.INVALID_CONFIGURATION,
            "only the authoritative ticker.txt path is accepted",
        )
    universe = load_ticker_universe(UnresolvedInstrumentResolver())
    if not arguments.execute:
        sys.stdout.write(
            json.dumps(
                {
                    "dry_run": True,
                    "network_access_performed": False,
                    "requested_ticker_count": len(universe.ordered_entries),
                    "universe_sha256": universe.universe_snapshot_sha256.hex(),
                },
                sort_keys=True,
            )
            + "\n"
        )
        return 0
    user_agent = os.environ.get("AEGIS_SEC_USER_AGENT", "")
    authorization = SecAuthorization.load(arguments.approval)
    if arguments.filing_start_date is None or arguments.filing_end_date is None:
        raise SecEdgarError(
            SecErrorCode.INVALID_CONFIGURATION,
            "execution requires an explicit complete filing date window",
        )
    config = SecConfig(
        user_agent=user_agent,
        filing_start_date=arguments.filing_start_date,
        filing_end_date=arguments.filing_end_date,
    )
    quota = QuotaEvidence(
        limit_bytes=arguments.quota_limit_bytes,
        used_bytes=arguments.quota_used_bytes,
        source=arguments.quota_source,
        observed_at_utc=arguments.quota_observed_at_utc,
        authoritative=arguments.quota_authoritative,
    )
    if not quota.known:
        raise SecEdgarError(
            SecErrorCode.STORAGE_LIMIT,
            "execution requires complete authoritative user-quota evidence",
        )
    repository = DataRepository(arguments.data_root)
    repository.initialize()
    sink = SecRepository(
        repository,
        quota,
        universe_sha256=universe.universe_snapshot_sha256.hex(),
        storage_limit_bytes=config.storage_limit_bytes,
    )
    adapter = SecEdgarAdapter(
        SecClient(config, UrllibSecTransport()), authorization, repository=sink
    )
    machine, human = _write_report(
        adapter.coverage(universe), arguments.report_directory
    )
    sys.stdout.write(
        json.dumps(
            {"human_report": str(human), "machine_report": str(machine)}, sort_keys=True
        )
        + "\n"
    )
    return 0


if __name__ == "__main__":  # pragma: no cover - console-script boundary
    raise SystemExit(cli_main())
