"""Streaming, point-in-time-safe canonical minute normalization and storage."""

from __future__ import annotations

import hashlib
import json
import os
import sqlite3
import zlib
from contextlib import nullcontext
from dataclasses import dataclass
from datetime import UTC, date, datetime
from decimal import Decimal, InvalidOperation
from enum import StrEnum
from itertools import pairwise
from typing import TYPE_CHECKING, Final, NoReturn, Protocol, Self, cast
from zoneinfo import ZoneInfo

import pyarrow as pa  # type: ignore[import-untyped]
import pyarrow.parquet as pq  # type: ignore[import-untyped]

from aegis_mx_research.data_repository import (
    AdmissionLease,
    DataRepository,
    ManifestLineage,
    ManifestTimeRange,
    PartitionManifest,
    QuotaEvidence,
    SourceManifest,
    StorageRequest,
)
from aegis_mx_research.reference_data import (
    CalendarDayKind,
    MappingStatus,
    PointInTimeReferenceStore,
    ReferenceDataError,
    snapshot_document,
)

if TYPE_CHECKING:
    from collections.abc import Callable, Iterable, Iterator, Mapping, Sequence
    from pathlib import Path

CANONICAL_MINUTE_SCHEMA_VERSION: Final = "1.0.0"
CANONICAL_MINUTE_QUALITY_SCHEMA_VERSION: Final = "1.0.0"
NANOSECONDS_PER_MINUTE: Final = 60_000_000_000
CURRENCY_NANOS_PER_UNIT: Final = 1_000_000_000
MAX_INT64: Final = (1 << 63) - 1
DEFAULT_INSTRUMENT_BUCKETS: Final = 8
DEFAULT_BATCH_ROWS: Final = 65_536
DEFAULT_MAX_SOURCE_OBJECT_BYTES: Final = 64_000_000
DEFAULT_MAX_SPOOL_RECORDS: Final = 20_000_000
DEFAULT_MAX_OUTPUT_BYTES_PER_RECORD: Final = 512
MAX_INSTRUMENT_BUCKETS: Final = 256
SHA256_HEX_LENGTH: Final = 64
_MINIMUM_SPOOL_BYTES: Final = 65_536
_SPOOL_PAGE_SIZE: Final = 8_192
_SPOOL_RECORD_MAGIC: Final = b"CMZ1"
_MAX_SPOOL_RECORD_BYTES: Final = 65_536
_SESSION_ZONE: Final = ZoneInfo("America/New_York")


class MinuteNormalizationCode(StrEnum):
    """Stable fail-closed normalization reasons."""

    CONFLICTING_DUPLICATE = "CONFLICTING_DUPLICATE"
    CORRUPT_SPOOL = "CORRUPT_SPOOL"
    HASH_MISMATCH = "HASH_MISMATCH"
    INEXACT_PRICE = "INEXACT_PRICE"
    INVALID_PRICE = "INVALID_PRICE"
    INVALID_QUANTITY = "INVALID_QUANTITY"
    INVALID_SOURCE = "INVALID_SOURCE"
    MISSING_CALENDAR = "MISSING_CALENDAR"
    MISSING_REFERENCE = "MISSING_REFERENCE"
    MISSING_TICK_SIZE = "MISSING_TICK_SIZE"
    OUTSIDE_SESSION = "OUTSIDE_SESSION"
    RECORD_LIMIT = "RECORD_LIMIT"
    SOURCE_TOO_LARGE = "SOURCE_TOO_LARGE"
    TIMESTAMP_DISORDER = "TIMESTAMP_DISORDER"
    UNSUPPORTED_SCHEMA = "UNSUPPORTED_SCHEMA"


class MinuteNormalizationError(ValueError):
    """Bounded normalization failure retaining a machine-readable code."""

    def __init__(self, code: MinuteNormalizationCode, message: str) -> None:
        """Construct one safe error without embedding raw provider payloads."""
        self.code = code
        super().__init__(f"{code.value}: {message}"[:512])


class RawPriceEncoding(StrEnum):
    """Explicit representation carried by a raw minute source."""

    CURRENCY_UNITS_DECIMAL = "CURRENCY_UNITS_DECIMAL"
    RESOLVED_TICKS = "RESOLVED_TICKS"


class MinuteDataQuality(StrEnum):
    """Accepted canonical record quality state."""

    VALID = "VALID"
    DEGRADED = "DEGRADED"


