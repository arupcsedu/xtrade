"""Bounded GDELT metadata ingestion and advisory entity classification."""

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
import unicodedata
from contextlib import contextmanager, nullcontext
from dataclasses import dataclass, field, replace
from datetime import UTC, date, datetime, timedelta
from enum import StrEnum
from pathlib import Path
from typing import TYPE_CHECKING, Final, Protocol, Self, cast, override
from urllib.error import HTTPError, URLError
from urllib.parse import SplitResult, urlsplit, urlunsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener

from aegis_mx_intelligence.contracts import Identifier128, InstrumentId
from aegis_mx_intelligence.news_security import contains_prompt_injection

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
from aegis_mx_research.ingestion import SecretValue

if TYPE_CHECKING:
    from collections.abc import Callable, Iterator, Mapping, Sequence

GDELT_SCHEMA_VERSION: Final = "1.0.0"
GDELT_REPORT_SCHEMA_VERSION: Final = "1.0.0"
GDELT_SOURCE_REVISION: Final = "gdelt-gkg-bigquery-reviewed-2026-09-11"
GDELT_TABLE: Final = "gdelt-bq.gdeltv2.gkg_partitioned"
GDELT_ATTRIBUTION: Final = (
    "Data source: The GDELT Project (https://www.gdeltproject.org/)"
)
DEFAULT_DATA_ROOT: Final = Path("/scratch/djy8hg/aegis_mx_poc_data")
DEFAULT_REFERENCE_SNAPSHOT: Final = (
    DEFAULT_DATA_ROOT / "reports/reference-data/prompt-52/reference-snapshot-v1.json"
)
DEFAULT_SEC_COVERAGE: Final = (
    DEFAULT_DATA_ROOT / "reports/sec-edgar/prompt-53/issuer-filing-coverage-v1_1.json"
)
DEFAULT_REPORT_DIRECTORY: Final = DEFAULT_DATA_ROOT / "reports/gdelt/prompt-54"
MAX_GDELT_STORAGE_BYTES: Final = 5 * DECIMAL_GB
MAX_QUERY_ROWS: Final = 25_000
MAX_ALIAS_BATCH: Final = 32
MAX_QUERY_PLANS: Final = 2_000
MAX_RECORD_BYTES: Final = 64 * 1024
MAX_FIELD_BYTES: Final = 4_096
MAX_URL_BYTES: Final = 2_048
MAX_THEMES: Final = 256
MAX_ORGANIZATIONS: Final = 128
MAX_REJECTION_EXAMPLES: Final = 100
MANIFEST_BUDGET_BYTES: Final = 16_384
MAX_APPROVAL_BYTES: Final = 32_768
MAX_REFERENCE_BYTES: Final = 64 * 1024 * 1024
MAX_REPORT_BYTES: Final = 4 * 1024 * 1024
MAX_HTTP_RESPONSE_BYTES: Final = 64 * 1024 * 1024
BIGQUERY_QUERY_URL_PREFIX: Final = (
    "https://bigquery.googleapis.com/bigquery/v2/projects/"
)
NANOSECONDS_PER_SECOND: Final = 1_000_000_000
HTTP_OK: Final = 200
MIN_HTTP_STATUS: Final = 100
MAX_HTTP_STATUS: Final = 599
_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_PROJECT_ID = re.compile(r"^[a-z][a-z0-9-]{4,28}[a-z0-9]$")
_SYMBOL = re.compile(r"^[A-Z][A-Z0-9]*(?:[.-][A-Z0-9]+)?$")
_THEME = re.compile(r"^[A-Z][A-Z0-9_]{0,127}$")
_LANGUAGE = re.compile(r"^[a-z]{3}$|^und$")
_CONTROL = re.compile(r"[\x00-\x1f\x7f]")
_SPACE = re.compile(r"\s+")
_PUNCTUATION = re.compile(r"[^a-z0-9]+")
_LEGAL_SUFFIX = re.compile(
    r"\b(?:incorporated|inc|corporation|corp|company|co|limited|ltd|plc|nv|sa|ag)\b"
)
_QUALIFIER = re.compile(r"\s*/[^/]*/\s*")
_OFFSET = re.compile(r"^(.*),([0-9]+)$")
_GDELT_TIME = re.compile(r"^[0-9]{14}$")
_ALLOWED_SOURCE_COLLECTION = 1
_MINIMUM_ALIAS_CHARACTERS = 3
_DECEMBER = 12


class GdeltErrorCode(StrEnum):
    """Stable fail-closed GDELT outcomes."""

    INVALID_CONFIGURATION = "INVALID_CONFIGURATION"
    AUTHORIZATION_DENIED = "AUTHORIZATION_DENIED"
    MALFORMED_RESPONSE = "MALFORMED_RESPONSE"
    RESPONSE_TOO_LARGE = "RESPONSE_TOO_LARGE"
    QUERY_LIMIT = "QUERY_LIMIT"
    TIMESTAMP_DISORDER = "TIMESTAMP_DISORDER"
    UNSAFE_METADATA = "UNSAFE_METADATA"
    MISSING_SOURCE_URL = "MISSING_SOURCE_URL"
    UNSUPPORTED_SOURCE = "UNSUPPORTED_SOURCE"
    UNSUPPORTED_LANGUAGE = "UNSUPPORTED_LANGUAGE"
    ENTITY_UNRESOLVED = "ENTITY_UNRESOLVED"
    ENTITY_AMBIGUOUS = "ENTITY_AMBIGUOUS"
    DUPLICATE_PROVIDER_ID = "DUPLICATE_PROVIDER_ID"
    DUPLICATE_CONTENT = "DUPLICATE_CONTENT"
    PROVIDER_ID_CONFLICT = "PROVIDER_ID_CONFLICT"
    STORAGE_LIMIT = "STORAGE_LIMIT"
    REMOTE_FAILURE = "REMOTE_FAILURE"
    CREDENTIAL_UNAVAILABLE = "CREDENTIAL_UNAVAILABLE"


class GdeltError(ValueError):
    """Typed GDELT failure without untrusted payload disclosure."""

    def __init__(self, code: GdeltErrorCode, message: str) -> None:
        """Retain a machine-readable reason code and safe message."""
        super().__init__(message)
        self.code = code


class EntityMatchStatus(StrEnum):
    """Deterministic issuer-name resolution outcome."""

    RESOLVED = "RESOLVED"
    AMBIGUOUS = "AMBIGUOUS"
    REJECTED = "REJECTED"


class DataQualityState(StrEnum):
    """Aggregate offline metadata quality state."""

    VALID = "VALID"
    DEGRADED = "DEGRADED"
    INVALID = "INVALID"


class AdvisoryEventType(StrEnum):
    """Rules-only infrastructure event labels."""

    EARNINGS_RELEASE = "EARNINGS_RELEASE"
    GUIDANCE_UPDATE = "GUIDANCE_UPDATE"
    MERGER_ACQUISITION = "MERGER_ACQUISITION"
    PRODUCT_RECALL = "PRODUCT_RECALL"
    REGULATORY_ACTION = "REGULATORY_ACTION"
    EXECUTIVE_CHANGE = "EXECUTIVE_CHANGE"
    FINANCING = "FINANCING"
    BANKRUPTCY = "BANKRUPTCY"
    LITIGATION = "LITIGATION"
    CYBERSECURITY_INCIDENT = "CYBERSECURITY_INCIDENT"
    SUPPLY_CHAIN_DISRUPTION = "SUPPLY_CHAIN_DISRUPTION"
    TRADING_HALT = "TRADING_HALT"
    RUMOR = "RUMOR"
    CORRECTION = "CORRECTION"


def _canonical_bytes(value: object) -> bytes:
    return json.dumps(
        value, ensure_ascii=False, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")


def _sha256(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _bounded_text(value: str, name: str, maximum: int = MAX_FIELD_BYTES) -> None:
    if (
        not value
        or len(value.encode("utf-8")) > maximum
        or _CONTROL.search(value) is not None
    ):
        raise GdeltError(
            GdeltErrorCode.UNSAFE_METADATA,
            f"{name} is empty, contains control bytes, or exceeds its bound",
        )


def _read_secure_json(path: Path, maximum: int) -> Mapping[str, object]:
    try:
        status = os.lstat(path)
        if not stat.S_ISREG(status.st_mode) or stat.S_ISLNK(status.st_mode):
            raise GdeltError(
                GdeltErrorCode.INVALID_CONFIGURATION,
                "configuration input is not a regular file",
            )
        if status.st_size <= 0 or status.st_size > maximum:
            raise GdeltError(
                GdeltErrorCode.RESPONSE_TOO_LARGE,
                "configuration input exceeds its bound",
            )
        value = json.loads(path.read_bytes())
    except (OSError, json.JSONDecodeError) as error:
        raise GdeltError(
            GdeltErrorCode.INVALID_CONFIGURATION,
            "configuration input is unreadable or malformed",
        ) from error
    if not isinstance(value, dict) or not all(isinstance(key, str) for key in value):
        raise GdeltError(
            GdeltErrorCode.INVALID_CONFIGURATION,
            "configuration input must be a JSON object",
        )
    return cast("Mapping[str, object]", value)


def _verify_self_hash(document: Mapping[str, object], field: str) -> None:
    claimed = document.get(field)
    if not isinstance(claimed, str) or _SHA256.fullmatch(claimed) is None:
        raise GdeltError(
            GdeltErrorCode.INVALID_CONFIGURATION, "document self-hash is missing"
        )
    body = dict(document)
    del body[field]
    if _sha256(_canonical_bytes(body)) != claimed:
        raise GdeltError(
            GdeltErrorCode.INVALID_CONFIGURATION, "document self-hash is invalid"
        )


def _utc_ns(value: str, name: str) -> int:
    try:
        parsed = datetime.strptime(value, "%Y%m%d%H%M%S").replace(tzinfo=UTC)
    except ValueError as error:
        raise GdeltError(
            GdeltErrorCode.MALFORMED_RESPONSE, f"{name} is not GDELT UTC time"
        ) from error
    return int(parsed.timestamp()) * NANOSECONDS_PER_SECOND


def _instrument_id(value: str) -> InstrumentId:
    if re.fullmatch(r"[0-9a-f]{32}", value) is None:
        raise GdeltError(
            GdeltErrorCode.INVALID_CONFIGURATION, "instrument identity is malformed"
        )
    return InstrumentId(Identifier128(int(value[:16], 16), int(value[16:], 16)))


def normalize_entity_name(value: str) -> str:
    """Normalize one bounded organization name without interpreting it."""
    _bounded_text(value, "organization name", 256)
    normalized = unicodedata.normalize("NFKC", value).casefold()
    normalized = _QUALIFIER.sub(" ", normalized)
    normalized = _PUNCTUATION.sub(" ", normalized)
    return _SPACE.sub(" ", normalized).strip()


def _alias_candidates(value: str) -> tuple[str, ...]:
    full = normalize_entity_name(value)
    stripped = _SPACE.sub(" ", _LEGAL_SUFFIX.sub(" ", full)).strip()
    candidates = [full]
    if len(stripped) >= _MINIMUM_ALIAS_CHARACTERS and stripped != full:
        candidates.append(stripped)
    return tuple(dict.fromkeys(candidates))


def _issuer_aliases(value: str, ticker: str) -> tuple[str, ...]:
    return tuple(
        alias for alias in _alias_candidates(value) if alias != ticker.casefold()
    )


@dataclass(frozen=True, slots=True)
class GdeltConfig:
    """Immutable bounded two-year POC configuration."""

    start_date: date
    end_date: date
    row_limit: int = MAX_QUERY_ROWS
    alias_batch_size: int = MAX_ALIAS_BATCH
    storage_limit_bytes: int = MAX_GDELT_STORAGE_BYTES
    maximum_bytes_billed_per_query: int = 100 * DECIMAL_GB
    allowed_languages: frozenset[str] = frozenset({"eng", "und"})

    def __post_init__(self) -> None:
        """Reject open-ended, overlong, or unbounded configurations."""
        if self.end_date < self.start_date or (
            self.end_date - self.start_date > timedelta(days=731)
        ):
            raise GdeltError(
                GdeltErrorCode.INVALID_CONFIGURATION,
                "GDELT window must be ordered and no longer than two years",
            )
        if not 0 < self.row_limit <= MAX_QUERY_ROWS:
            raise GdeltError(
                GdeltErrorCode.INVALID_CONFIGURATION, "query row limit is invalid"
            )
        if not 0 < self.alias_batch_size <= MAX_ALIAS_BATCH:
            raise GdeltError(
                GdeltErrorCode.INVALID_CONFIGURATION, "alias batch size is invalid"
            )
        if not 0 < self.storage_limit_bytes <= MAX_GDELT_STORAGE_BYTES:
            raise GdeltError(
                GdeltErrorCode.INVALID_CONFIGURATION, "GDELT storage cap is invalid"
            )
        if not 0 < self.maximum_bytes_billed_per_query <= 1_000 * DECIMAL_GB:
            raise GdeltError(
                GdeltErrorCode.INVALID_CONFIGURATION, "query byte limit is invalid"
            )
        if not self.allowed_languages or any(
            _LANGUAGE.fullmatch(item) is None for item in self.allowed_languages
        ):
            raise GdeltError(
                GdeltErrorCode.INVALID_CONFIGURATION, "language allowlist is invalid"
            )


@dataclass(frozen=True, slots=True)
class GdeltAuthorization:
    """External accountable authorization for GDELT-owned metadata."""

    approval_id: str
    universe_snapshot_sha256: str
    valid_from: date
    valid_through: date
    expires_at_utc_ns: int
    metadata_storage_authorized: bool
    derived_data_authorized: bool
    publisher_full_text_authorized: bool
    bigquery_execution_authorized: bool
    storage_limit_bytes: int
    attribution: str
    approval_sha256: str

    @classmethod
    def load(cls, path: Path) -> Self:
        """Load an owner-only self-hashed approval without credentials."""
        document = _read_secure_json(path, MAX_APPROVAL_BYTES)
        if stat.S_IMODE(os.lstat(path).st_mode) & 0o077:
            raise GdeltError(
                GdeltErrorCode.AUTHORIZATION_DENIED,
                "GDELT approval must be owner-only",
            )
        required = {
            "approval_id",
            "approval_sha256",
            "attribution",
            "bigquery_execution_authorized",
            "contains_secrets",
            "dataset",
            "derived_data_authorized",
            "distribution",
            "expires_at_utc",
            "metadata_storage_authorized",
            "publisher_full_text_authorized",
            "schema_version",
            "source_id",
            "storage_limit_bytes_decimal",
            "universe_snapshot_sha256",
            "use_classification",
            "valid_from",
            "valid_through",
        }
        if set(document) != required:
            raise GdeltError(
                GdeltErrorCode.AUTHORIZATION_DENIED,
                "GDELT approval fields do not match the closed schema",
            )
        _verify_self_hash(document, "approval_sha256")
        if (
            document["schema_version"] != GDELT_SCHEMA_VERSION
            or document["source_id"] != "gdelt_2_metadata"
            or document["dataset"] != GDELT_TABLE
            or document["use_classification"] != "ACADEMIC_NON_COMMERCIAL"
            or document["distribution"] != "OWNER_ONLY_INTERNAL"
            or document["contains_secrets"] is not False
        ):
            raise GdeltError(
                GdeltErrorCode.AUTHORIZATION_DENIED,
                "GDELT approval identity or scope is invalid",
            )
        try:
            expires = datetime.fromisoformat(cast("str", document["expires_at_utc"]))
            valid_from = date.fromisoformat(cast("str", document["valid_from"]))
            valid_through = date.fromisoformat(cast("str", document["valid_through"]))
        except (TypeError, ValueError) as error:
            raise GdeltError(
                GdeltErrorCode.AUTHORIZATION_DENIED,
                "GDELT approval dates are malformed",
            ) from error
        if expires.tzinfo is None or expires.utcoffset() != timedelta(0):
            raise GdeltError(
                GdeltErrorCode.AUTHORIZATION_DENIED,
                "GDELT approval expiry must be UTC",
            )
        booleans = (
            document["metadata_storage_authorized"],
            document["derived_data_authorized"],
            document["publisher_full_text_authorized"],
            document["bigquery_execution_authorized"],
        )
        storage = document["storage_limit_bytes_decimal"]
        universe_hash = document["universe_snapshot_sha256"]
        if (
            any(not isinstance(item, bool) for item in booleans)
            or not isinstance(storage, int)
            or isinstance(storage, bool)
            or not isinstance(universe_hash, str)
            or _SHA256.fullmatch(universe_hash) is None
            or not isinstance(document["approval_id"], str)
            or not isinstance(document["attribution"], str)
            or not 0 < storage <= MAX_GDELT_STORAGE_BYTES
            or valid_through < valid_from
        ):
            raise GdeltError(
                GdeltErrorCode.AUTHORIZATION_DENIED,
                "GDELT approval values are malformed",
            )
        _bounded_text(document["approval_id"], "approval ID", 128)
        _bounded_text(document["attribution"], "attribution", 512)
        return cls(
            approval_id=document["approval_id"],
            universe_snapshot_sha256=universe_hash,
            valid_from=valid_from,
            valid_through=valid_through,
            expires_at_utc_ns=int(expires.timestamp()) * NANOSECONDS_PER_SECOND,
            metadata_storage_authorized=cast("bool", booleans[0]),
            derived_data_authorized=cast("bool", booleans[1]),
            publisher_full_text_authorized=cast("bool", booleans[2]),
            bigquery_execution_authorized=cast("bool", booleans[3]),
            storage_limit_bytes=storage,
            attribution=document["attribution"],
            approval_sha256=cast("str", document["approval_sha256"]),
        )

    def require(
        self,
        config: GdeltConfig,
        universe_sha256: str,
        *,
        now_utc_ns: int,
        network_access: bool,
    ) -> None:
        """Fail closed on expired, mismatched, broader, or full-text use."""
        if (
            now_utc_ns <= 0
            or now_utc_ns > self.expires_at_utc_ns
            or universe_sha256 != self.universe_snapshot_sha256
            or config.start_date < self.valid_from
            or config.end_date > self.valid_through
            or not self.metadata_storage_authorized
            or not self.derived_data_authorized
            or self.publisher_full_text_authorized
            or config.storage_limit_bytes > self.storage_limit_bytes
            or self.attribution != GDELT_ATTRIBUTION
            or (network_access and not self.bigquery_execution_authorized)
        ):
            raise GdeltError(
                GdeltErrorCode.AUTHORIZATION_DENIED,
                "GDELT authorization is absent, expired, mismatched, or too narrow",
            )


@dataclass(frozen=True, slots=True)
class IssuerIdentity:
    """Current-only issuer name bound to a stable POC instrument identity."""

    ticker: str
    instrument_id: InstrumentId
    canonical_name: str
    aliases: tuple[str, ...]
    provenance: str = "SEC_CURRENT_ASSOCIATION_ONLY"

    def __post_init__(self) -> None:
        """Validate bounded aliases while excluding ticker strings."""
        if _SYMBOL.fullmatch(self.ticker) is None or not self.aliases:
            raise GdeltError(
                GdeltErrorCode.INVALID_CONFIGURATION, "issuer identity is malformed"
            )
        expected = _issuer_aliases(self.canonical_name, self.ticker)
        if not expected or self.aliases != expected:
            raise GdeltError(
                GdeltErrorCode.INVALID_CONFIGURATION,
                "issuer aliases must be deterministic names, never ticker tokens",
            )


@dataclass(frozen=True, slots=True)
class EntityMatchIssue:
    """Privacy-bounded record of an ambiguous or rejected source entity."""

    normalized_name_sha256: str
    status: EntityMatchStatus
    candidate_tickers: tuple[str, ...] = ()


class GdeltEntityResolver:
    """Exact normalized issuer-name resolver with explicit ambiguity."""

    def __init__(
        self,
        identities: tuple[IssuerIdentity, ...],
        unresolved_tickers: tuple[str, ...],
    ) -> None:
        """Build an immutable alias index; ambiguous aliases remain ambiguous."""
        if not identities:
            raise GdeltError(
                GdeltErrorCode.INVALID_CONFIGURATION, "issuer registry is empty"
            )
        tickers = [item.ticker for item in identities]
        if len(set(tickers)) != len(tickers):
            raise GdeltError(
                GdeltErrorCode.INVALID_CONFIGURATION, "issuer ticker is duplicated"
            )
        index: dict[str, list[IssuerIdentity]] = {}
        for identity in identities:
            for alias in identity.aliases:
                index.setdefault(alias, []).append(identity)
        self.identities = identities
        self.unresolved_tickers = unresolved_tickers
        self._index = {
            alias: tuple(sorted(values, key=lambda item: item.ticker))
            for alias, values in index.items()
        }

    @property
    def query_aliases(self) -> tuple[str, ...]:
        """Return only unambiguous name aliases for parameterized queries."""
        return tuple(
            sorted(alias for alias, values in self._index.items() if len(values) == 1)
        )

    def resolve(
        self, organization_names: Sequence[str]
    ) -> tuple[tuple[IssuerIdentity, ...], tuple[EntityMatchIssue, ...]]:
        """Resolve exact names, retaining every ambiguity and rejection."""
        resolved: dict[str, IssuerIdentity] = {}
        issues: list[EntityMatchIssue] = []
        for source_name in organization_names:
            normalized = normalize_entity_name(source_name)
            if len(normalized) < _MINIMUM_ALIAS_CHARACTERS:
                issues.append(
                    EntityMatchIssue(
                        _sha256(normalized.encode("utf-8")),
                        EntityMatchStatus.REJECTED,
                        (),
                    )
                )
                continue
            digest = _sha256(normalized.encode("utf-8"))
            candidates = self._index.get(normalized, ())
            if len(candidates) == 1:
                resolved[candidates[0].ticker] = candidates[0]
            elif candidates:
                issues.append(
                    EntityMatchIssue(
                        digest,
                        EntityMatchStatus.AMBIGUOUS,
                        tuple(item.ticker for item in candidates),
                    )
                )
            else:
                issues.append(EntityMatchIssue(digest, EntityMatchStatus.REJECTED, ()))
        return (
            tuple(resolved[ticker] for ticker in sorted(resolved)),
            tuple(issues),
        )


def load_issuer_resolver(
    universe: UniverseSnapshot,
    reference_snapshot_path: Path = DEFAULT_REFERENCE_SNAPSHOT,
    sec_coverage_path: Path = DEFAULT_SEC_COVERAGE,
) -> GdeltEntityResolver:
    """Join current SEC issuer names to stable reference InstrumentIds."""
    reference = _read_secure_json(reference_snapshot_path, MAX_REFERENCE_BYTES)
    coverage = _read_secure_json(sec_coverage_path, MAX_REPORT_BYTES)
    _verify_self_hash(reference, "document_sha256")
    _verify_self_hash(coverage, "report_sha256")
    if (
        reference.get("universe_source_sha256") != universe.source_file_sha256.hex()
        or coverage.get("universe_sha256") != universe.universe_snapshot_sha256.hex()
    ):
        raise GdeltError(
            GdeltErrorCode.INVALID_CONFIGURATION,
            "reference or SEC evidence does not match ticker.txt",
        )
    raw_mappings = reference.get("mappings")
    raw_coverage = coverage.get("issuer_coverage")
    if not isinstance(raw_mappings, list) or not isinstance(raw_coverage, list):
        raise GdeltError(
            GdeltErrorCode.INVALID_CONFIGURATION,
            "reference or SEC evidence has malformed coverage",
        )
    instruments: dict[str, InstrumentId] = {}
    for raw in raw_mappings:
        if not isinstance(raw, dict):
            raise GdeltError(
                GdeltErrorCode.INVALID_CONFIGURATION, "reference mapping is malformed"
            )
        symbol = raw.get("symbol")
        identifier = raw.get("instrument_id")
        if isinstance(symbol, str) and isinstance(identifier, str):
            if symbol in instruments:
                raise GdeltError(
                    GdeltErrorCode.INVALID_CONFIGURATION,
                    "reference mapping contains a duplicate symbol",
                )
            instruments[symbol] = _instrument_id(identifier)
    issuer_names: dict[str, str] = {}
    for raw in raw_coverage:
        if not isinstance(raw, dict):
            raise GdeltError(
                GdeltErrorCode.INVALID_CONFIGURATION, "SEC coverage row is malformed"
            )
        ticker = raw.get("ticker")
        name = raw.get("issuer_name")
        if isinstance(ticker, str) and isinstance(name, str):
            if ticker in issuer_names:
                raise GdeltError(
                    GdeltErrorCode.INVALID_CONFIGURATION,
                    "SEC coverage contains a duplicate ticker",
                )
            issuer_names[ticker] = name
    identities = tuple(
        IssuerIdentity(
            ticker=symbol,
            instrument_id=instruments[symbol],
            canonical_name=issuer_names[symbol],
            aliases=_issuer_aliases(issuer_names[symbol], symbol),
        )
        for symbol in (entry.symbol for entry in universe.ordered_entries)
        if symbol in instruments and symbol in issuer_names
    )
    unresolved = tuple(
        symbol
        for symbol in (entry.symbol for entry in universe.ordered_entries)
        if symbol not in instruments or symbol not in issuer_names
    )
    return GdeltEntityResolver(identities, unresolved)


_QUERY = """  # noqa: S608 - fixed table; variable values are query parameters.
SELECT
  CAST(GKGRECORDID AS STRING) AS provider_record_id,
  CAST(DATE AS STRING) AS gdelt_record_time,
  CAST(SourceCollectionIdentifier AS STRING) AS source_collection_identifier,
  SourceCommonName AS source_common_name,
  DocumentIdentifier AS source_url,
  V2Themes AS themes,
  V2Organizations AS organizations,
  IFNULL(REGEXP_EXTRACT(TranslationInfo, r'srclc:(.*?);'), 'und') AS source_language
FROM `gdelt-bq.gdeltv2.gkg_partitioned`
WHERE DATE(_PARTITIONTIME) >= @start_date
  AND DATE(_PARTITIONTIME) < @end_date_exclusive
  AND SourceCollectionIdentifier = 1
  AND DocumentIdentifier IS NOT NULL
  AND EXISTS (
    SELECT 1
    FROM UNNEST(SPLIT(IFNULL(V2Organizations, ''), ';')) AS organization,
         UNNEST(@organization_aliases) AS allowed_alias
    WHERE TRIM(REGEXP_REPLACE(NORMALIZE_AND_CASEFOLD(
      REGEXP_REPLACE(organization, r',[0-9]+$', '')
    ), r'[^[:alnum:]]+', ' ')) = allowed_alias
  )
ORDER BY DATE, GKGRECORDID
LIMIT @row_limit_plus_one
""".strip()


@dataclass(frozen=True, slots=True)
class GdeltQueryPlan:
    """One bounded partition-pruned parameterized BigQuery request."""

    start_date: date
    end_date_exclusive: date
    organization_aliases: tuple[str, ...]
    row_limit: int
    maximum_bytes_billed: int
    sql: str = _QUERY

    def __post_init__(self) -> None:
        """Require a monthly-or-smaller partition and bounded parameters."""
        if (
            self.end_date_exclusive <= self.start_date
            or self.end_date_exclusive - self.start_date > timedelta(days=32)
            or not self.organization_aliases
            or len(self.organization_aliases) > MAX_ALIAS_BATCH
            or tuple(sorted(set(self.organization_aliases)))
            != self.organization_aliases
            or not 0 < self.row_limit <= MAX_QUERY_ROWS
            or self.maximum_bytes_billed <= 0
            or GDELT_TABLE not in self.sql
            or "_PARTITIONTIME" not in self.sql
        ):
            raise GdeltError(
                GdeltErrorCode.INVALID_CONFIGURATION, "GDELT query plan is unbounded"
            )

    @property
    def parameters(self) -> Mapping[str, object]:
        """Return typed parameters; no untrusted value is interpolated into SQL."""
        return {
            "end_date_exclusive": self.end_date_exclusive.isoformat(),
            "organization_aliases": list(self.organization_aliases),
            "row_limit_plus_one": self.row_limit + 1,
            "start_date": self.start_date.isoformat(),
        }

    @property
    def plan_id(self) -> str:
        """Return deterministic query-plan identity."""
        return "gdelt-query-" + _sha256(
            _canonical_bytes(
                {
                    "maximum_bytes_billed": self.maximum_bytes_billed,
                    "parameters": self.parameters,
                    "sql": self.sql,
                }
            )
        )


def build_query_plans(
    config: GdeltConfig, resolver: GdeltEntityResolver
) -> tuple[GdeltQueryPlan, ...]:
    """Create deterministic monthly and alias-batched query plans."""
    aliases = resolver.query_aliases
    if not aliases:
        raise GdeltError(
            GdeltErrorCode.INVALID_CONFIGURATION,
            "no unambiguous issuer aliases are queryable",
        )
    periods: list[tuple[date, date]] = []
    current = config.start_date
    final_exclusive = config.end_date + timedelta(days=1)
    while current < final_exclusive:
        if current.month == _DECEMBER:
            next_month = date(current.year + 1, 1, 1)
        else:
            next_month = date(current.year, current.month + 1, 1)
        end = min(next_month, final_exclusive)
        periods.append((current, end))
        current = end
    plans = tuple(
        GdeltQueryPlan(
            start,
            end,
            aliases[offset : offset + config.alias_batch_size],
            config.row_limit,
            config.maximum_bytes_billed_per_query,
        )
        for start, end in periods
        for offset in range(0, len(aliases), config.alias_batch_size)
    )
    if len(plans) > MAX_QUERY_PLANS:
        raise GdeltError(
            GdeltErrorCode.QUERY_LIMIT, "query plan count exceeds its bound"
        )
    return plans


@dataclass(frozen=True, slots=True)
class GdeltQueryResult:
    """One complete bounded query response returned by an injected client."""

    plan_id: str
    rows: tuple[Mapping[str, object], ...]
    receipt_time_utc_ns: int
    bytes_processed: int
    request_count: int
    network_access_performed: bool

    def __post_init__(self) -> None:
        """Validate result accounting before any row is interpreted."""
        if (
            not self.plan_id.startswith("gdelt-query-")
            or self.receipt_time_utc_ns <= 0
            or self.bytes_processed < 0
            or self.request_count <= 0
            or len(self.rows) > MAX_QUERY_ROWS + 1
        ):
            raise GdeltError(
                GdeltErrorCode.MALFORMED_RESPONSE, "query result metadata is malformed"
            )


class GdeltQueryClient(Protocol):
    """Replaceable query boundary; unit tests never require remote access."""

    @property
    def network_access(self) -> bool:
        """Return whether execution performs external network requests."""

    def execute(self, plan: GdeltQueryPlan) -> GdeltQueryResult:
        """Execute one exact bounded plan."""


@dataclass(slots=True)
class ScriptedGdeltQueryClient:
    """Deterministic mock/replay query client."""

    results: dict[str, GdeltQueryResult]
    network_access: bool = False
    calls: list[str] = field(default_factory=list)

    def execute(self, plan: GdeltQueryPlan) -> GdeltQueryResult:
        """Return a repeatable scripted result and record the call."""
        self.calls.append(plan.plan_id)
        result = self.results.get(plan.plan_id)
        if result is None:
            raise GdeltError(
                GdeltErrorCode.MALFORMED_RESPONSE, "scripted query result is absent"
            )
        return result


@dataclass(frozen=True, slots=True)
class GdeltHttpResponse:
    """Bounded HTTP response supplied by a production or mock transport."""

    status: int
    body: bytes
    received_at_utc_ns: int

    def __post_init__(self) -> None:
        """Reject invalid status, size, or receipt metadata."""
        if (
            not MIN_HTTP_STATUS <= self.status <= MAX_HTTP_STATUS
            or len(self.body) > MAX_HTTP_RESPONSE_BYTES
            or self.received_at_utc_ns <= 0
        ):
            raise GdeltError(
                GdeltErrorCode.RESPONSE_TOO_LARGE,
                "BigQuery HTTP response metadata is malformed",
            )


class GdeltTransport(Protocol):
    """Injectable fixed-host HTTPS POST boundary."""

    def post(
        self,
        url: str,
        headers: Mapping[str, str],
        body: bytes,
        timeout_seconds: float,
    ) -> GdeltHttpResponse:
        """Return one complete bounded response without following redirects."""


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


def _bigquery_url(project_id: str) -> str:
    if _PROJECT_ID.fullmatch(project_id) is None:
        raise GdeltError(
            GdeltErrorCode.INVALID_CONFIGURATION,
            "Google Cloud project ID is malformed",
        )
    return f"{BIGQUERY_QUERY_URL_PREFIX}{project_id}/queries"


class UrllibGdeltTransport:
    """Production fixed-host transport for BigQuery's read-only query call."""

    def __init__(self, *, wall_clock_ns: Callable[[], int] = time.time_ns) -> None:
        """Create a redirect-denying transport with injectable receipt time."""
        self._wall_clock_ns = wall_clock_ns
        self._opener = build_opener(_NoRedirect())

    def post(
        self,
        url: str,
        headers: Mapping[str, str],
        body: bytes,
        timeout_seconds: float,
    ) -> GdeltHttpResponse:
        """POST only to the exact BigQuery query endpoint."""
        split = urlsplit(url)
        if (
            split.scheme != "https"
            or split.hostname != "bigquery.googleapis.com"
            or split.port is not None
            or split.username is not None
            or split.password is not None
            or split.query
            or split.fragment
            or re.fullmatch(
                r"/bigquery/v2/projects/[a-z][a-z0-9-]{4,28}[a-z0-9]/queries",
                split.path,
            )
            is None
        ):
            raise GdeltError(
                GdeltErrorCode.INVALID_CONFIGURATION,
                "only the exact BigQuery HTTPS query endpoint is allowed",
            )
        if len(body) > MAX_FIELD_BYTES * MAX_ALIAS_BATCH:
            raise GdeltError(
                GdeltErrorCode.RESPONSE_TOO_LARGE,
                "BigQuery request exceeds its bound",
            )
        request = Request(  # noqa: S310 - exact HTTPS host/path validated above.
            url, data=body, headers=dict(headers), method="POST"
        )
        try:
            with self._opener.open(request, timeout=timeout_seconds) as response:
                response_body = response.read(MAX_HTTP_RESPONSE_BYTES + 1)
                return GdeltHttpResponse(
                    response.status,
                    response_body,
                    self._wall_clock_ns(),
                )
        except HTTPError as error:
            return GdeltHttpResponse(
                error.code,
                error.read(MAX_HTTP_RESPONSE_BYTES + 1),
                self._wall_clock_ns(),
            )
        except (TimeoutError, URLError, OSError) as error:
            raise GdeltError(
                GdeltErrorCode.REMOTE_FAILURE,
                "BigQuery HTTPS request failed",
            ) from error


def _query_body(plan: GdeltQueryPlan) -> bytes:
    parameters = (
        {
            "name": "start_date",
            "parameterType": {"type": "DATE"},
            "parameterValue": {"value": plan.start_date.isoformat()},
        },
        {
            "name": "end_date_exclusive",
            "parameterType": {"type": "DATE"},
            "parameterValue": {"value": plan.end_date_exclusive.isoformat()},
        },
        {
            "name": "organization_aliases",
            "parameterType": {
                "arrayType": {"type": "STRING"},
                "type": "ARRAY",
            },
            "parameterValue": {
                "arrayValues": [{"value": alias} for alias in plan.organization_aliases]
            },
        },
        {
            "name": "row_limit_plus_one",
            "parameterType": {"type": "INT64"},
            "parameterValue": {"value": str(plan.row_limit + 1)},
        },
    )
    return _canonical_bytes(
        {
            "maxResults": plan.row_limit + 1,
            "maximumBytesBilled": str(plan.maximum_bytes_billed),
            "parameterMode": "NAMED",
            "query": plan.sql,
            "queryParameters": list(parameters),
            "timeoutMs": 30_000,
            "useLegacySql": False,
        }
    )


_RESULT_COLUMNS: Final = (
    "provider_record_id",
    "gdelt_record_time",
    "source_collection_identifier",
    "source_common_name",
    "source_url",
    "themes",
    "organizations",
    "source_language",
)


def _decode_query_response(
    plan: GdeltQueryPlan, response: GdeltHttpResponse
) -> GdeltQueryResult:
    if response.status != HTTP_OK or len(response.body) > MAX_HTTP_RESPONSE_BYTES:
        raise GdeltError(
            GdeltErrorCode.REMOTE_FAILURE,
            f"BigQuery returned HTTP {response.status}",
        )
    try:
        document = json.loads(response.body)
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError) as error:
        raise GdeltError(
            GdeltErrorCode.MALFORMED_RESPONSE,
            "BigQuery response is malformed JSON",
        ) from error
    if not isinstance(document, dict):
        raise GdeltError(
            GdeltErrorCode.MALFORMED_RESPONSE,
            "BigQuery response is not an object",
        )
    if document.get("jobComplete") is not True or document.get("pageToken") is not None:
        raise GdeltError(
            GdeltErrorCode.QUERY_LIMIT,
            "BigQuery response is incomplete or paginated",
        )
    schema = document.get("schema")
    if not isinstance(schema, dict) or not isinstance(schema.get("fields"), list):
        raise GdeltError(
            GdeltErrorCode.MALFORMED_RESPONSE,
            "BigQuery response schema is missing",
        )
    fields = cast("list[object]", schema["fields"])
    names = tuple(
        field.get("name") if isinstance(field, dict) else None for field in fields
    )
    if names != _RESULT_COLUMNS:
        raise GdeltError(
            GdeltErrorCode.MALFORMED_RESPONSE,
            "BigQuery response schema is incompatible",
        )
    raw_rows = document.get("rows", [])
    if not isinstance(raw_rows, list) or len(raw_rows) > plan.row_limit:
        raise GdeltError(
            GdeltErrorCode.QUERY_LIMIT,
            "BigQuery response row count exceeds its bound",
        )
    rows: list[Mapping[str, object]] = []
    for raw_row in raw_rows:
        if not isinstance(raw_row, dict) or set(raw_row) != {"f"}:
            raise GdeltError(
                GdeltErrorCode.MALFORMED_RESPONSE,
                "BigQuery row is malformed",
            )
        cells = raw_row["f"]
        if not isinstance(cells, list) or len(cells) != len(_RESULT_COLUMNS):
            raise GdeltError(
                GdeltErrorCode.MALFORMED_RESPONSE,
                "BigQuery row width is incompatible",
            )
        values: list[object] = []
        for cell in cells:
            if not isinstance(cell, dict) or set(cell) != {"v"}:
                raise GdeltError(
                    GdeltErrorCode.MALFORMED_RESPONSE,
                    "BigQuery cell is malformed",
                )
            values.append(cell["v"])
        rows.append(dict(zip(_RESULT_COLUMNS, values, strict=True)))
    total = document.get("totalBytesProcessed", "0")
    try:
        bytes_processed = int(total)
    except (TypeError, ValueError) as error:
        raise GdeltError(
            GdeltErrorCode.MALFORMED_RESPONSE,
            "BigQuery byte accounting is malformed",
        ) from error
    return GdeltQueryResult(
        plan_id=plan.plan_id,
        rows=tuple(rows),
        receipt_time_utc_ns=response.received_at_utc_ns,
        bytes_processed=bytes_processed,
        request_count=1,
        network_access_performed=True,
    )


class BigQueryGdeltClient:
    """Bounded GDELT query client using Google's documented jobs.query API."""

    network_access = True

    def __init__(
        self,
        project_id: str,
        access_token: SecretValue,
        transport: GdeltTransport,
    ) -> None:
        """Bind an external Google project and non-printing access token."""
        self._url = _bigquery_url(project_id)
        self._access_token = access_token
        self._transport = transport

    def execute(self, plan: GdeltQueryPlan) -> GdeltQueryResult:
        """Execute exactly one parameterized, partition-pruned query."""
        response = self._transport.post(
            self._url,
            {
                "Accept": "application/json",
                "Authorization": f"Bearer {self._access_token.reveal()}",
                "Content-Type": "application/json; charset=utf-8",
            },
            _query_body(plan),
            35.0,
        )
        return _decode_query_response(plan, response)


def _parse_list(value: object, name: str, maximum: int) -> tuple[str, ...]:
    if not isinstance(value, str) or len(value.encode("utf-8")) > MAX_FIELD_BYTES * 4:
        raise GdeltError(
            GdeltErrorCode.RESPONSE_TOO_LARGE, f"{name} field is malformed or oversized"
        )
    values = tuple(item.strip() for item in value.split(";") if item.strip())
    if len(values) > maximum:
        raise GdeltError(
            GdeltErrorCode.RESPONSE_TOO_LARGE, f"{name} count exceeds its bound"
        )
    return values


def _organizations(value: object) -> tuple[str, ...]:
    organizations = []
    for item in _parse_list(value, "organizations", MAX_ORGANIZATIONS):
        match = _OFFSET.fullmatch(item)
        name = match.group(1) if match is not None else item
        _bounded_text(name, "organization", 256)
        organizations.append(name)
    if not organizations:
        raise GdeltError(
            GdeltErrorCode.ENTITY_UNRESOLVED, "GDELT row has no organization metadata"
        )
    return tuple(organizations)


def _themes(value: object) -> tuple[str, ...]:
    parsed: list[str] = []
    for item in _parse_list(value, "themes", MAX_THEMES):
        token = item.partition(",")[0].strip().upper()
        if _THEME.fullmatch(token) is None:
            raise GdeltError(GdeltErrorCode.UNSAFE_METADATA, "GDELT theme is malformed")
        parsed.append(token)
    return tuple(sorted(set(parsed)))


def normalize_source_url(value: object) -> str:
    """Validate and normalize an untrusted URL without fetching it."""
    if not isinstance(value, str) or not value:
        raise GdeltError(GdeltErrorCode.MISSING_SOURCE_URL, "source URL is missing")
    _bounded_text(value, "source URL", MAX_URL_BYTES)
    split = urlsplit(value)
    if (
        split.scheme.casefold() not in {"http", "https"}
        or split.hostname is None
        or split.username is not None
        or split.password is not None
        or split.hostname.encode("ascii", "ignore").decode("ascii") != split.hostname
    ):
        raise GdeltError(
            GdeltErrorCode.MISSING_SOURCE_URL, "source URL is unsafe or unsupported"
        )
    try:
        port = split.port
    except ValueError as error:
        raise GdeltError(
            GdeltErrorCode.MISSING_SOURCE_URL, "source URL port is malformed"
        ) from error
    hostname = split.hostname.casefold()
    netloc = hostname if port is None else f"{hostname}:{port}"
    normalized = SplitResult(split.scheme.casefold(), netloc, split.path or "/", "", "")
    return urlunsplit(normalized)


@dataclass(frozen=True, slots=True)
class GdeltMetadataRecord:
    """Canonical metadata-only record with explicit temporal uncertainty."""

    provider_record_id: str
    provider_observation_time_utc_ns: int
    publication_date: None
    publication_time_utc_ns: None
    receipt_time_utc_ns: int
    processing_time_utc_ns: int
    source_common_name: str
    source_url: str
    source_url_sha256: str
    source_language: str
    themes: tuple[str, ...]
    organizations: tuple[str, ...]
    resolved_issuers: tuple[IssuerIdentity, ...]
    event_type: AdvisoryEventType | None
    advisory_event_id: str | None
    uncertainty_ppm: int
    contradicts_provider_ids: tuple[str, ...]
    content_sha256: str

    def to_dict(self) -> dict[str, object]:
        """Return the closed metadata record without publisher full text."""
        return {
            "advisory_event_id": self.advisory_event_id,
            "advisory_only": True,
            "attribution": GDELT_ATTRIBUTION,
            "content_sha256": self.content_sha256,
            "contradicts_provider_ids": list(self.contradicts_provider_ids),
            "event_type": self.event_type.value if self.event_type else None,
            "provider_observation_time_semantics": "GDELT_GKG_DATE",
            "provider_observation_time_utc_ns": self.provider_observation_time_utc_ns,
            "instrument_ids": [
                item.instrument_id.hex() for item in self.resolved_issuers
            ],
            "live_trading_capable": False,
            "organizations": list(self.organizations),
            "processing_time_utc_ns": self.processing_time_utc_ns,
            "provider_record_id": self.provider_record_id,
            "publication_date": self.publication_date,
            "publication_time_reason": "PRECISE_TIME_NOT_ASSERTED_BY_ADAPTER",
            "publication_time_utc_ns": self.publication_time_utc_ns,
            "publisher_content_fetched": False,
            "publisher_full_text_stored": False,
            "receipt_time_utc_ns": self.receipt_time_utc_ns,
            "schema_version": GDELT_SCHEMA_VERSION,
            "source_common_name": self.source_common_name,
            "source_language": self.source_language,
            "source_url": self.source_url,
            "source_url_sha256": self.source_url_sha256,
            "themes": list(self.themes),
            "tickers": [item.ticker for item in self.resolved_issuers],
            "uncertainty_ppm": self.uncertainty_ppm,
        }


_EVENT_THEMES: Final = (
    (AdvisoryEventType.CORRECTION, ("CORRECTION",)),
    (AdvisoryEventType.RUMOR, ("RUMOR",)),
    (AdvisoryEventType.TRADING_HALT, ("TRADING_HALT", "ECON_STOCKMARKET_HALT")),
    (AdvisoryEventType.BANKRUPTCY, ("BANKRUPTCY", "ECON_BANKRUPTCY")),
    (AdvisoryEventType.CYBERSECURITY_INCIDENT, ("CYBER_ATTACK", "CYBERSECURITY")),
    (AdvisoryEventType.MERGER_ACQUISITION, ("MERGER", "ACQUISITION")),
    (AdvisoryEventType.PRODUCT_RECALL, ("PRODUCT_RECALL",)),
    (AdvisoryEventType.REGULATORY_ACTION, ("REGULATORY", "SANCTIONS")),
    (AdvisoryEventType.EXECUTIVE_CHANGE, ("EXECUTIVE_CHANGE",)),
    (AdvisoryEventType.GUIDANCE_UPDATE, ("GUIDANCE",)),
    (AdvisoryEventType.EARNINGS_RELEASE, ("EARNINGS", "QUARTERLY_RESULTS")),
    (AdvisoryEventType.FINANCING, ("FINANCING", "DEBT_OFFERING")),
    (AdvisoryEventType.LITIGATION, ("LITIGATION", "LAWSUIT")),
    (AdvisoryEventType.SUPPLY_CHAIN_DISRUPTION, ("SUPPLY_CHAIN", "SHORTAGE")),
)


class GdeltFastClassifier:
    """Deterministic theme-only classifier for infrastructure validation."""

    def classify(self, themes: Sequence[str]) -> AdvisoryEventType | None:
        """Return the first ordered theme rule; never infer an order action."""
        for event_type, markers in _EVENT_THEMES:
            if any(marker in theme for marker in markers for theme in themes):
                return event_type
        return None


def parse_metadata_row(
    row: Mapping[str, object],
    *,
    receipt_time_utc_ns: int,
    processing_time_utc_ns: int,
    config: GdeltConfig,
    resolver: GdeltEntityResolver,
    classifier: GdeltFastClassifier,
) -> tuple[GdeltMetadataRecord, tuple[EntityMatchIssue, ...]]:
    """Parse, constrain, resolve, and classify one metadata-only row."""
    expected = {
        "gdelt_record_time",
        "organizations",
        "provider_record_id",
        "source_collection_identifier",
        "source_common_name",
        "source_language",
        "source_url",
        "themes",
    }
    if set(row) != expected or len(_canonical_bytes(row)) > MAX_RECORD_BYTES:
        raise GdeltError(
            GdeltErrorCode.RESPONSE_TOO_LARGE,
            "GDELT row fields are malformed or oversized",
        )
    provider_id = row["provider_record_id"]
    common_name = row["source_common_name"]
    language = row["source_language"]
    collection = row["source_collection_identifier"]
    record_time = row["gdelt_record_time"]
    if not isinstance(provider_id, str) or not isinstance(common_name, str):
        raise GdeltError(
            GdeltErrorCode.MALFORMED_RESPONSE, "GDELT identifiers are malformed"
        )
    _bounded_text(provider_id, "provider record ID", 128)
    _bounded_text(common_name, "source common name", 256)
    if collection not in {1, "1"}:
        raise GdeltError(
            GdeltErrorCode.UNSUPPORTED_SOURCE,
            "only GDELT web-source metadata is accepted",
        )
    if not isinstance(language, str) or _LANGUAGE.fullmatch(language) is None:
        raise GdeltError(
            GdeltErrorCode.UNSUPPORTED_LANGUAGE, "source language is malformed"
        )
    if language not in config.allowed_languages:
        raise GdeltError(
            GdeltErrorCode.UNSUPPORTED_LANGUAGE, "source language is unsupported"
        )
    if not isinstance(record_time, str) or _GDELT_TIME.fullmatch(record_time) is None:
        raise GdeltError(
            GdeltErrorCode.MALFORMED_RESPONSE, "GDELT record time is malformed"
        )
    record_time_ns = _utc_ns(record_time, "GDELT record time")
    record_date = date.fromisoformat(
        f"{record_time[:4]}-{record_time[4:6]}-{record_time[6:8]}"
    )
    if (
        record_date < config.start_date
        or record_date > config.end_date
        or receipt_time_utc_ns < record_time_ns
        or processing_time_utc_ns < receipt_time_utc_ns
    ):
        raise GdeltError(
            GdeltErrorCode.TIMESTAMP_DISORDER, "GDELT local timestamps are disordered"
        )
    source_url = normalize_source_url(row["source_url"])
    organizations = _organizations(row["organizations"])
    themes = _themes(row["themes"])
    untrusted_text = "\n".join((common_name, source_url, *organizations, *themes))
    if contains_prompt_injection(untrusted_text) or any(
        marker in untrusted_text.casefold()
        for marker in ("<script", "<iframe", "javascript:", "data:text/html")
    ):
        raise GdeltError(
            GdeltErrorCode.UNSAFE_METADATA,
            "GDELT metadata contains active or instruction-like content",
        )
    resolved, issues = resolver.resolve(organizations)
    if not resolved:
        code = (
            GdeltErrorCode.ENTITY_AMBIGUOUS
            if any(item.status is EntityMatchStatus.AMBIGUOUS for item in issues)
            else GdeltErrorCode.ENTITY_UNRESOLVED
        )
        raise GdeltError(code, "GDELT metadata has no unambiguous POC issuer")
    event_type = classifier.classify(themes)
    content = {
        "gdelt_record_time": record_time,
        "organizations": organizations,
        "source_common_name": common_name,
        "source_language": language,
        "source_url": source_url,
        "themes": themes,
    }
    content_hash = _sha256(_canonical_bytes(content))
    advisory_id = (
        "gdelt-advisory-"
        + _sha256(
            _canonical_bytes(
                {
                    "content_sha256": content_hash,
                    "event_type": event_type.value,
                    "instruments": [item.instrument_id.hex() for item in resolved],
                }
            )
        )
        if event_type is not None
        else None
    )
    return (
        GdeltMetadataRecord(
            provider_record_id=provider_id,
            provider_observation_time_utc_ns=record_time_ns,
            publication_date=None,
            publication_time_utc_ns=None,
            receipt_time_utc_ns=receipt_time_utc_ns,
            processing_time_utc_ns=processing_time_utc_ns,
            source_common_name=common_name,
            source_url=source_url,
            source_url_sha256=_sha256(source_url.encode("utf-8")),
            source_language=language,
            themes=themes,
            organizations=tuple(normalize_entity_name(item) for item in organizations),
            resolved_issuers=resolved,
            event_type=event_type,
            advisory_event_id=advisory_id,
            uncertainty_ppm=850_000,
            contradicts_provider_ids=(),
            content_sha256=content_hash,
        ),
        issues,
    )


class _Deduplicator:
    def __init__(self) -> None:
        self._provider: dict[str, str] = {}
        self._content: dict[str, str] = {}
        self._url: dict[str, tuple[str, str]] = {}

    def apply(
        self, record: GdeltMetadataRecord
    ) -> tuple[GdeltMetadataRecord | None, GdeltErrorCode | None]:
        prior_content = self._provider.get(record.provider_record_id)
        if prior_content is not None:
            return (
                None,
                GdeltErrorCode.DUPLICATE_PROVIDER_ID
                if prior_content == record.content_sha256
                else GdeltErrorCode.PROVIDER_ID_CONFLICT,
            )
        if record.content_sha256 in self._content:
            return None, GdeltErrorCode.DUPLICATE_CONTENT
        contradiction: tuple[str, ...] = ()
        prior_url = self._url.get(record.source_url_sha256)
        if prior_url is not None and prior_url[0] != record.content_sha256:
            contradiction = (prior_url[1],)
        self._provider[record.provider_record_id] = record.content_sha256
        self._content[record.content_sha256] = record.provider_record_id
        self._url[record.source_url_sha256] = (
            record.content_sha256,
            record.provider_record_id,
        )
        if contradiction:
            record = replace(record, contradicts_provider_ids=contradiction)
        return record, None


@dataclass(frozen=True, slots=True)
class GdeltArtifactManifest:
    """Immutable metadata partition manifest."""

    plan_id: str
    storage_path: str
    object_sha256: str
    size_bytes: int
    record_count: int
    event_time_min_ns: int
    event_time_max_ns: int
    receipt_time_utc_ns: int
    processing_time_utc_ns: int
    universe_sha256: str

    def payload(self) -> dict[str, object]:
        """Return canonical manifest fields."""
        return {
            "attribution": GDELT_ATTRIBUTION,
            "event_time_max_ns": self.event_time_max_ns,
            "event_time_min_ns": self.event_time_min_ns,
            "object_sha256": self.object_sha256,
            "plan_id": self.plan_id,
            "processing_time_utc_ns": self.processing_time_utc_ns,
            "publication_time_reason": "PRECISE_TIME_NOT_ASSERTED_BY_ADAPTER",
            "receipt_time_utc_ns": self.receipt_time_utc_ns,
            "record_count": self.record_count,
            "schema_name": "gdelt-metadata-record",
            "schema_version": GDELT_SCHEMA_VERSION,
            "size_bytes_decimal": self.size_bytes,
            "source_revision": GDELT_SOURCE_REVISION,
            "storage_path": self.storage_path,
            "universe_snapshot_sha256": self.universe_sha256,
        }

    @property
    def manifest_id(self) -> str:
        """Return the content-derived manifest identity."""
        return "gdelt-" + _sha256(_canonical_bytes(self.payload()))

    def encode(self) -> bytes:
        """Encode a closed self-hashed manifest."""
        body = self.payload()
        identified = {**body, "manifest_id": self.manifest_id}
        return (
            _canonical_bytes(
                {
                    **identified,
                    "manifest_sha256": _sha256(_canonical_bytes(identified)),
                }
            )
            + b"\n"
        )


def _write_all(descriptor: int, payload: bytes) -> None:
    offset = 0
    while offset < len(payload):
        written = os.write(descriptor, payload[offset:])
        if written <= 0:
            raise GdeltError(GdeltErrorCode.STORAGE_LIMIT, "GDELT write was partial")
        offset += written


class GdeltRepository:
    """Single-writer immutable publication under global and 5 GB caps."""

    def __init__(
        self,
        repository: DataRepository,
        quota: QuotaEvidence,
        *,
        universe_sha256: str,
        storage_limit_bytes: int = MAX_GDELT_STORAGE_BYTES,
    ) -> None:
        """Bind storage policy and authoritative quota evidence."""
        if _SHA256.fullmatch(universe_sha256) is None or not (
            0 < storage_limit_bytes <= MAX_GDELT_STORAGE_BYTES
        ):
            raise GdeltError(
                GdeltErrorCode.INVALID_CONFIGURATION, "GDELT repository is invalid"
            )
        self.repository = repository
        self.quota = quota
        self.universe_sha256 = universe_sha256
        self.storage_limit_bytes = storage_limit_bytes
        self._lock = threading.Lock()
        self._batch_lock = threading.Lock()
        self._batch_lease: AdmissionLease | None = None

    def usage_bytes(self) -> int:
        """Count every GDELT object, manifest, and report without symlinks."""
        roots = (
            *(
                self.repository.root / area / "gdelt"
                for area in (
                    "raw",
                    "canonical",
                    "derived",
                    "datasets",
                    "quarantine",
                    "reports",
                )
            ),
            self.repository.root / "manifests/gdelt",
        )
        total = 0

        def count_file(path: Path) -> None:
            nonlocal total
            status = os.lstat(path)
            if not stat.S_ISREG(status.st_mode) or stat.S_ISLNK(status.st_mode):
                raise GdeltError(
                    GdeltErrorCode.STORAGE_LIMIT,
                    "GDELT tree contains an unsafe object",
                )
            total += status.st_size
            if total > self.storage_limit_bytes:
                raise GdeltError(
                    GdeltErrorCode.STORAGE_LIMIT,
                    "GDELT storage already exceeds its cap",
                )

        for root in roots:
            if not root.exists():
                continue
            if root.is_symlink():
                raise GdeltError(
                    GdeltErrorCode.STORAGE_LIMIT, "GDELT root is a symlink"
                )
            for directory, names, files in os.walk(root, followlinks=False):
                if any((Path(directory) / name).is_symlink() for name in names):
                    raise GdeltError(
                        GdeltErrorCode.STORAGE_LIMIT, "GDELT tree contains a symlink"
                    )
                for filename in files:
                    count_file(Path(directory) / filename)
        temporary = self.repository.root / "tmp"
        for path in temporary.iterdir():
            if path.name.startswith("gdelt-"):
                count_file(path)
        return total

    @contextmanager
    def publication_batch(self) -> Iterator[None]:
        """Reserve remaining GDELT capacity once for a bounded run."""
        with self._batch_lock:
            remaining = self.storage_limit_bytes - self.usage_bytes()
            if remaining <= 0:
                raise GdeltError(
                    GdeltErrorCode.STORAGE_LIMIT, "GDELT storage cap is exhausted"
                )
            request = StorageRequest(
                "gdelt-bounded-publication-batch",
                remaining,
                MAX_RECORD_BYTES,
            )
            with self.repository.acquire_admission(request, self.quota) as lease:
                self._batch_lease = lease
                try:
                    yield
                finally:
                    self._batch_lease = None

    def publish(
        self,
        plan: GdeltQueryPlan,
        records: Sequence[GdeltMetadataRecord],
        *,
        receipt_time_utc_ns: int,
        processing_time_utc_ns: int,
    ) -> GdeltArtifactManifest | None:
        """Publish one nonempty canonical metadata partition atomically."""
        if not records:
            return None
        with self._lock:
            payload = b"".join(
                _canonical_bytes(item.to_dict()) + b"\n" for item in records
            )
            current = self.usage_bytes()
            if (
                len(payload) + MANIFEST_BUDGET_BYTES
                > self.storage_limit_bytes - current
            ):
                raise GdeltError(
                    GdeltErrorCode.STORAGE_LIMIT,
                    "GDELT partition would exceed the 5 GB cap",
                )
            digest = _sha256(payload)
            final_relative = f"raw/gdelt/query-results/{digest}.jsonl"
            staged_relative = f"tmp/gdelt-{digest}.partial"
            request = StorageRequest(
                f"gdelt-{digest[:24]}",
                len(payload) + MANIFEST_BUDGET_BYTES,
                len(payload),
            )
            if self._batch_lease is not None:
                return self._publish_admitted(
                    plan,
                    records,
                    payload,
                    digest,
                    staged_relative,
                    final_relative,
                    receipt_time_utc_ns,
                    processing_time_utc_ns,
                    self._batch_lease,
                )
            with self.repository.acquire_admission(request, self.quota) as lease:
                return self._publish_admitted(
                    plan,
                    records,
                    payload,
                    digest,
                    staged_relative,
                    final_relative,
                    receipt_time_utc_ns,
                    processing_time_utc_ns,
                    lease,
                )

    def _publish_admitted(
        self,
        plan: GdeltQueryPlan,
        records: Sequence[GdeltMetadataRecord],
        payload: bytes,
        digest: str,
        staged_relative: str,
        final_relative: str,
        receipt_time_utc_ns: int,
        processing_time_utc_ns: int,
        lease: AdmissionLease,
    ) -> GdeltArtifactManifest:
        staged = self.repository.root / staged_relative
        try:
            descriptor = os.open(
                staged,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
                0o600,
            )
        except FileExistsError:
            if staged.is_symlink() or staged.read_bytes() != payload:
                raise GdeltError(
                    GdeltErrorCode.STORAGE_LIMIT, "staged GDELT object conflicts"
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
        times = [item.provider_observation_time_utc_ns for item in records]
        manifest = GdeltArtifactManifest(
            plan.plan_id,
            final_relative,
            digest,
            len(payload),
            len(records),
            min(times),
            max(times),
            receipt_time_utc_ns,
            processing_time_utc_ns,
            self.universe_sha256,
        )
        self._publish_manifest(manifest)
        return manifest

    def _publish_manifest(self, manifest: GdeltArtifactManifest) -> None:
        encoded = manifest.encode()
        directory = self.repository.root / "manifests/gdelt"
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        final = directory / f"{manifest.manifest_id}.json"
        if final.exists():
            if final.is_symlink() or final.read_bytes() != encoded:
                raise GdeltError(
                    GdeltErrorCode.STORAGE_LIMIT,
                    "GDELT manifest conflicts with immutable evidence",
                )
            return
        staged = self.repository.root / f"tmp/{manifest.manifest_id}.partial"
        try:
            descriptor = os.open(
                staged,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
                0o600,
            )
        except FileExistsError:
            if staged.is_symlink() or staged.read_bytes() != encoded:
                raise GdeltError(
                    GdeltErrorCode.STORAGE_LIMIT,
                    "staged GDELT manifest conflicts",
                ) from None
        else:
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
                raise GdeltError(
                    GdeltErrorCode.STORAGE_LIMIT,
                    "GDELT manifest appeared concurrently",
                ) from None
            staged.unlink()

    def publish_reports(
        self, report: GdeltRunReport, directory: Path
    ) -> tuple[Path, Path]:
        """Publish bounded immutable machine and human reports under the cap."""
        expected = self.repository.root / "reports/gdelt/prompt-54"
        if directory != expected:
            raise GdeltError(
                GdeltErrorCode.STORAGE_LIMIT,
                "GDELT reports must stay in their fixed data-root area",
            )
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        machine = directory / f"{report.run_id}.json"
        human = directory / f"{report.run_id}.md"
        document = report.to_dict()
        payloads = (
            (machine, _canonical_bytes(document) + b"\n"),
            (human, _human_report_payload(report)),
        )
        byte_count = sum(len(payload) for _, payload in payloads)
        with self._lock:
            if self.usage_bytes() + byte_count > self.storage_limit_bytes:
                raise GdeltError(
                    GdeltErrorCode.STORAGE_LIMIT,
                    "GDELT reports would exceed the 5 GB cap",
                )
            request = StorageRequest(
                f"gdelt-report-{report.run_id[-24:]}",
                byte_count,
                byte_count,
            )
            with self.repository.acquire_admission(request, self.quota):
                for path, payload in payloads:
                    self._publish_report_file(path, payload)
        return machine, human

    def _publish_report_file(self, final: Path, payload: bytes) -> None:
        digest = _sha256(payload)
        staged = self.repository.root / f"tmp/gdelt-report-{digest}.partial"
        try:
            descriptor = os.open(
                staged,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
                0o600,
            )
        except FileExistsError:
            if staged.is_symlink() or staged.read_bytes() != payload:
                raise GdeltError(
                    GdeltErrorCode.STORAGE_LIMIT,
                    "staged GDELT report conflicts",
                ) from None
        else:
            try:
                _write_all(descriptor, payload)
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
        try:
            os.link(staged, final, follow_symlinks=False)
            staged.unlink()
        except FileExistsError:
            if final.is_symlink() or final.read_bytes() != payload:
                raise GdeltError(
                    GdeltErrorCode.STORAGE_LIMIT,
                    "immutable GDELT report conflicts",
                ) from None
            staged.unlink()


@dataclass(frozen=True, slots=True)
class GdeltRunReport:
    """Combined coverage, rejection, storage, and data-quality evidence."""

    run_id: str
    universe_sha256: str
    start_date: date
    end_date: date
    generated_at_utc_ns: int
    network_access_performed: bool
    query_plan_count: int
    request_count: int
    bytes_processed: int
    accepted_records: int
    advisory_records: int
    coverage_by_ticker: Mapping[str, int]
    unresolved_tickers: tuple[str, ...]
    rejection_counts: Mapping[str, int]
    rejection_examples: tuple[Mapping[str, str], ...]
    duplicate_provider_ids: int
    duplicate_content_keys: int
    provider_id_conflicts: int
    contradictory_records: int
    missing_precise_publication_time: int
    stored_bytes: int
    storage_usage_before: int
    storage_usage_after: int
    manifest_ids: tuple[str, ...]
    storage_cap_bytes: int

    def to_dict(self) -> dict[str, object]:
        """Encode the four requested report views with a self-hash."""
        quality_state = (
            DataQualityState.INVALID
            if self.provider_id_conflicts
            else (
                DataQualityState.DEGRADED
                if (
                    self.rejection_counts
                    or self.unresolved_tickers
                    or self.missing_precise_publication_time
                )
                else DataQualityState.VALID
            )
        )
        body: dict[str, object] = {
            "attribution": GDELT_ATTRIBUTION,
            "coverage": {
                "accepted_records": self.accepted_records,
                "advisory_records": self.advisory_records,
                "by_ticker": dict(sorted(self.coverage_by_ticker.items())),
                "unresolved_tickers": list(self.unresolved_tickers),
            },
            "data_quality": {
                "contradictory_records": self.contradictory_records,
                "duplicate_content_keys": self.duplicate_content_keys,
                "duplicate_provider_ids": self.duplicate_provider_ids,
                "missing_precise_publication_time": (
                    self.missing_precise_publication_time
                ),
                "provider_id_conflicts": self.provider_id_conflicts,
                "state": quality_state.value,
            },
            "end_date": self.end_date.isoformat(),
            "generated_at_utc_ns": self.generated_at_utc_ns,
            "live_trading_capable": False,
            "network_access_performed": self.network_access_performed,
            "query": {
                "bytes_processed": self.bytes_processed,
                "plan_count": self.query_plan_count,
                "request_count": self.request_count,
                "table": GDELT_TABLE,
            },
            "rejections": {
                "counts": dict(sorted(self.rejection_counts.items())),
                "examples": list(self.rejection_examples),
            },
            "run_id": self.run_id,
            "schema_version": GDELT_REPORT_SCHEMA_VERSION,
            "start_date": self.start_date.isoformat(),
            "storage": {
                "cap_bytes_decimal": self.storage_cap_bytes,
                "manifest_ids": list(self.manifest_ids),
                "stored_bytes_decimal": self.stored_bytes,
                "usage_after_bytes_decimal": self.storage_usage_after,
                "usage_before_bytes_decimal": self.storage_usage_before,
            },
            "universe_sha256": self.universe_sha256,
        }
        return {**body, "report_sha256": _sha256(_canonical_bytes(body))}


class GdeltAdapter:
    """Metadata-only GDELT coordinator with no trading dependency."""

    def __init__(
        self,
        config: GdeltConfig,
        universe: UniverseSnapshot,
        resolver: GdeltEntityResolver,
        authorization: GdeltAuthorization,
        *,
        repository: GdeltRepository | None = None,
        classifier: GdeltFastClassifier | None = None,
        wall_clock_ns: Callable[[], int] = time.time_ns,
    ) -> None:
        """Bind immutable dependencies and injectable time."""
        self.config = config
        self.universe = universe
        self.resolver = resolver
        self.authorization = authorization
        self.repository = repository
        self.classifier = classifier or GdeltFastClassifier()
        self._wall_clock_ns = wall_clock_ns

    def run(
        self, plans: Sequence[GdeltQueryPlan], client: GdeltQueryClient
    ) -> GdeltRunReport:
        """Execute bounded plans and publish only validated metadata."""
        now = self._wall_clock_ns()
        universe_hash = self.universe.universe_snapshot_sha256.hex()
        self.authorization.require(
            self.config,
            universe_hash,
            now_utc_ns=now,
            network_access=client.network_access,
        )
        expected_plans = build_query_plans(self.config, self.resolver)
        if tuple(plans) != expected_plans:
            raise GdeltError(
                GdeltErrorCode.INVALID_CONFIGURATION,
                "execution plans do not match the canonical bounded plan",
            )
        usage_before = self.repository.usage_bytes() if self.repository else 0
        deduplicator = _Deduplicator()
        coverage = dict.fromkeys(
            (entry.symbol for entry in self.universe.ordered_entries), 0
        )
        rejection_counts: dict[str, int] = {}
        rejection_examples: list[Mapping[str, str]] = []
        manifest_ids: list[str] = []
        request_count = 0
        bytes_processed = 0
        accepted_records = 0
        advisory_records = 0
        duplicate_provider_ids = 0
        duplicate_content_keys = 0
        provider_id_conflicts = 0
        contradictory_records = 0
        missing_publication = 0
        stored_bytes = 0
        accepted_content_hashes: list[str] = []

        def reject(code: GdeltErrorCode, row: Mapping[str, object]) -> None:
            rejection_counts[code.value] = rejection_counts.get(code.value, 0) + 1
            if len(rejection_examples) < MAX_REJECTION_EXAMPLES:
                rejection_examples.append(
                    {
                        "reason": code.value,
                        "row_sha256": _sha256(_canonical_bytes(row)),
                    }
                )

        context = (
            self.repository.publication_batch()
            if self.repository is not None
            else nullcontext()
        )
        with context:
            for plan in plans:
                result = client.execute(plan)
                if result.plan_id != plan.plan_id:
                    raise GdeltError(
                        GdeltErrorCode.MALFORMED_RESPONSE,
                        "query result provenance does not match its plan",
                    )
                if result.network_access_performed != client.network_access:
                    raise GdeltError(
                        GdeltErrorCode.MALFORMED_RESPONSE,
                        "query result network provenance is inconsistent",
                    )
                if result.bytes_processed > plan.maximum_bytes_billed:
                    raise GdeltError(
                        GdeltErrorCode.QUERY_LIMIT,
                        "query exceeded its processed-byte limit",
                    )
                if len(result.rows) > plan.row_limit:
                    raise GdeltError(
                        GdeltErrorCode.QUERY_LIMIT,
                        "query partition exceeded its row limit",
                    )
                request_count += result.request_count
                bytes_processed += result.bytes_processed
                partition_records: list[GdeltMetadataRecord] = []
                for row in result.rows:
                    processed = self._wall_clock_ns()
                    try:
                        record, issues = parse_metadata_row(
                            row,
                            receipt_time_utc_ns=result.receipt_time_utc_ns,
                            processing_time_utc_ns=processed,
                            config=self.config,
                            resolver=self.resolver,
                            classifier=self.classifier,
                        )
                    except GdeltError as error:
                        reject(error.code, row)
                        continue
                    for issue in issues:
                        rejection_counts[issue.status.value] = (
                            rejection_counts.get(issue.status.value, 0) + 1
                        )
                    candidate, duplicate = deduplicator.apply(record)
                    if duplicate is not None:
                        reject(duplicate, row)
                        if duplicate is GdeltErrorCode.DUPLICATE_PROVIDER_ID:
                            duplicate_provider_ids += 1
                        elif duplicate is GdeltErrorCode.DUPLICATE_CONTENT:
                            duplicate_content_keys += 1
                        else:
                            provider_id_conflicts += 1
                        continue
                    if candidate is None:
                        raise GdeltError(
                            GdeltErrorCode.MALFORMED_RESPONSE,
                            "deduplication returned no record or reason",
                        )
                    record = candidate
                    contradictory_records += len(record.contradicts_provider_ids)
                    missing_publication += record.publication_time_utc_ns is None
                    advisory_records += record.event_type is not None
                    accepted_records += 1
                    accepted_content_hashes.append(record.content_sha256)
                    for issuer in record.resolved_issuers:
                        coverage[issuer.ticker] += 1
                    partition_records.append(record)
                if self.repository is not None:
                    manifest = self.repository.publish(
                        plan,
                        partition_records,
                        receipt_time_utc_ns=result.receipt_time_utc_ns,
                        processing_time_utc_ns=(
                            partition_records[-1].processing_time_utc_ns
                            if partition_records
                            else self._wall_clock_ns()
                        ),
                    )
                    if manifest is not None:
                        manifest_ids.append(manifest.manifest_id)
                        stored_bytes += manifest.size_bytes
        usage_after = self.repository.usage_bytes() if self.repository else 0
        report_payload = {
            "accepted_content_hashes": accepted_content_hashes,
            "plans": [item.plan_id for item in plans],
            "rejections": dict(sorted(rejection_counts.items())),
            "universe": universe_hash,
        }
        run_id = "gdelt-run-" + _sha256(_canonical_bytes(report_payload))
        return GdeltRunReport(
            run_id=run_id,
            universe_sha256=universe_hash,
            start_date=self.config.start_date,
            end_date=self.config.end_date,
            generated_at_utc_ns=self._wall_clock_ns(),
            network_access_performed=client.network_access,
            query_plan_count=len(plans),
            request_count=request_count,
            bytes_processed=bytes_processed,
            accepted_records=accepted_records,
            advisory_records=advisory_records,
            coverage_by_ticker=coverage,
            unresolved_tickers=self.resolver.unresolved_tickers,
            rejection_counts=rejection_counts,
            rejection_examples=tuple(rejection_examples),
            duplicate_provider_ids=duplicate_provider_ids,
            duplicate_content_keys=duplicate_content_keys,
            provider_id_conflicts=provider_id_conflicts,
            contradictory_records=contradictory_records,
            missing_precise_publication_time=missing_publication,
            stored_bytes=stored_bytes,
            storage_usage_before=usage_before,
            storage_usage_after=usage_after,
            manifest_ids=tuple(manifest_ids),
            storage_cap_bytes=self.config.storage_limit_bytes,
        )


def _human_report_payload(report: GdeltRunReport) -> bytes:
    return (
        "# Bounded GDELT metadata ingestion report\n\n"
        f"- Run: `{report.run_id}`\n"
        f"- Window: `{report.start_date}` through `{report.end_date}`\n"
        f"- Accepted metadata records: {report.accepted_records}\n"
        f"- Advisory records: {report.advisory_records}\n"
        f"- Rejections: {sum(report.rejection_counts.values())}\n"
        f"- Stored bytes (decimal): {report.stored_bytes}\n"
        f"- Network access: `{str(report.network_access_performed).lower()}`\n"
        f"- Live trading capable: `false`\n"
        f"- Attribution: {GDELT_ATTRIBUTION}\n"
    ).encode()


def write_report(
    report: GdeltRunReport,
    repository: GdeltRepository,
    directory: Path | None = None,
) -> tuple[Path, Path]:
    """Publish reports through the same storage-admission boundary as data."""
    target = directory or repository.repository.root / "reports/gdelt/prompt-54"
    return repository.publish_reports(report, target)


def _plan_document(
    config: GdeltConfig,
    universe: UniverseSnapshot,
    resolver: GdeltEntityResolver,
    plans: Sequence[GdeltQueryPlan],
) -> dict[str, object]:
    body: dict[str, object] = {
        "attribution": GDELT_ATTRIBUTION,
        "bigquery_execution_authorized": False,
        "end_date": config.end_date.isoformat(),
        "issuer_count": len(resolver.identities),
        "live_trading_capable": False,
        "network_access_performed": False,
        "plan_count": len(plans),
        "plan_ids": [item.plan_id for item in plans],
        "query_table": GDELT_TABLE,
        "start_date": config.start_date.isoformat(),
        "universe_sha256": universe.universe_snapshot_sha256.hex(),
        "unresolved_tickers": list(resolver.unresolved_tickers),
    }
    return {**body, "plan_sha256": _sha256(_canonical_bytes(body))}


def cli_main(argv: Sequence[str] | None = None) -> int:
    """Plan by default; require explicit authorization for remote execution."""
    parser = argparse.ArgumentParser(prog="aegis-gdelt")
    parser.add_argument("command", choices=("plan", "ingest"))
    parser.add_argument("--start-date", required=True, type=date.fromisoformat)
    parser.add_argument("--end-date", required=True, type=date.fromisoformat)
    parser.add_argument("--ticker-file", type=Path, default=TICKER_UNIVERSE_PATH)
    parser.add_argument(
        "--reference-snapshot", type=Path, default=DEFAULT_REFERENCE_SNAPSHOT
    )
    parser.add_argument("--sec-coverage", type=Path, default=DEFAULT_SEC_COVERAGE)
    parser.add_argument("--approval", type=Path)
    parser.add_argument("--data-root", type=Path, default=DEFAULT_DATA_ROOT)
    parser.add_argument(
        "--google-project", default=os.environ.get("AEGIS_GCP_PROJECT_ID")
    )
    parser.add_argument("--quota-limit-bytes", type=int)
    parser.add_argument("--quota-used-bytes", type=int)
    parser.add_argument("--quota-source")
    parser.add_argument("--quota-observed-at-utc")
    parser.add_argument("--quota-authoritative", action="store_true")
    parser.add_argument("--execute", action="store_true")
    args = parser.parse_args(argv)
    if args.ticker_file != TICKER_UNIVERSE_PATH:
        raise GdeltError(
            GdeltErrorCode.INVALID_CONFIGURATION,
            "only the authoritative ticker.txt path is accepted",
        )
    universe = load_ticker_universe(UnresolvedInstrumentResolver())
    resolver = load_issuer_resolver(
        universe, args.reference_snapshot, args.sec_coverage
    )
    config = GdeltConfig(args.start_date, args.end_date)
    plans = build_query_plans(config, resolver)
    if args.command == "plan" or not args.execute:
        if args.command == "ingest" and not args.execute:
            raise GdeltError(
                GdeltErrorCode.AUTHORIZATION_DENIED,
                "remote ingestion requires explicit --execute",
            )
        sys.stdout.write(
            json.dumps(
                _plan_document(config, universe, resolver, plans), sort_keys=True
            )
            + "\n"
        )
        return 0
    if args.approval is None or args.google_project is None:
        raise GdeltError(
            GdeltErrorCode.AUTHORIZATION_DENIED,
            "execution requires an approval and Google Cloud project",
        )
    raw_token = os.environ.get("AEGIS_GCP_ACCESS_TOKEN")
    if raw_token is None:
        raise GdeltError(
            GdeltErrorCode.CREDENTIAL_UNAVAILABLE,
            "AEGIS_GCP_ACCESS_TOKEN is not configured",
        )
    quota = QuotaEvidence(
        limit_bytes=args.quota_limit_bytes,
        used_bytes=args.quota_used_bytes,
        source=args.quota_source,
        observed_at_utc=args.quota_observed_at_utc,
        authoritative=args.quota_authoritative,
    )
    if not quota.known:
        raise GdeltError(
            GdeltErrorCode.STORAGE_LIMIT,
            "execution requires complete authoritative user-quota evidence",
        )
    data_repository = DataRepository(args.data_root)
    data_repository.initialize()
    sink = GdeltRepository(
        data_repository,
        quota,
        universe_sha256=universe.universe_snapshot_sha256.hex(),
        storage_limit_bytes=config.storage_limit_bytes,
    )
    client = BigQueryGdeltClient(
        args.google_project,
        SecretValue(raw_token),
        UrllibGdeltTransport(),
    )
    report = GdeltAdapter(
        config,
        universe,
        resolver,
        GdeltAuthorization.load(args.approval),
        repository=sink,
    ).run(plans, client)
    machine, human = write_report(report, sink)
    sys.stdout.write(
        json.dumps(
            {
                "human_report": str(human),
                "machine_report": str(machine),
                "network_access_performed": True,
            },
            sort_keys=True,
        )
        + "\n"
    )
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(cli_main())