def _canonical_bytes(value: object) -> bytes:
    return json.dumps(
        value, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode("ascii")


def _digest(value: object) -> str:
    return hashlib.sha256(_canonical_bytes(value)).hexdigest()


def _require_sha256(value: str, field: str) -> None:
    if (
        len(value) != SHA256_HEX_LENGTH
        or value.lower() != value
        or any(character not in "0123456789abcdef" for character in value)
        or not any(character != "0" for character in value)
    ):
        raise MinuteNormalizationError(
            MinuteNormalizationCode.INVALID_SOURCE, f"{field} is not a SHA-256"
        )


def _require_int64(value: int, field: str, *, allow_zero: bool = False) -> None:
    lower = 0 if allow_zero else 1
    if type(value) is not int or not lower <= value <= MAX_INT64:
        raise MinuteNormalizationError(
            MinuteNormalizationCode.INVALID_QUANTITY,
            f"{field} is outside the accepted int64 range",
        )


@dataclass(frozen=True, slots=True)
class TickSizeRevision:
    """One immutable, bitemporal price-grid revision."""

    revision_id: str
    symbol: str
    tick_value_currency_nanos: int
    currency: str
    effective_from_ns: int
    effective_to_ns: int | None
    available_at_ns: int
    version: int
    source_sha256: str
    historical_completeness: bool

    def __post_init__(self) -> None:
        """Validate exact units and half-open business effectiveness."""
        if not self.revision_id or not self.symbol or not self.currency:
            raise MinuteNormalizationError(
                MinuteNormalizationCode.MISSING_TICK_SIZE,
                "tick revision identity is incomplete",
            )
        _require_int64(self.tick_value_currency_nanos, "tick value")
        _require_int64(self.effective_from_ns, "tick effective_from_ns")
        _require_int64(self.available_at_ns, "tick available_at_ns")
        if self.effective_to_ns is not None:
            _require_int64(self.effective_to_ns, "tick effective_to_ns")
            if self.effective_to_ns <= self.effective_from_ns:
                raise MinuteNormalizationError(
                    MinuteNormalizationCode.MISSING_TICK_SIZE,
                    "tick validity interval is empty",
                )
        if self.version <= 0:
            raise MinuteNormalizationError(
                MinuteNormalizationCode.MISSING_TICK_SIZE,
                "tick revision version is invalid",
            )
        _require_sha256(self.source_sha256, "tick source_sha256")

    def effective_at(self, event_time_ns: int) -> bool:
        """Return whether this grid applies at an event time."""
        return self.effective_from_ns <= event_time_ns and (
            self.effective_to_ns is None or event_time_ns < self.effective_to_ns
        )


class TickSizeStore:
    """Bounded immutable price-grid index with conflict detection."""

    def __init__(self, revisions: Sequence[TickSizeRevision]) -> None:
        """Index caller-supplied revisions after deterministic lineage checks."""
        self._by_symbol: dict[str, list[TickSizeRevision]] = {}
        for revision in revisions:
            self._by_symbol.setdefault(revision.symbol, []).append(revision)
        for symbol, records in self._by_symbol.items():
            versions = sorted((item.version, item.available_at_ns) for item in records)
            if [item[0] for item in versions] != list(range(1, len(versions) + 1)):
                raise MinuteNormalizationError(
                    MinuteNormalizationCode.MISSING_TICK_SIZE,
                    f"tick revisions for {symbol} are not contiguous",
                )
            if any(
                current[1] <= previous[1] for previous, current in pairwise(versions)
            ):
                raise MinuteNormalizationError(
                    MinuteNormalizationCode.MISSING_TICK_SIZE,
                    f"tick revisions for {symbol} do not advance knowledge time",
                )
        self.sha256 = _digest(
            [
                {
                    "available_at_ns": item.available_at_ns,
                    "currency": item.currency,
                    "effective_from_ns": item.effective_from_ns,
                    "effective_to_ns": item.effective_to_ns,
                    "historical_completeness": item.historical_completeness,
                    "revision_id": item.revision_id,
                    "source_sha256": item.source_sha256,
                    "symbol": item.symbol,
                    "tick_value_currency_nanos": item.tick_value_currency_nanos,
                    "version": item.version,
                }
                for item in sorted(
                    revisions, key=lambda value: (value.symbol, value.version)
                )
            ]
        )

    def at(
        self, symbol: str, *, event_time_ns: int, known_at_ns: int
    ) -> TickSizeRevision:
        """Resolve exactly one known/effective grid or fail closed."""
        candidates = [
            item
            for item in self._by_symbol.get(symbol, ())
            if item.available_at_ns <= known_at_ns and item.effective_at(event_time_ns)
        ]
        if not candidates:
            raise MinuteNormalizationError(
                MinuteNormalizationCode.MISSING_TICK_SIZE,
                f"no known tick-size revision for {symbol}",
            )
        latest_version = max(item.version for item in candidates)
        latest = [item for item in candidates if item.version == latest_version]
        return min(latest, key=lambda item: item.revision_id)


@dataclass(frozen=True, slots=True)
class ResolvedMinuteReference:
    """All point-in-time reference facts bound to one raw minute."""

    instrument_id: str
    market_id: str
    mapping_id: str
    instrument_revision_id: str
    tick_revision_id: str
    tick_value_currency_nanos: int
    currency: str
    session_id: str
    trading_date: date
    session_open_ns: int
    session_close_ns: int
    corporate_action_ids: tuple[str, ...]
    reference_sha256: str
    quality_reasons: tuple[str, ...]


class MinuteReferenceResolver(Protocol):
    """Point-in-time lookup boundary used by the pure normalizer."""

    def resolve(
        self, symbol: str, *, event_time_ns: int, known_at_ns: int
    ) -> ResolvedMinuteReference:
        """Resolve all facts known at ``known_at_ns`` and effective at the event."""


class ReferenceStoreMinuteResolver:
    """Adapter over the Prompt 52 reference store and explicit tick revisions."""

    def __init__(
        self, reference_store: PointInTimeReferenceStore, tick_store: TickSizeStore
    ) -> None:
        """Bind immutable reference and price-grid snapshots."""
        self._reference = reference_store
        self._ticks = tick_store
        reference_document = snapshot_document(reference_store.snapshot)
        self.reference_sha256 = _digest(
            {
                "reference_snapshot_sha256": reference_document["document_sha256"],
                "tick_snapshot_sha256": tick_store.sha256,
            }
        )

    def resolve(
        self, symbol: str, *, event_time_ns: int, known_at_ns: int
    ) -> ResolvedMinuteReference:
        """Perform fail-closed bitemporal identity, grid, session, and halt lookup."""
        try:
            mapping = self._reference.symbol_mapping_at(
                symbol, effective_at_ns=event_time_ns, known_at_ns=known_at_ns
            )
        except ReferenceDataError as error:
            raise MinuteNormalizationError(
                MinuteNormalizationCode.MISSING_REFERENCE,
                "symbol mapping cannot be resolved without conflict",
            ) from error
        if (
            mapping is None
            or mapping.instrument_id is None
            or mapping.status
            not in {
                MappingStatus.RESOLVED_CURRENT_ONLY,
                MappingStatus.RECENTLY_LISTED,
                MappingStatus.DELISTED,
            }
        ):
            raise MinuteNormalizationError(
                MinuteNormalizationCode.MISSING_REFERENCE,
                f"symbol {symbol} has no eligible point-in-time mapping",
            )
        instrument = self._reference.instrument_at(
            mapping.instrument_id,
            effective_at_ns=event_time_ns,
            known_at_ns=known_at_ns,
        )
        if instrument is None:
            raise MinuteNormalizationError(
                MinuteNormalizationCode.MISSING_REFERENCE,
                f"symbol {symbol} has no point-in-time instrument revision",
            )
        tick = self._ticks.at(
            symbol, event_time_ns=event_time_ns, known_at_ns=known_at_ns
        )
        trading_date = (
            datetime.fromtimestamp(event_time_ns // CURRENCY_NANOS_PER_UNIT, tz=UTC)
            .astimezone(_SESSION_ZONE)
            .date()
        )
        try:
            calendar = self._reference.calendar_day_at(
                trading_date, known_at_ns=known_at_ns
            )
        except ReferenceDataError as error:
            raise MinuteNormalizationError(
                MinuteNormalizationCode.MISSING_CALENDAR,
                f"calendar is unavailable for {trading_date.isoformat()}",
            ) from error
        if (
            calendar.kind
            not in {
                CalendarDayKind.REGULAR_SESSION,
                CalendarDayKind.EARLY_CLOSE_SESSION,
            }
            or calendar.open_exchange_time_ns is None
            or calendar.close_exchange_time_ns is None
            or event_time_ns < calendar.open_exchange_time_ns
            or event_time_ns + NANOSECONDS_PER_MINUTE > calendar.close_exchange_time_ns
        ):
            raise MinuteNormalizationError(
                MinuteNormalizationCode.OUTSIDE_SESSION,
                "minute is outside the explicit regular session",
            )
        halts = self._reference.halts_at(
            mapping.instrument_id,
            start_exchange_time_ns=event_time_ns,
            end_exchange_time_ns=event_time_ns + NANOSECONDS_PER_MINUTE,
            known_at_ns=known_at_ns,
        )
        if halts:
            raise MinuteNormalizationError(
                MinuteNormalizationCode.OUTSIDE_SESSION,
                "minute intersects a known trading halt",
            )
        reasons: list[str] = []
        if not mapping.provenance.historical_completeness:
            reasons.append("MAPPING_HISTORY_INCOMPLETE")
        if not instrument.provenance.historical_completeness:
            reasons.append("INSTRUMENT_HISTORY_INCOMPLETE")
        if not calendar.provenance.historical_completeness:
            reasons.append("CALENDAR_HISTORY_INCOMPLETE")
        if not tick.historical_completeness:
            reasons.append("TICK_HISTORY_INCOMPLETE")
        if not self._reference.snapshot.halt_coverage_complete:
            reasons.append("HALT_HISTORY_INCOMPLETE")
        session_payload = {
            "calendar_id": calendar.calendar_id,
            "close_ns": calendar.close_exchange_time_ns,
            "date": trading_date.isoformat(),
            "open_ns": calendar.open_exchange_time_ns,
        }
        actions = self._reference.corporate_actions_at(
            mapping.instrument_id,
            effective_at_ns=event_time_ns,
            known_at_ns=known_at_ns,
        )
        return ResolvedMinuteReference(
            instrument_id=mapping.instrument_id.hex(),
            market_id=mapping.listing_venue.value,
            mapping_id=mapping.mapping_id,
            instrument_revision_id=instrument.revision_id,
            tick_revision_id=tick.revision_id,
            tick_value_currency_nanos=tick.tick_value_currency_nanos,
            currency=tick.currency,
            session_id=hashlib.sha256(_canonical_bytes(session_payload)).hexdigest()[
                :32
            ],
            trading_date=trading_date,
            session_open_ns=calendar.open_exchange_time_ns,
            session_close_ns=calendar.close_exchange_time_ns,
            corporate_action_ids=tuple(item.action_id for item in actions),
            reference_sha256=self.reference_sha256,
            quality_reasons=tuple(sorted(reasons)),
        )


@dataclass(frozen=True, slots=True)
class RawMinuteBar:
    """Provider-neutral raw bar with explicit provenance and price encoding."""

    symbol: str
    minute_start_exchange_time_ns: int
    open_value: Decimal | int
    high_value: Decimal | int
    low_value: Decimal | int
    close_value: Decimal | int
    volume: int
    trade_count: int | None
    vwap_value: Decimal | int | None
    price_encoding: RawPriceEncoding
    source_manifest_id: str
    source_object_id: str
    source_record_id: str
    source_publication_time_ns: int | None
    source_availability_time_ns: int
    local_receipt_time_ns: int
    local_processing_time_ns: int
    market_id: str | None = None


@dataclass(frozen=True, slots=True)
class CanonicalMinuteRecord:
    """Immutable canonical minute record with integer execution-safe units."""

    instrument_id: str
    source_ticker: str
    market_id: str
    session_id: str
    trading_date: date
    minute_start_exchange_time_ns: int
    minute_end_exchange_time_ns: int
    session_open_exchange_time_ns: int
    session_close_exchange_time_ns: int
    currency: str
    tick_value_currency_nanos: int
    open_ticks: int
    high_ticks: int
    low_ticks: int
    close_ticks: int
    volume_shares: int
    trade_count: int | None
    vwap_ticks: int | None
    source_publication_time_ns: int | None
    source_availability_time_ns: int
    local_receipt_time_ns: int
    local_processing_time_ns: int
    data_quality_state: MinuteDataQuality
    data_quality_reasons: tuple[str, ...]
    source_manifest_id: str
    source_object_id: str
    source_record_id: str
    mapping_id: str
    instrument_revision_id: str
    tick_revision_id: str
    corporate_action_ids: tuple[str, ...]
    reference_sha256: str
    record_sha256: str
    schema_version: str = CANONICAL_MINUTE_SCHEMA_VERSION

    def payload(self, *, include_hash: bool = True) -> dict[str, object]:
        """Return canonical JSON-compatible fields for hashing and persistence."""
        result: dict[str, object] = {
            "close_ticks": self.close_ticks,
            "corporate_action_ids": list(self.corporate_action_ids),
            "currency": self.currency,
            "data_quality_reasons": list(self.data_quality_reasons),
            "data_quality_state": self.data_quality_state.value,
            "high_ticks": self.high_ticks,
            "instrument_id": self.instrument_id,
            "instrument_revision_id": self.instrument_revision_id,
            "local_processing_time_ns": self.local_processing_time_ns,
            "local_receipt_time_ns": self.local_receipt_time_ns,
            "low_ticks": self.low_ticks,
            "mapping_id": self.mapping_id,
            "market_id": self.market_id,
            "minute_end_exchange_time_ns": self.minute_end_exchange_time_ns,
            "minute_start_exchange_time_ns": self.minute_start_exchange_time_ns,
            "open_ticks": self.open_ticks,
            "price_unit": "TICKS",
            "quantity_unit": "SHARES",
            "record_sha256": self.record_sha256 if include_hash else None,
            "reference_sha256": self.reference_sha256,
            "schema_version": self.schema_version,
            "session_close_exchange_time_ns": self.session_close_exchange_time_ns,
            "session_id": self.session_id,
            "session_open_exchange_time_ns": self.session_open_exchange_time_ns,
            "source_availability_time_ns": self.source_availability_time_ns,
            "source_manifest_id": self.source_manifest_id,
            "source_object_id": self.source_object_id,
            "source_publication_time_ns": self.source_publication_time_ns,
            "source_record_id": self.source_record_id,
            "source_ticker": self.source_ticker,
            "tick_revision_id": self.tick_revision_id,
            "tick_value_currency_nanos": self.tick_value_currency_nanos,
            "trade_count": self.trade_count,
            "trading_date": self.trading_date.isoformat(),
            "volume_shares": self.volume_shares,
            "vwap_ticks": self.vwap_ticks,
        }
        if not include_hash:
            result.pop("record_sha256")
        return result

    def economic_hash(self) -> str:
        """Hash values that must agree when two sources name the same minute."""
        return _digest(
            {
                "close_ticks": self.close_ticks,
                "high_ticks": self.high_ticks,
                "instrument_id": self.instrument_id,
                "low_ticks": self.low_ticks,
                "market_id": self.market_id,
                "minute_start_exchange_time_ns": self.minute_start_exchange_time_ns,
                "open_ticks": self.open_ticks,
                "tick_value_currency_nanos": self.tick_value_currency_nanos,
                "trade_count": self.trade_count,
                "volume_shares": self.volume_shares,
                "vwap_ticks": self.vwap_ticks,
            }
        )

    @classmethod
    def from_bytes(cls, encoded: bytes) -> CanonicalMinuteRecord:
        """Decode and verify one record from the bounded spool."""
        try:
            value = json.loads(encoded)
            if not isinstance(value, dict):
                raise TypeError
            document = cast("Mapping[str, object]", value)
            record = cls(
                instrument_id=cast("str", document["instrument_id"]),
                source_ticker=cast("str", document["source_ticker"]),
                market_id=cast("str", document["market_id"]),
                session_id=cast("str", document["session_id"]),
                trading_date=date.fromisoformat(cast("str", document["trading_date"])),
                minute_start_exchange_time_ns=cast(
                    "int", document["minute_start_exchange_time_ns"]
                ),
                minute_end_exchange_time_ns=cast(
                    "int", document["minute_end_exchange_time_ns"]
                ),
                session_open_exchange_time_ns=cast(
                    "int", document["session_open_exchange_time_ns"]
                ),
                session_close_exchange_time_ns=cast(
                    "int", document["session_close_exchange_time_ns"]
                ),
                currency=cast("str", document["currency"]),
                tick_value_currency_nanos=cast(
                    "int", document["tick_value_currency_nanos"]
                ),
                open_ticks=cast("int", document["open_ticks"]),
                high_ticks=cast("int", document["high_ticks"]),
                low_ticks=cast("int", document["low_ticks"]),
                close_ticks=cast("int", document["close_ticks"]),
                volume_shares=cast("int", document["volume_shares"]),
                trade_count=cast("int | None", document["trade_count"]),
                vwap_ticks=cast("int | None", document["vwap_ticks"]),
                source_publication_time_ns=cast(
                    "int | None", document["source_publication_time_ns"]
                ),
                source_availability_time_ns=cast(
                    "int", document["source_availability_time_ns"]
                ),
                local_receipt_time_ns=cast("int", document["local_receipt_time_ns"]),
                local_processing_time_ns=cast(
                    "int", document["local_processing_time_ns"]
                ),
                data_quality_state=MinuteDataQuality(
                    cast("str", document["data_quality_state"])
                ),
                data_quality_reasons=tuple(
                    cast("Sequence[str]", document["data_quality_reasons"])
                ),
                source_manifest_id=cast("str", document["source_manifest_id"]),
                source_object_id=cast("str", document["source_object_id"]),
                source_record_id=cast("str", document["source_record_id"]),
                mapping_id=cast("str", document["mapping_id"]),
                instrument_revision_id=cast("str", document["instrument_revision_id"]),
                tick_revision_id=cast("str", document["tick_revision_id"]),
                corporate_action_ids=tuple(
                    cast("Sequence[str]", document["corporate_action_ids"])
                ),
                reference_sha256=cast("str", document["reference_sha256"]),
                record_sha256=cast("str", document["record_sha256"]),
                schema_version=cast("str", document["schema_version"]),
            )
        except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
            raise MinuteNormalizationError(
                MinuteNormalizationCode.CORRUPT_SPOOL,
                "stored canonical minute is malformed",
            ) from error
        if (
            record.schema_version != CANONICAL_MINUTE_SCHEMA_VERSION
            or _digest(record.payload(include_hash=False)) != record.record_sha256
        ):
            raise MinuteNormalizationError(
                MinuteNormalizationCode.CORRUPT_SPOOL,
                "stored canonical minute hash or schema is invalid",
            )
        return record


class CanonicalMinuteNormalizer:
    """Pure fail-closed conversion from bounded raw values to integer ticks."""

    def __init__(self, resolver: MinuteReferenceResolver) -> None:
        """Inject all time and reference dependencies."""
        self._resolver = resolver

    @staticmethod
    def _currency_nanos(value: Decimal | int) -> int:
        if isinstance(value, bool):
            raise MinuteNormalizationError(
                MinuteNormalizationCode.INVALID_PRICE, "boolean price is invalid"
            )
        try:
            decimal_value = value if isinstance(value, Decimal) else Decimal(value)
            scaled = decimal_value * CURRENCY_NANOS_PER_UNIT
        except (InvalidOperation, TypeError, ValueError) as error:
            raise MinuteNormalizationError(
                MinuteNormalizationCode.INVALID_PRICE, "price is not finite decimal"
            ) from error
        if not decimal_value.is_finite() or scaled != scaled.to_integral_value():
            raise MinuteNormalizationError(
                MinuteNormalizationCode.INEXACT_PRICE,
                "price cannot be represented in currency nanos",
            )
        result = int(scaled)
        if not 0 < result <= MAX_INT64:
            raise MinuteNormalizationError(
                MinuteNormalizationCode.INVALID_PRICE, "price is outside int64 range"
            )
        return result

    @classmethod
    def _ticks(
        cls,
        value: Decimal | int,
        encoding: RawPriceEncoding,
        tick_value_currency_nanos: int,
    ) -> int:
        if encoding is RawPriceEncoding.RESOLVED_TICKS:
            if type(value) is not int or not 0 < value <= MAX_INT64:
                raise MinuteNormalizationError(
                    MinuteNormalizationCode.INVALID_PRICE,
                    "resolved tick price is outside int64 range",
                )
            return value
        currency_nanos = cls._currency_nanos(value)
        ticks, remainder = divmod(currency_nanos, tick_value_currency_nanos)
        if remainder:
            raise MinuteNormalizationError(
                MinuteNormalizationCode.INEXACT_PRICE,
                "price is not an exact multiple of its known tick size",
            )
        return ticks

    def normalize(self, raw: RawMinuteBar) -> CanonicalMinuteRecord:
        """Normalize one bar without consulting system time or mutable globals."""
        _require_int64(raw.minute_start_exchange_time_ns, "minute start")
        if raw.minute_start_exchange_time_ns % NANOSECONDS_PER_MINUTE:
            raise MinuteNormalizationError(
                MinuteNormalizationCode.TIMESTAMP_DISORDER,
                "minute start is not aligned to an exchange-time minute",
            )
        if raw.minute_start_exchange_time_ns > MAX_INT64 - NANOSECONDS_PER_MINUTE:
            raise MinuteNormalizationError(
                MinuteNormalizationCode.TIMESTAMP_DISORDER,
                "minute end overflows int64",
            )
        _require_int64(raw.volume, "volume", allow_zero=True)
        if raw.trade_count is not None:
            _require_int64(raw.trade_count, "trade count", allow_zero=True)
        for field, value in (
            ("source availability", raw.source_availability_time_ns),
            ("local receipt", raw.local_receipt_time_ns),
            ("local processing", raw.local_processing_time_ns),
        ):
            _require_int64(value, field)
        if raw.source_publication_time_ns is not None:
            _require_int64(raw.source_publication_time_ns, "source publication")
            if raw.source_availability_time_ns < raw.source_publication_time_ns:
                raise MinuteNormalizationError(
                    MinuteNormalizationCode.TIMESTAMP_DISORDER,
                    "availability precedes publication",
                )
        if (
            raw.local_receipt_time_ns < raw.source_availability_time_ns
            or raw.local_processing_time_ns < raw.local_receipt_time_ns
        ):
            raise MinuteNormalizationError(
                MinuteNormalizationCode.TIMESTAMP_DISORDER,
                "source/local availability timestamps are disordered",
            )
        reference = self._resolver.resolve(
            raw.symbol,
            event_time_ns=raw.minute_start_exchange_time_ns,
            known_at_ns=raw.local_processing_time_ns,
        )
        prices = tuple(
            self._ticks(value, raw.price_encoding, reference.tick_value_currency_nanos)
            for value in (
                raw.open_value,
                raw.high_value,
                raw.low_value,
                raw.close_value,
            )
        )
        open_ticks, high_ticks, low_ticks, close_ticks = prices
        if high_ticks < low_ticks or not (
            low_ticks <= open_ticks <= high_ticks
            and low_ticks <= close_ticks <= high_ticks
        ):
            raise MinuteNormalizationError(
                MinuteNormalizationCode.INVALID_PRICE,
                "OHLC values violate bar invariants",
            )
        vwap_ticks = (
            None
            if raw.vwap_value is None
            else self._ticks(
                raw.vwap_value,
                raw.price_encoding,
                reference.tick_value_currency_nanos,
            )
        )
        reasons = list(reference.quality_reasons)
        if raw.source_publication_time_ns is None:
            reasons.append("SOURCE_PUBLICATION_TIME_UNAVAILABLE")
        common = {
            "instrument_id": reference.instrument_id,
            "source_ticker": raw.symbol,
            "market_id": raw.market_id or reference.market_id,
            "session_id": reference.session_id,
            "trading_date": reference.trading_date,
            "minute_start_exchange_time_ns": raw.minute_start_exchange_time_ns,
            "minute_end_exchange_time_ns": (
                raw.minute_start_exchange_time_ns + NANOSECONDS_PER_MINUTE
            ),
            "session_open_exchange_time_ns": reference.session_open_ns,
            "session_close_exchange_time_ns": reference.session_close_ns,
            "currency": reference.currency,
            "tick_value_currency_nanos": reference.tick_value_currency_nanos,
            "open_ticks": open_ticks,
            "high_ticks": high_ticks,
            "low_ticks": low_ticks,
            "close_ticks": close_ticks,
            "volume_shares": raw.volume,
            "trade_count": raw.trade_count,
            "vwap_ticks": vwap_ticks,
            "source_publication_time_ns": raw.source_publication_time_ns,
            "source_availability_time_ns": raw.source_availability_time_ns,
            "local_receipt_time_ns": raw.local_receipt_time_ns,
            "local_processing_time_ns": raw.local_processing_time_ns,
            "data_quality_state": (
                MinuteDataQuality.DEGRADED if reasons else MinuteDataQuality.VALID
            ),
            "data_quality_reasons": tuple(sorted(set(reasons))),
            "source_manifest_id": raw.source_manifest_id,
            "source_object_id": raw.source_object_id,
            "source_record_id": raw.source_record_id,
            "mapping_id": reference.mapping_id,
            "instrument_revision_id": reference.instrument_revision_id,
            "tick_revision_id": reference.tick_revision_id,
            "corporate_action_ids": reference.corporate_action_ids,
            "reference_sha256": reference.reference_sha256,
        }
        provisional = CanonicalMinuteRecord(
            **common,  # type: ignore[arg-type]
            record_sha256="0" * 64,
        )
        return CanonicalMinuteRecord(
            **common,  # type: ignore[arg-type]
            record_sha256=_digest(provisional.payload(include_hash=False)),
        )


def _parse_utc_ns(value: object) -> int:
    if not isinstance(value, str) or not value.endswith("Z"):
        raise MinuteNormalizationError(
            MinuteNormalizationCode.INVALID_SOURCE,
            "provider timestamp is not explicit UTC",
        )
    try:
        parsed = datetime.fromisoformat(value)
    except ValueError as error:
        raise MinuteNormalizationError(
            MinuteNormalizationCode.INVALID_SOURCE, "provider timestamp is malformed"
        ) from error
    delta = parsed - datetime(1970, 1, 1, tzinfo=UTC)
    result = (
        delta.days * 86_400 * CURRENCY_NANOS_PER_UNIT
        + delta.seconds * CURRENCY_NANOS_PER_UNIT
        + delta.microseconds * 1_000
    )
    _require_int64(result, "provider timestamp")
    return result


def _safe_source_path(root: Path, manifest: SourceManifest) -> Path:
    try:
        root_resolved = root.resolve(strict=True)
        path = root / manifest.storage_path
        if path.is_symlink():
            raise MinuteNormalizationError(
                MinuteNormalizationCode.INVALID_SOURCE, "source object is a symlink"
            )
        resolved = path.resolve(strict=True)
        resolved.relative_to(root_resolved)
    except MinuteNormalizationError:
        raise
    except (OSError, ValueError) as error:
        raise MinuteNormalizationError(
            MinuteNormalizationCode.INVALID_SOURCE,
            "source object is unavailable or outside the data root",
        ) from error
    if not resolved.is_file() or resolved.stat().st_nlink != 1:
        raise MinuteNormalizationError(
            MinuteNormalizationCode.INVALID_SOURCE,
            "source object is not an owner-controlled regular file",
        )
    return resolved


def _verified_source_bytes(
    root: Path, manifest: SourceManifest, maximum_bytes: int
) -> bytes:
    if manifest.size_bytes > maximum_bytes:
        raise MinuteNormalizationError(
            MinuteNormalizationCode.SOURCE_TOO_LARGE,
            "source object exceeds the bounded parser limit",
        )
    path = _safe_source_path(root, manifest)
    try:
        payload = path.read_bytes()
    except OSError as error:
        raise MinuteNormalizationError(
            MinuteNormalizationCode.INVALID_SOURCE, "source object cannot be read"
        ) from error
    if len(payload) != manifest.size_bytes:
        raise MinuteNormalizationError(
            MinuteNormalizationCode.HASH_MISMATCH, "source object size changed"
        )
    if hashlib.sha256(payload).hexdigest() != manifest.object_sha256:
        raise MinuteNormalizationError(
            MinuteNormalizationCode.HASH_MISMATCH, "source object hash changed"
        )
    return payload


def _decimal_source_value(value: object) -> Decimal:
    if isinstance(value, Decimal):
        return value
    if type(value) is int:
        return Decimal(value)
    if isinstance(value, str):
        return Decimal(value)
    raise TypeError


def _alpaca_bars(
    payload: bytes,
    manifest: SourceManifest,
    *,
    canonical_processing_time_ns: int | None = None,
) -> Iterator[RawMinuteBar]:
    try:
        value = json.loads(payload, parse_float=Decimal)
    except (UnicodeDecodeError, json.JSONDecodeError, InvalidOperation) as error:
        raise MinuteNormalizationError(
            MinuteNormalizationCode.INVALID_SOURCE,
            "Alpaca source object is malformed JSON",
        ) from error
    if not isinstance(value, dict) or not isinstance(value.get("bars"), dict):
        raise MinuteNormalizationError(
            MinuteNormalizationCode.INVALID_SOURCE,
            "Alpaca source object lacks its bars mapping",
        )
    bars_by_symbol = cast("Mapping[str, object]", value["bars"])
    for symbol in sorted(bars_by_symbol):
        raw_bars = bars_by_symbol[symbol]
        if not isinstance(raw_bars, list):
            raise MinuteNormalizationError(
                MinuteNormalizationCode.INVALID_SOURCE,
                "Alpaca symbol bars are not an array",
            )
        previous_event_time_ns: int | None = None
        for raw in raw_bars:
            if not isinstance(raw, dict):
                raise MinuteNormalizationError(
                    MinuteNormalizationCode.INVALID_SOURCE,
                    "Alpaca bar is not an object",
                )
            fields = cast("Mapping[str, object]", raw)
            try:
                event_time_ns = _parse_utc_ns(fields["t"])
                prices = tuple(
                    _decimal_source_value(value)
                    for value in (
                        fields["o"],
                        fields["h"],
                        fields["l"],
                        fields["c"],
                    )
                )
                volume = fields["v"]
                trade_count = fields.get("n")
                vwap = fields.get("vw")
            except (KeyError, InvalidOperation, TypeError, ValueError) as error:
                raise MinuteNormalizationError(
                    MinuteNormalizationCode.INVALID_SOURCE,
                    "Alpaca bar fields are malformed",
                ) from error
            if (
                previous_event_time_ns is not None
                and event_time_ns < previous_event_time_ns
            ):
                raise MinuteNormalizationError(
                    MinuteNormalizationCode.TIMESTAMP_DISORDER,
                    "Alpaca source bars are not monotonic within a symbol",
                )
            previous_event_time_ns = event_time_ns
            if type(volume) is not int or (
                trade_count is not None and type(trade_count) is not int
            ):
                raise MinuteNormalizationError(
                    MinuteNormalizationCode.INVALID_SOURCE,
                    "Alpaca quantity fields are not integers",
                )
            open_value, high_value, low_value, close_value = prices
            record_payload = {
                "close": str(close_value),
                "event_time_ns": event_time_ns,
                "high": str(high_value),
                "low": str(low_value),
                "open": str(open_value),
                "symbol": symbol,
                "trade_count": trade_count,
                "volume": volume,
                "vwap": None if vwap is None else str(vwap),
            }
            processing_time_ns = max(
                manifest.times.processing_time_min_ns,
                canonical_processing_time_ns or 0,
            )
            yield RawMinuteBar(
                symbol=symbol,
                minute_start_exchange_time_ns=event_time_ns,
                open_value=open_value,
                high_value=high_value,
                low_value=low_value,
                close_value=close_value,
                volume=volume,
                trade_count=trade_count,
                vwap_value=(None if vwap is None else _decimal_source_value(vwap)),
                price_encoding=RawPriceEncoding.CURRENCY_UNITS_DECIMAL,
                source_manifest_id=manifest.manifest_id,
                source_object_id=manifest.source_object_id,
                source_record_id=f"alpaca:{_digest(record_payload)}",
                # Alpaca historical bars do not carry their original publication
                # time.  The source manifest publication field records acquisition
                # receipt for repository compatibility and must not be relabelled
                # as historical provider publication here.
                source_publication_time_ns=None,
                source_availability_time_ns=manifest.times.publication_time_min_ns,
                local_receipt_time_ns=manifest.times.receive_time_min_ns,
                local_processing_time_ns=processing_time_ns,
                market_id="IEX",
            )


def _synthetic_bars(payload: bytes, manifest: SourceManifest) -> Iterator[RawMinuteBar]:
    for line_number, line in enumerate(payload.splitlines(), start=1):
        if not line:
            continue
        try:
            value = json.loads(line)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise MinuteNormalizationError(
                MinuteNormalizationCode.INVALID_SOURCE,
                "synthetic source line is malformed",
            ) from error
        if not isinstance(value, dict):
            raise MinuteNormalizationError(
                MinuteNormalizationCode.INVALID_SOURCE,
                "synthetic source line is not an object",
            )
        fields = cast("Mapping[str, object]", value)
        try:
            symbol = cast("str", fields["symbol"])
            event_time_ns = cast("int", fields["event_time_ns"])
            open_ticks = cast("int", fields["open_ticks"])
            high_ticks = cast("int", fields["high_ticks"])
            low_ticks = cast("int", fields["low_ticks"])
            close_ticks = cast("int", fields["close_ticks"])
            volume = cast("int", fields["quantity_units"])
        except KeyError as error:
            raise MinuteNormalizationError(
                MinuteNormalizationCode.INVALID_SOURCE,
                "synthetic source fields are incomplete",
            ) from error
        record_hash = hashlib.sha256(line).hexdigest()
        yield RawMinuteBar(
            symbol=symbol,
            minute_start_exchange_time_ns=event_time_ns,
            open_value=open_ticks,
            high_value=high_ticks,
            low_value=low_ticks,
            close_value=close_ticks,
            volume=volume,
            trade_count=None,
            vwap_value=None,
            price_encoding=RawPriceEncoding.RESOLVED_TICKS,
            source_manifest_id=manifest.manifest_id,
            source_object_id=manifest.source_object_id,
            source_record_id=f"synthetic:{line_number}:{record_hash}",
            source_publication_time_ns=event_time_ns,
            source_availability_time_ns=event_time_ns,
            local_receipt_time_ns=event_time_ns,
            local_processing_time_ns=event_time_ns,
            market_id="SYNTHETIC",
        )


def iter_source_minutes(
    root: Path,
    manifest: SourceManifest,
    *,
    maximum_source_object_bytes: int = DEFAULT_MAX_SOURCE_OBJECT_BYTES,
    canonical_processing_time_ns: int | None = None,
) -> Iterator[RawMinuteBar]:
    """Yield one verified source object without retaining the complete dataset."""
    if maximum_source_object_bytes <= 0:
        raise MinuteNormalizationError(
            MinuteNormalizationCode.SOURCE_TOO_LARGE,
            "source-object parser bound must be positive",
        )
    payload = _verified_source_bytes(root, manifest, maximum_source_object_bytes)
    if manifest.schema_name == "alpaca-market-data-v2-bars-response":
        records = _alpaca_bars(
            payload,
            manifest,
            canonical_processing_time_ns=canonical_processing_time_ns,
        )
    elif manifest.schema_name == "synthetic-minute-bars-jsonl":
        records = _synthetic_bars(payload, manifest)
    else:
        raise MinuteNormalizationError(
            MinuteNormalizationCode.UNSUPPORTED_SCHEMA,
            f"source schema {manifest.schema_name} is not supported",
        )
    seen = 0
    for record in records:
        seen += 1
        if seen > manifest.record_count:
            raise MinuteNormalizationError(
                MinuteNormalizationCode.INVALID_SOURCE,
                "source record count exceeds its immutable manifest",
            )
        yield record
    if seen != manifest.record_count:
        raise MinuteNormalizationError(
            MinuteNormalizationCode.INVALID_SOURCE,
            "source record count differs from its immutable manifest",
        )


@dataclass(frozen=True, slots=True)
class SpoolIngestResult:
    """Bounded result for one atomic source-object ingestion."""

    source_manifest_id: str
    input_records: int
    inserted_records: int
    identical_duplicates: int
    rejected_outside_session: int
    already_processed: bool


def _encode_spool_record(payload: bytes) -> bytes:
    if len(payload) > _MAX_SPOOL_RECORD_BYTES:
        raise MinuteNormalizationError(
            MinuteNormalizationCode.CORRUPT_SPOOL,
            "canonical spool record exceeds its encoding bound",
        )
    return _SPOOL_RECORD_MAGIC + zlib.compress(payload, level=6)


def _decode_spool_record(value: object) -> bytes:
    if not isinstance(value, bytes) or not value.startswith(_SPOOL_RECORD_MAGIC):
        raise MinuteNormalizationError(
            MinuteNormalizationCode.CORRUPT_SPOOL,
            "spool record is not a versioned compressed canonical document",
        )
    try:
        decoder = zlib.decompressobj()
        payload = decoder.decompress(
            value[len(_SPOOL_RECORD_MAGIC) :], _MAX_SPOOL_RECORD_BYTES + 1
        )
    except zlib.error as error:
        raise MinuteNormalizationError(
            MinuteNormalizationCode.CORRUPT_SPOOL,
            "spool record compression is corrupt",
        ) from error
    if (
        len(payload) > _MAX_SPOOL_RECORD_BYTES
        or not decoder.eof
        or decoder.unconsumed_tail
        or decoder.unused_data
    ):
        raise MinuteNormalizationError(
            MinuteNormalizationCode.CORRUPT_SPOOL,
            "spool record compression is truncated, oversized, or trailing",
        )
    return payload


class CanonicalMinuteSpool:
    """Crash-safe bounded deduplication spool for date/bucket Parquet output."""

    def __init__(
        self,
        path: Path,
        *,
        instrument_buckets: int = DEFAULT_INSTRUMENT_BUCKETS,
        maximum_records: int = DEFAULT_MAX_SPOOL_RECORDS,
        maximum_bytes: int = 12_000_000_000,
    ) -> None:
        """Open an owner-only SQLite spool with explicit record and byte limits."""
        if not 1 <= instrument_buckets <= MAX_INSTRUMENT_BUCKETS:
            raise ValueError("instrument_buckets must be from 1 through 256")
        if maximum_records <= 0:
            raise ValueError("maximum_records must be positive")
        if maximum_bytes < _MINIMUM_SPOOL_BYTES:
            raise ValueError("maximum_bytes is too small")
        if path.is_symlink() or (path.parent.exists() and path.parent.is_symlink()):
            raise MinuteNormalizationError(
                MinuteNormalizationCode.CORRUPT_SPOOL, "spool path is a symlink"
            )
        path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.path = path
        self.instrument_buckets = instrument_buckets
        self.maximum_records = maximum_records
        self._closed = False
        try:
            new_database = not path.exists()
            self._connection = sqlite3.connect(path, isolation_level=None)
            if new_database:
                self._connection.execute(f"PRAGMA page_size={_SPOOL_PAGE_SIZE}")
            self._connection.execute("PRAGMA journal_mode=DELETE")
            self._connection.execute("PRAGMA synchronous=FULL")
            self._connection.execute("PRAGMA trusted_schema=OFF")
            page_size = cast(
                "int", self._connection.execute("PRAGMA page_size").fetchone()[0]
            )
            self._connection.execute(
                f"PRAGMA max_page_count={maximum_bytes // page_size}"
            )
            self._initialize()
        except (sqlite3.DatabaseError, MinuteNormalizationError) as error:
            connection = getattr(self, "_connection", None)
            if connection is not None:
                connection.close()
            if isinstance(error, MinuteNormalizationError):
                raise
            if "full" in str(error).lower():
                raise MinuteNormalizationError(
                    MinuteNormalizationCode.RECORD_LIMIT,
                    "spool byte limit is too small for its schema",
                ) from error
            raise MinuteNormalizationError(
                MinuteNormalizationCode.CORRUPT_SPOOL, "spool cannot be opened"
            ) from error

    def __enter__(self) -> Self:
        """Return this spool for context-managed use."""
        return self

    def __exit__(self, *_args: object) -> None:
        """Close the spool on context exit."""
        self.close()

    def close(self) -> None:
        """Close the spool idempotently."""
        if not self._closed:
            self._connection.close()
            self._closed = True

    def _initialize(self) -> None:
        self._connection.executescript(
            """
            CREATE TABLE IF NOT EXISTS metadata(
              key TEXT PRIMARY KEY, value TEXT NOT NULL
            ) WITHOUT ROWID;
            INSERT OR IGNORE INTO metadata VALUES('schema_version', '1.2.0');
            CREATE TABLE IF NOT EXISTS records(
              trading_date TEXT NOT NULL,
              instrument_bucket INTEGER NOT NULL,
              instrument_id TEXT NOT NULL,
              minute_start_ns INTEGER NOT NULL,
              economic_sha256 TEXT NOT NULL,
              record_sha256 TEXT NOT NULL,
              record_json BLOB NOT NULL,
              PRIMARY KEY(instrument_id, minute_start_ns)
            ) WITHOUT ROWID;
            DROP INDEX IF EXISTS partition_records_idx;
            CREATE TABLE IF NOT EXISTS processed_sources(
              source_manifest_id TEXT PRIMARY KEY,
              object_sha256 TEXT NOT NULL,
              input_records INTEGER NOT NULL,
              inserted_records INTEGER NOT NULL,
              identical_duplicates INTEGER NOT NULL,
              rejected_outside_session INTEGER NOT NULL
            ) WITHOUT ROWID;
            CREATE TABLE IF NOT EXISTS partition_sources(
              trading_date TEXT NOT NULL,
              instrument_bucket INTEGER NOT NULL,
              source_manifest_id TEXT NOT NULL,
              PRIMARY KEY(trading_date, instrument_bucket, source_manifest_id)
            ) WITHOUT ROWID;
            """
        )
        result = self._connection.execute("PRAGMA quick_check").fetchone()
        metadata = self._connection.execute(
            "SELECT value FROM metadata WHERE key='schema_version'"
        ).fetchone()
        if result is None or result[0] != "ok" or metadata != ("1.2.0",):
            raise MinuteNormalizationError(
                MinuteNormalizationCode.CORRUPT_SPOOL,
                "spool integrity or schema validation failed",
            )
        self._record_count = cast(
            "int",
            self._connection.execute("SELECT count(*) FROM records").fetchone()[0],
        )

    def _bucket(self, instrument_id: str) -> int:
        digest = hashlib.sha256(instrument_id.encode("ascii")).digest()
        return int.from_bytes(digest[:8], "big") % self.instrument_buckets

    def _rollback(self) -> None:
        if self._connection.in_transaction:
            self._connection.execute("ROLLBACK")

    def _raise_database_failure(self, error: sqlite3.DatabaseError) -> NoReturn:
        self._rollback()
        if "full" in str(error).lower():
            raise MinuteNormalizationError(
                MinuteNormalizationCode.RECORD_LIMIT,
                "normalization spool byte limit is exhausted",
            ) from error
        raise MinuteNormalizationError(
            MinuteNormalizationCode.CORRUPT_SPOOL,
            "normalization spool transaction failed",
        ) from error

    def ingest(  # noqa: PLR0915
        self,
        manifest: SourceManifest,
        records: Iterable[RawMinuteBar],
        normalizer: CanonicalMinuteNormalizer,
        *,
        fault_injector: Callable[[str], None] | None = None,
    ) -> SpoolIngestResult:
        """Normalize and deduplicate one source object as a single transaction."""
        prior = self._connection.execute(
            """
            SELECT object_sha256, input_records, inserted_records,
                   identical_duplicates, rejected_outside_session
            FROM processed_sources WHERE source_manifest_id=?
            """,
            (manifest.manifest_id,),
        ).fetchone()
        if prior is not None:
            if prior[0] != manifest.object_sha256:
                raise MinuteNormalizationError(
                    MinuteNormalizationCode.HASH_MISMATCH,
                    "processed source identity now names different bytes",
                )
            return SpoolIngestResult(
                manifest.manifest_id,
                cast("int", prior[1]),
                cast("int", prior[2]),
                cast("int", prior[3]),
                cast("int", prior[4]),
                True,
            )
        input_count = 0
        inserted = 0
        duplicates = 0
        rejected_outside_session = 0
        starting_record_count = self._record_count
        previous_by_symbol: dict[str, int] = {}
        partitions_seen: set[tuple[str, int]] = set()
        try:
            self._connection.execute("BEGIN IMMEDIATE")
            for raw in records:
                input_count += 1
                previous = previous_by_symbol.get(raw.symbol)
                if (
                    previous is not None
                    and raw.minute_start_exchange_time_ns < previous
                ):
                    raise MinuteNormalizationError(
                        MinuteNormalizationCode.TIMESTAMP_DISORDER,
                        "source bars are not monotonic within a symbol",
                    )
                previous_by_symbol[raw.symbol] = raw.minute_start_exchange_time_ns
                try:
                    canonical = normalizer.normalize(raw)
                except MinuteNormalizationError as error:
                    if error.code is MinuteNormalizationCode.OUTSIDE_SESSION:
                        rejected_outside_session += 1
                        continue
                    raise
                encoded = _canonical_bytes(canonical.payload())
                stored = _encode_spool_record(encoded)
                bucket = self._bucket(canonical.instrument_id)
                economic_hash = canonical.economic_hash()
                cursor = self._connection.execute(
                    """
                    INSERT OR IGNORE INTO records VALUES(?, ?, ?, ?, ?, ?, ?)
                    """,
                    (
                        canonical.trading_date.isoformat(),
                        bucket,
                        canonical.instrument_id,
                        canonical.minute_start_exchange_time_ns,
                        economic_hash,
                        canonical.record_sha256,
                        stored,
                    ),
                )
                if cursor.rowcount == 1:
                    if self._record_count >= self.maximum_records:
                        raise MinuteNormalizationError(
                            MinuteNormalizationCode.RECORD_LIMIT,
                            "normalization spool record limit is exhausted",
                        )
                    inserted += 1
                    self._record_count += 1
                else:
                    existing = self._connection.execute(
                        """
                        SELECT economic_sha256, record_json FROM records
                        WHERE instrument_id=? AND minute_start_ns=?
                        """,
                        (
                            canonical.instrument_id,
                            canonical.minute_start_exchange_time_ns,
                        ),
                    ).fetchone()
                    if existing is None:  # pragma: no cover - SQLite atomic invariant
                        raise MinuteNormalizationError(
                            MinuteNormalizationCode.CORRUPT_SPOOL,
                            "ignored insert has no existing record",
                        )
                    if existing[0] != economic_hash:
                        raise MinuteNormalizationError(
                            MinuteNormalizationCode.CONFLICTING_DUPLICATE,
                            "same instrument/minute carries different economic values",
                        )
                    duplicates += 1
                    existing_bytes = _decode_spool_record(existing[1])
                    if encoded < existing_bytes:
                        self._connection.execute(
                            """
                            UPDATE records SET record_sha256=?, record_json=?
                            WHERE instrument_id=? AND minute_start_ns=?
                            """,
                            (
                                canonical.record_sha256,
                                stored,
                                canonical.instrument_id,
                                canonical.minute_start_exchange_time_ns,
                            ),
                        )
                partitions_seen.add((canonical.trading_date.isoformat(), bucket))
            for trading_date, bucket in sorted(partitions_seen):
                self._connection.execute(
                    "INSERT OR IGNORE INTO partition_sources VALUES(?, ?, ?)",
                    (trading_date, bucket, manifest.manifest_id),
                )
            if input_count != manifest.record_count:
                raise MinuteNormalizationError(
                    MinuteNormalizationCode.INVALID_SOURCE,
                    "normalized count differs from source manifest",
                )
            self._connection.execute(
                "INSERT INTO processed_sources VALUES(?, ?, ?, ?, ?, ?)",
                (
                    manifest.manifest_id,
                    manifest.object_sha256,
                    input_count,
                    inserted,
                    duplicates,
                    rejected_outside_session,
                ),
            )
            if fault_injector is not None:
                fault_injector("AFTER_ROWS_BEFORE_COMMIT")
            self._connection.execute("COMMIT")
            self._sync_parent()
        except sqlite3.DatabaseError as error:
            self._record_count = starting_record_count
            self._raise_database_failure(error)
        except Exception:
            self._record_count = starting_record_count
            self._rollback()
            raise
        return SpoolIngestResult(
            manifest.manifest_id,
            input_count,
            inserted,
            duplicates,
            rejected_outside_session,
            False,
        )

    @property
    def record_count(self) -> int:
        """Return the bounded accepted-row count without a full table scan."""
        return self._record_count

    def ingest_totals(self) -> Mapping[str, int]:
        """Return deterministic source and rejection totals for audit reports."""
        row = self._connection.execute(
            """
            SELECT count(*), coalesce(sum(input_records), 0),
                   coalesce(sum(inserted_records), 0),
                   coalesce(sum(identical_duplicates), 0),
                   coalesce(sum(rejected_outside_session), 0)
            FROM processed_sources
            """
        ).fetchone()
        return {
            "source_objects": cast("int", row[0]),
            "input_records": cast("int", row[1]),
            "inserted_records": cast("int", row[2]),
            "identical_duplicates": cast("int", row[3]),
            "rejected_outside_session": cast("int", row[4]),
        }

    def partition_keys(self) -> tuple[tuple[str, int], ...]:
        """Return deterministic nonempty date/bucket partition keys."""
        self._connection.execute(
            """
            CREATE INDEX IF NOT EXISTS partition_records_idx ON records(
              trading_date, instrument_bucket, instrument_id, minute_start_ns
            )
            """
        )
        rows = self._connection.execute(
            """
            SELECT DISTINCT trading_date, instrument_bucket FROM records
            ORDER BY trading_date, instrument_bucket
            """
        )
        return tuple((cast("str", row[0]), cast("int", row[1])) for row in rows)

    def records_for_partition(
        self, trading_date: str, instrument_bucket: int, *, batch_rows: int
    ) -> Iterator[tuple[CanonicalMinuteRecord, ...]]:
        """Stream a deterministic partition in bounded batches."""
        if batch_rows <= 0:
            raise ValueError("batch_rows must be positive")
        cursor = self._connection.execute(
            """
            SELECT record_json FROM records
            WHERE trading_date=? AND instrument_bucket=?
            ORDER BY instrument_id, minute_start_ns
            """,
            (trading_date, instrument_bucket),
        )
        while rows := cursor.fetchmany(batch_rows):
            records = tuple(
                (CanonicalMinuteRecord.from_bytes(_decode_spool_record(row[0])))
                for row in rows
            )
            yield records

    def source_manifest_ids(
        self, trading_date: str, instrument_bucket: int
    ) -> tuple[str, ...]:
        """Return all source manifests contributing to one partition."""
        rows = self._connection.execute(
            """
            SELECT source_manifest_id FROM partition_sources
            WHERE trading_date=? AND instrument_bucket=?
            ORDER BY source_manifest_id
            """,
            (trading_date, instrument_bucket),
        )
        return tuple(cast("str", row[0]) for row in rows)

    def duplicate_count(self, source_manifest_ids: Sequence[str]) -> int:
        """Return identical-duplicate observations for exact sources."""
        if not source_manifest_ids:
            return 0
        return sum(
            cast(
                "int",
                self._connection.execute(
                    """
                    SELECT identical_duplicates FROM processed_sources
                    WHERE source_manifest_id=?
                    """,
                    (source_id,),
                ).fetchone()[0],
            )
            for source_id in source_manifest_ids
        )

    def _sync_parent(self) -> None:
        descriptor = os.open(self.path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)


_PARQUET_SCHEMA = pa.schema(
    [
        pa.field("schema_version", pa.string(), nullable=False),
        pa.field("instrument_id", pa.string(), nullable=False),
        pa.field("source_ticker", pa.string(), nullable=False),
        pa.field("market_id", pa.string(), nullable=False),
        pa.field("session_id", pa.string(), nullable=False),
        pa.field("trading_date", pa.date32(), nullable=False),
        pa.field("minute_start_exchange_time_ns", pa.int64(), nullable=False),
        pa.field("minute_end_exchange_time_ns", pa.int64(), nullable=False),
        pa.field("session_open_exchange_time_ns", pa.int64(), nullable=False),
        pa.field("session_close_exchange_time_ns", pa.int64(), nullable=False),
        pa.field("currency", pa.string(), nullable=False),
        pa.field("price_unit", pa.string(), nullable=False),
        pa.field("tick_value_currency_nanos", pa.int64(), nullable=False),
        pa.field("open_ticks", pa.int64(), nullable=False),
        pa.field("high_ticks", pa.int64(), nullable=False),
        pa.field("low_ticks", pa.int64(), nullable=False),
        pa.field("close_ticks", pa.int64(), nullable=False),
        pa.field("quantity_unit", pa.string(), nullable=False),
        pa.field("volume_shares", pa.int64(), nullable=False),
        pa.field("trade_count", pa.int64()),
        pa.field("vwap_ticks", pa.int64()),
        pa.field("source_publication_time_ns", pa.int64()),
        pa.field("source_availability_time_ns", pa.int64(), nullable=False),
        pa.field("local_receipt_time_ns", pa.int64(), nullable=False),
        pa.field("local_processing_time_ns", pa.int64(), nullable=False),
        pa.field("data_quality_state", pa.string(), nullable=False),
        pa.field("data_quality_reasons", pa.list_(pa.string()), nullable=False),
        pa.field("source_manifest_id", pa.string(), nullable=False),
        pa.field("source_object_id", pa.string(), nullable=False),
        pa.field("source_record_id", pa.string(), nullable=False),
        pa.field("mapping_id", pa.string(), nullable=False),
        pa.field("instrument_revision_id", pa.string(), nullable=False),
        pa.field("tick_revision_id", pa.string(), nullable=False),
        pa.field("corporate_action_ids", pa.list_(pa.string()), nullable=False),
        pa.field("reference_sha256", pa.string(), nullable=False),
        pa.field("record_sha256", pa.string(), nullable=False),
    ],
    metadata={
        b"aegis.schema": b"canonical-minute-record",
        b"aegis.schema_version": CANONICAL_MINUTE_SCHEMA_VERSION.encode("ascii"),
        b"aegis.price_unit": b"integer ticks with explicit currency nanos per tick",
        b"aegis.quantity_unit": b"shares",
    },
)


def _arrow_row(record: CanonicalMinuteRecord) -> dict[str, object]:
    payload = record.payload()
    payload["trading_date"] = record.trading_date
    return payload


@dataclass(frozen=True, slots=True)
class PartitionQuality:
    """Quality and point-in-time evidence computed before partition acceptance."""

    document: Mapping[str, object]
    encoded: bytes
    sha256: str
    accepted: bool


@dataclass(frozen=True, slots=True)
class ParquetWriteResult:
    """Immutable staged Parquet metadata."""

    path: Path
    sha256: str
    size_bytes: int
    record_count: int
    times: ManifestTimeRange


@dataclass(frozen=True, slots=True)
class PublishedMinutePartition:
    """Accepted quality report, Parquet object, and final manifest paths."""

    quality_report_path: Path
    parquet_path: Path
    manifest_path: Path
    manifest_id: str
    quality_report_sha256: str
    parquet_sha256: str
    record_count: int


def build_partition_quality(
    spool: CanonicalMinuteSpool,
    trading_date: str,
    instrument_bucket: int,
    *,
    batch_rows: int = DEFAULT_BATCH_ROWS,
) -> PartitionQuality:
    """Scan bounded batches and publish explicit missing/quality observations."""
    record_count = 0
    degraded_count = 0
    record_hash = hashlib.sha256()
    instrument_sessions: dict[str, tuple[int, int]] = {}
    reason_counts: dict[str, int] = {}
    for batch in spool.records_for_partition(
        trading_date, instrument_bucket, batch_rows=batch_rows
    ):
        for record in batch:
            record_count += 1
            record_hash.update(bytes.fromhex(record.record_sha256))
            instrument_sessions[record.instrument_id] = (
                record.session_open_exchange_time_ns,
                record.session_close_exchange_time_ns,
            )
            if record.data_quality_state is MinuteDataQuality.DEGRADED:
                degraded_count += 1
            for reason in record.data_quality_reasons:
                reason_counts[reason] = reason_counts.get(reason, 0) + 1
    expected_minutes = sum(
        (closed - opened) // NANOSECONDS_PER_MINUTE
        for opened, closed in instrument_sessions.values()
    )
    missing_minutes = max(expected_minutes - record_count, 0)
    sources = spool.source_manifest_ids(trading_date, instrument_bucket)
    accepted = record_count > 0
    body: dict[str, object] = {
        "accepted": accepted,
        "conflicting_duplicates": 0,
        "data_quality_reason_counts": dict(sorted(reason_counts.items())),
        "degraded_records": degraded_count,
        "economic_value_claimed": False,
        "expected_regular_session_minutes": expected_minutes,
        "filled_missing_minutes": 0,
        "identical_duplicates": spool.duplicate_count(sources),
        "instrument_bucket": instrument_bucket,
        "instrument_count": len(instrument_sessions),
        "live_trading_capable": False,
        "missing_minutes": missing_minutes,
        "record_count": record_count,
        "record_sequence_sha256": record_hash.hexdigest(),
        "schema_version": CANONICAL_MINUTE_QUALITY_SCHEMA_VERSION,
        "source_manifest_ids": list(sources),
        "status": (
            "VALID"
            if accepted and degraded_count == 0 and missing_minutes == 0
            else "DEGRADED"
            if accepted
            else "REJECTED"
        ),
        "trading_date": trading_date,
    }
    quality_sha256 = _digest(body)
    document = {**body, "quality_report_sha256": quality_sha256}
    encoded = _canonical_bytes(document) + b"\n"
    return PartitionQuality(document, encoded, quality_sha256, accepted)


def write_partition_parquet(
    spool: CanonicalMinuteSpool,
    trading_date: str,
    instrument_bucket: int,
    output_path: Path,
    *,
    batch_rows: int = DEFAULT_BATCH_ROWS,
) -> ParquetWriteResult:
    """Write deterministically ordered Zstandard Parquet in bounded row groups."""
    if batch_rows <= 0:
        raise ValueError("batch_rows must be positive")
    output_path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    if output_path.exists() or output_path.is_symlink():
        raise MinuteNormalizationError(
            MinuteNormalizationCode.INVALID_SOURCE,
            "staged Parquet path must not already exist",
        )
    count = 0
    minima: dict[str, int] = {}
    maxima: dict[str, int] = {}
    try:
        writer = pq.ParquetWriter(
            output_path,
            _PARQUET_SCHEMA,
            version="2.6",
            compression="zstd",
            compression_level=3,
            use_dictionary=(
                "schema_version",
                "source_ticker",
                "market_id",
                "currency",
                "price_unit",
                "quantity_unit",
                "data_quality_state",
            ),
            write_statistics=True,
            data_page_version="2.0",
            write_page_index=True,
        )
        try:
            for batch in spool.records_for_partition(
                trading_date, instrument_bucket, batch_rows=batch_rows
            ):
                rows = [_arrow_row(record) for record in batch]
                table = pa.Table.from_pylist(rows, schema=_PARQUET_SCHEMA)
                writer.write_table(table, row_group_size=batch_rows)
                for record in batch:
                    count += 1
                    values = {
                        "event": record.minute_start_exchange_time_ns,
                        "processing": record.local_processing_time_ns,
                        "publication": (
                            record.source_publication_time_ns
                            if record.source_publication_time_ns is not None
                            else record.source_availability_time_ns
                        ),
                        "receive": record.local_receipt_time_ns,
                        "revision": record.source_availability_time_ns,
                    }
                    for name, value in values.items():
                        minima[name] = min(minima.get(name, value), value)
                        maxima[name] = max(maxima.get(name, value), value)
        finally:
            writer.close()
        if count == 0:
            raise MinuteNormalizationError(
                MinuteNormalizationCode.INVALID_SOURCE, "partition contains no records"
            )
    except Exception:
        output_path.unlink(missing_ok=True)
        raise
    with output_path.open("rb") as source:
        object_sha256 = hashlib.file_digest(source, "sha256").hexdigest()
    size_bytes = output_path.stat().st_size
    metadata = pq.read_metadata(output_path)
    if metadata.num_rows != count:
        output_path.unlink(missing_ok=True)
        raise MinuteNormalizationError(
            MinuteNormalizationCode.HASH_MISMATCH,
            "Parquet row count differs after write",
        )
    compression = {
        metadata.row_group(group).column(column).compression
        for group in range(metadata.num_row_groups)
        for column in range(metadata.num_columns)
    }
    if compression != {"ZSTD"}:
        output_path.unlink(missing_ok=True)
        raise MinuteNormalizationError(
            MinuteNormalizationCode.HASH_MISMATCH,
            "Parquet columns are not uniformly Zstandard compressed",
        )
    return ParquetWriteResult(
        output_path,
        object_sha256,
        size_bytes,
        count,
        ManifestTimeRange(
            event_time_min_ns=minima["event"],
            event_time_max_ns=maxima["event"],
            publication_time_min_ns=minima["publication"],
            publication_time_max_ns=maxima["publication"],
            receive_time_min_ns=minima["receive"],
            receive_time_max_ns=maxima["receive"],
            processing_time_min_ns=minima["processing"],
            processing_time_max_ns=maxima["processing"],
            revision_time_min_ns=minima["revision"],
            revision_time_max_ns=maxima["revision"],
        ),
    )


def _write_staged(path: Path, payload: bytes) -> None:
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    descriptor = os.open(
        path,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC,
        0o600,
    )
    try:
        with os.fdopen(descriptor, "wb", closefd=True) as output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
    except BaseException:
        path.unlink(missing_ok=True)
        raise


def publish_partition(
    repository: DataRepository,
    quota: QuotaEvidence,
    spool: CanonicalMinuteSpool,
    trading_date: str,
    instrument_bucket: int,
    *,
    universe_snapshot_sha256: str,
    batch_rows: int = DEFAULT_BATCH_ROWS,
    fault_injector: Callable[[str], None] | None = None,
    admission_lease: AdmissionLease | None = None,
) -> PublishedMinutePartition:
    """Publish quality first, then data, and manifest last as acceptance marker."""
    _require_sha256(universe_snapshot_sha256, "universe_snapshot_sha256")
    quality = build_partition_quality(
        spool, trading_date, instrument_bucket, batch_rows=batch_rows
    )
    if not quality.accepted:
        raise MinuteNormalizationError(
            MinuteNormalizationCode.INVALID_SOURCE,
            "empty partition failed quality acceptance",
        )
    record_count = cast("int", quality.document["record_count"])
    estimated_data_bytes = (
        record_count * DEFAULT_MAX_OUTPUT_BYTES_PER_RECORD + 1_000_000
    )
    operation_id = (
        f"canonical-minute:{trading_date}:bucket-{instrument_bucket:03d}:"
        f"{quality.sha256[:16]}"
    )
    request = StorageRequest(
        operation_id,
        output_bytes=estimated_data_bytes + len(quality.encoded) + 1_000_000,
        temporary_bytes=estimated_data_bytes + len(quality.encoded),
    )
    temp_prefix = f"tmp/canonical-minute/{quality.sha256}"
    parquet_stage_relative = f"{temp_prefix}.parquet.part"
    report_stage_relative = f"{temp_prefix}.quality.json.part"
    parquet_stage = repository.root / parquet_stage_relative
    report_stage = repository.root / report_stage_relative
    lease_context = (
        repository.acquire_admission(request, quota)
        if admission_lease is None
        else nullcontext(admission_lease)
    )
    with lease_context as lease:
        try:
            _write_staged(report_stage, quality.encoded)
            parquet = write_partition_parquet(
                spool,
                trading_date,
                instrument_bucket,
                parquet_stage,
                batch_rows=batch_rows,
            )
            if parquet.size_bytes > estimated_data_bytes:
                raise MinuteNormalizationError(
                    MinuteNormalizationCode.SOURCE_TOO_LARGE,
                    "Parquet output exceeded its admitted upper bound",
                )
            quality_final_relative = (
                f"reports/canonical-minute/quality/{quality.sha256}.json"
            )
            quality_path = repository.publish_staged_object(
                report_stage_relative,
                quality_final_relative,
                expected_sha256=hashlib.sha256(quality.encoded).hexdigest(),
                expected_size_bytes=len(quality.encoded),
                lease=lease,
            )
            if fault_injector is not None:
                fault_injector("QUALITY_PUBLISHED")
            parquet_final_relative = (
                f"canonical/minute/date={trading_date}/"
                f"instrument_bucket={instrument_bucket:03d}/"
                f"part-{parquet.sha256}.parquet"
            )
            parquet_path = repository.publish_staged_object(
                parquet_stage_relative,
                parquet_final_relative,
                expected_sha256=parquet.sha256,
                expected_size_bytes=parquet.size_bytes,
                lease=lease,
            )
            if fault_injector is not None:
                fault_injector("PARQUET_PUBLISHED")
            source_ids = spool.source_manifest_ids(trading_date, instrument_bucket)
            manifest = PartitionManifest(
                dataset_name="canonical-minute-poc",
                partition_key=(
                    f"date={trading_date}/instrument_bucket={instrument_bucket:03d}/"
                    f"quality={quality.sha256}"
                ),
                storage_path=parquet_final_relative,
                object_sha256=parquet.sha256,
                size_bytes=parquet.size_bytes,
                record_count=parquet.record_count,
                schema_name="canonical-minute-record",
                schema_version=CANONICAL_MINUTE_SCHEMA_VERSION,
                times=parquet.times,
                universe_snapshot_sha256=universe_snapshot_sha256,
                source_manifest_ids=source_ids,
                lineage=ManifestLineage(),
            )
            manifest_path = repository.publish_manifest(manifest, lease)
            return PublishedMinutePartition(
                quality_path,
                parquet_path,
                manifest_path,
                manifest.manifest_id,
                quality.sha256,
                parquet.sha256,
                parquet.record_count,
            )
        finally:
            parquet_stage.unlink(missing_ok=True)
            report_stage.unlink(missing_ok=True)
