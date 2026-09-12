"""Point-in-time instrument, calendar, and corporate-action reference data."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import resource
import tempfile
import time
from dataclasses import dataclass
from datetime import UTC, date, datetime, timedelta
from enum import StrEnum
from itertools import pairwise
from math import gcd
from pathlib import Path
from typing import TYPE_CHECKING, Final, Protocol, cast
from zoneinfo import ZoneInfo

from aegis_mx_intelligence import Identifier128, InstrumentId

from aegis_mx_research.forecast_contracts import (
    InstrumentResolution,
    InstrumentResolutionCode,
    InstrumentResolver,
    load_ticker_universe,
)

if TYPE_CHECKING:
    from collections.abc import Callable, Mapping, Sequence

REFERENCE_SCHEMA_VERSION: Final = "1.0.0"
REFERENCE_REPORT_SCHEMA_VERSION: Final = "1.0.0"
DEFAULT_DATA_ROOT: Final = Path("/scratch/djy8hg/aegis_mx_poc_data")
DEFAULT_BACKFILL_REPORT: Final = (
    DEFAULT_DATA_ROOT / "reports/alpaca-iex-minute/backfill/report.json"
)
DEFAULT_REFERENCE_DIRECTORY: Final = (
    DEFAULT_DATA_ROOT / "reports/reference-data/prompt-52"
)
MAX_INPUT_BYTES: Final = 64 << 20
MAX_INT64: Final = (1 << 63) - 1
MAX_TEXT_BYTES: Final = 256
REGULAR_CLOSE_HOUR: Final = 16
WEEKEND_START_DAY: Final = 5
MAX_BENCHMARK_ITERATIONS: Final = 10_000
NANOSECONDS_PER_SECOND: Final = 1_000_000_000
SESSION_ZONE: Final = ZoneInfo("America/New_York")
_SYMBOL = re.compile(r"^[A-Z][A-Z0-9]*(?:[.-][A-Z0-9]+)?$")
_SHA256 = re.compile(r"^[a-f0-9]{64}$")


class ReferenceErrorCode(StrEnum):
    """Stable fail-closed error reasons."""

    INVALID_INPUT = "INVALID_INPUT"
    INTEGRITY_FAILURE = "INTEGRITY_FAILURE"
    CONFLICT = "CONFLICT"
    MISSING_CALENDAR = "MISSING_CALENDAR"
    INEXACT_ARITHMETIC = "INEXACT_ARITHMETIC"
    INTEGER_OVERFLOW = "INTEGER_OVERFLOW"
    UNSUPPORTED_SCHEMA = "UNSUPPORTED_SCHEMA"
    UNRESOLVED = "UNRESOLVED"


class ReferenceDataError(ValueError):
    """Reference-data failure with a machine-readable reason."""

    def __init__(self, code: ReferenceErrorCode, message: str) -> None:
        """Create a bounded failure retaining its stable code."""
        self.code = code
        super().__init__(f"{code.value}: {message}"[:512])


class ListingVenue(StrEnum):
    """Provider-neutral listing venue classification."""

    NASDAQ = "NASDAQ"
    NYSE = "NYSE"
    NYSE_AMERICAN = "NYSE_AMERICAN"
    OTC = "OTC"
    UNKNOWN = "UNKNOWN"


class SecurityType(StrEnum):
    """Security subtype; UNKNOWN is retained when a source is not specific."""

    COMMON_STOCK = "COMMON_STOCK"
    ADR = "ADR"
    ETF = "ETF"
    PREFERRED_STOCK = "PREFERRED_STOCK"
    UNIT = "UNIT"
    WARRANT = "WARRANT"
    US_EQUITY_UNSPECIFIED = "US_EQUITY_UNSPECIFIED"
    UNKNOWN = "UNKNOWN"


class MappingStatus(StrEnum):
    """Point-in-time mapping disposition."""

    RESOLVED_CURRENT_ONLY = "RESOLVED_CURRENT_ONLY"
    RECENTLY_LISTED = "RECENTLY_LISTED"
    DELISTED = "DELISTED"
    UNSUPPORTED_VENUE = "UNSUPPORTED_VENUE"
    AMBIGUOUS = "AMBIGUOUS"
    MISSING_REFERENCE = "MISSING_REFERENCE"


class CorporateActionKind(StrEnum):
    """Supported exact corporate-action payloads."""

    SPLIT = "SPLIT"
    CASH_DIVIDEND = "CASH_DIVIDEND"


class CalendarDayKind(StrEnum):
    """Explicit calendar-day state without inventing closure reasons."""

    REGULAR_SESSION = "REGULAR_SESSION"
    EARLY_CLOSE_SESSION = "EARLY_CLOSE_SESSION"
    WEEKEND = "WEEKEND"
    HOLIDAY = "HOLIDAY"
    UNCLASSIFIED_CLOSURE = "UNCLASSIFIED_CLOSURE"


class RoundingPolicy(StrEnum):
    """Explicit integer conversion behavior for exact rational arithmetic."""

    REJECT_INEXACT = "REJECT_INEXACT"
    FLOOR = "FLOOR"
    HALF_EVEN = "HALF_EVEN"


def _bounded_text(value: str, field: str, maximum: int = MAX_TEXT_BYTES) -> None:
    if not value or "\x00" in value or len(value.encode()) > maximum:
        raise ReferenceDataError(
            ReferenceErrorCode.INVALID_INPUT,
            f"{field} is empty, contains NUL, or exceeds {maximum} bytes",
        )


def _positive_ns(value: int, field: str) -> None:
    if isinstance(value, bool) or not 0 < value <= MAX_INT64:
        raise ReferenceDataError(
            ReferenceErrorCode.INVALID_INPUT, f"{field} is not a positive int64"
        )


def _checked_mul(left: int, right: int) -> int:
    result = left * right
    if not -MAX_INT64 <= result <= MAX_INT64:
        raise ReferenceDataError(
            ReferenceErrorCode.INTEGER_OVERFLOW, "signed int64 multiplication overflow"
        )
    return result


def _canonical_bytes(value: object) -> bytes:
    return json.dumps(
        value, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode("ascii")


def _document(payload: Mapping[str, object]) -> dict[str, object]:
    body = dict(payload)
    body.pop("document_sha256", None)
    return {
        **body,
        "document_sha256": hashlib.sha256(_canonical_bytes(body)).hexdigest(),
    }


def _verify_document(value: object, *, expected_schema: str) -> dict[str, object]:
    if not isinstance(value, dict) or not all(isinstance(key, str) for key in value):
        raise ReferenceDataError(
            ReferenceErrorCode.INVALID_INPUT, "document is not an object"
        )
    document = cast("dict[str, object]", value)
    supplied = document.get("document_sha256")
    body = dict(document)
    body.pop("document_sha256", None)
    actual = hashlib.sha256(_canonical_bytes(body)).hexdigest()
    if supplied != actual:
        raise ReferenceDataError(
            ReferenceErrorCode.INTEGRITY_FAILURE, "document SHA-256 does not match"
        )
    if document.get("schema_version") != expected_schema:
        raise ReferenceDataError(
            ReferenceErrorCode.UNSUPPORTED_SCHEMA, "schema version is not supported"
        )
    return document


def _instrument_hex(instrument_id: InstrumentId) -> str:
    return instrument_id.hex()


def alpaca_instrument_id(asset_id: str) -> InstrumentId:
    """Derive the stable ID already used by canonical Alpaca minute records."""
    _bounded_text(asset_id, "asset_id", 128)
    try:
        asset_id.encode("ascii")
    except UnicodeEncodeError as error:
        raise ReferenceDataError(
            ReferenceErrorCode.INVALID_INPUT, "asset_id must be ASCII"
        ) from error
    digest = hashlib.sha256(f"ALPACA-ASSET-V1:{asset_id}".encode("ascii")).digest()
    return InstrumentId(
        Identifier128(
            int.from_bytes(digest[:8], "big"),
            int.from_bytes(digest[8:16], "big"),
        )
    )


@dataclass(frozen=True, slots=True)
class ExactRatio:
    """Canonical positive rational with checked signed-int64 operations."""

    numerator: int
    denominator: int

    def __post_init__(self) -> None:
        """Require positive reduced int64 terms."""
        if not 0 < self.numerator <= MAX_INT64 or not 0 < self.denominator <= MAX_INT64:
            raise ReferenceDataError(
                ReferenceErrorCode.INVALID_INPUT, "ratio terms must be positive int64"
            )
        if gcd(self.numerator, self.denominator) != 1:
            raise ReferenceDataError(
                ReferenceErrorCode.INVALID_INPUT, "ratio must be reduced"
            )

    @classmethod
    def reduced(cls, numerator: int, denominator: int) -> ExactRatio:
        """Construct the unique reduced form of a positive ratio."""
        if numerator <= 0 or denominator <= 0:
            raise ReferenceDataError(
                ReferenceErrorCode.INVALID_INPUT, "ratio terms must be positive"
            )
        divisor = gcd(numerator, denominator)
        return cls(numerator // divisor, denominator // divisor)

    def inverse(self) -> ExactRatio:
        """Return an exact reciprocal."""
        return ExactRatio(self.denominator, self.numerator)

    def multiply(self, other: ExactRatio) -> ExactRatio:
        """Cross-cancel and multiply without hiding int64 overflow."""
        left_divisor = gcd(self.numerator, other.denominator)
        right_divisor = gcd(other.numerator, self.denominator)
        numerator = _checked_mul(
            self.numerator // left_divisor, other.numerator // right_divisor
        )
        denominator = _checked_mul(
            self.denominator // right_divisor, other.denominator // left_divisor
        )
        return ExactRatio.reduced(numerator, denominator)

    def apply(self, value: int, rounding: RoundingPolicy) -> int:
        """Apply the ratio to a nonnegative integer under an explicit policy."""
        if isinstance(value, bool) or not 0 <= value <= MAX_INT64:
            raise ReferenceDataError(
                ReferenceErrorCode.INVALID_INPUT, "value must be a nonnegative int64"
            )
        product = _checked_mul(value, self.numerator)
        quotient, remainder = divmod(product, self.denominator)
        if remainder == 0:
            return quotient
        if rounding is RoundingPolicy.REJECT_INEXACT:
            raise ReferenceDataError(
                ReferenceErrorCode.INEXACT_ARITHMETIC,
                "rational result is not an exact integer",
            )
        if rounding is RoundingPolicy.HALF_EVEN:
            doubled = remainder * 2
            if doubled > self.denominator or (
                doubled == self.denominator and quotient % 2 == 1
            ):
                quotient += 1
        return quotient


@dataclass(frozen=True, slots=True)
class BusinessInterval:
    """Half-open interval for economic effectiveness."""

    effective_from_ns: int
    effective_to_ns: int | None = None

    def __post_init__(self) -> None:
        """Require a positive nonempty half-open interval."""
        _positive_ns(self.effective_from_ns, "effective_from_ns")
        if self.effective_to_ns is not None:
            _positive_ns(self.effective_to_ns, "effective_to_ns")
            if self.effective_to_ns <= self.effective_from_ns:
                raise ReferenceDataError(
                    ReferenceErrorCode.INVALID_INPUT, "business interval is empty"
                )

    def contains(self, timestamp_ns: int) -> bool:
        """Return whether an event time falls in the interval."""
        return self.effective_from_ns <= timestamp_ns and (
            self.effective_to_ns is None or timestamp_ns < self.effective_to_ns
        )


@dataclass(frozen=True, slots=True)
class ReferenceProvenance:
    """Source identity and bitemporal availability for one immutable revision."""

    provider: str
    dataset: str
    document_id: str
    content_sha256: str
    observed_at_ns: int
    processed_at_ns: int
    revision_at_ns: int
    source_publication_time_ns: int | None
    historical_completeness: bool
    limitation: str

    def __post_init__(self) -> None:
        """Validate bounded identity, digest, and timestamp ordering."""
        for field, value in (
            ("provider", self.provider),
            ("dataset", self.dataset),
            ("document_id", self.document_id),
            ("limitation", self.limitation),
        ):
            _bounded_text(value, field, 512 if field == "limitation" else 256)
        if _SHA256.fullmatch(self.content_sha256) is None:
            raise ReferenceDataError(
                ReferenceErrorCode.INVALID_INPUT, "content_sha256 is malformed"
            )
        for time_field, time_value in (
            ("observed_at_ns", self.observed_at_ns),
            ("processed_at_ns", self.processed_at_ns),
            ("revision_at_ns", self.revision_at_ns),
        ):
            _positive_ns(time_value, time_field)
        if self.processed_at_ns < self.observed_at_ns:
            raise ReferenceDataError(
                ReferenceErrorCode.INVALID_INPUT, "processing precedes observation"
            )
        if self.source_publication_time_ns is not None:
            _positive_ns(self.source_publication_time_ns, "source_publication_time_ns")
            if self.observed_at_ns < self.source_publication_time_ns:
                raise ReferenceDataError(
                    ReferenceErrorCode.INVALID_INPUT, "observation precedes publication"
                )
            if self.revision_at_ns < self.source_publication_time_ns:
                raise ReferenceDataError(
                    ReferenceErrorCode.INVALID_INPUT, "revision precedes publication"
                )

    @property
    def available_at_ns(self) -> int:
        """Return the first safe local knowledge time."""
        return max(self.observed_at_ns, self.processed_at_ns, self.revision_at_ns)


@dataclass(frozen=True, slots=True)
class InstrumentRevision:
    """One immutable instrument-master revision."""

    revision_id: str
    instrument_id: InstrumentId
    primary_symbol: str
    listing_venue: ListingVenue
    security_type: SecurityType
    listing_date: date | None
    delisting_date: date | None
    effective: BusinessInterval
    version: int
    unresolved_fields: tuple[str, ...]
    provenance: ReferenceProvenance

    def __post_init__(self) -> None:
        """Validate immutable instrument identity and date bounds."""
        _bounded_text(self.revision_id, "revision_id")
        if _SYMBOL.fullmatch(self.primary_symbol) is None:
            raise ReferenceDataError(
                ReferenceErrorCode.INVALID_INPUT, "primary_symbol is malformed"
            )
        if self.version <= 0:
            raise ReferenceDataError(
                ReferenceErrorCode.INVALID_INPUT, "instrument version must be positive"
            )
        if (
            self.listing_date
            and self.delisting_date
            and self.delisting_date < self.listing_date
        ):
            raise ReferenceDataError(
                ReferenceErrorCode.INVALID_INPUT, "delisting precedes listing"
            )
        if len(set(self.unresolved_fields)) != len(self.unresolved_fields):
            raise ReferenceDataError(
                ReferenceErrorCode.INVALID_INPUT, "unresolved fields are duplicated"
            )


@dataclass(frozen=True, slots=True)
class SymbolMappingRevision:
    """One symbol mapping revision; aliases are never followed implicitly."""

    mapping_id: str
    symbol: str
    instrument_id: InstrumentId | None
    listing_venue: ListingVenue
    status: MappingStatus
    effective: BusinessInterval
    version: int
    provenance: ReferenceProvenance

    def __post_init__(self) -> None:
        """Validate mapping identity, symbol, and resolution status."""
        _bounded_text(self.mapping_id, "mapping_id")
        if _SYMBOL.fullmatch(self.symbol) is None:
            raise ReferenceDataError(
                ReferenceErrorCode.INVALID_INPUT, "mapping symbol is malformed"
            )
        if self.version <= 0:
            raise ReferenceDataError(
                ReferenceErrorCode.INVALID_INPUT, "mapping version must be positive"
            )
        needs_id = self.status in {
            MappingStatus.RESOLVED_CURRENT_ONLY,
            MappingStatus.RECENTLY_LISTED,
            MappingStatus.DELISTED,
            MappingStatus.UNSUPPORTED_VENUE,
        }
        if needs_id != (self.instrument_id is not None):
            raise ReferenceDataError(
                ReferenceErrorCode.INVALID_INPUT,
                "mapping identity and status disagree",
            )


@dataclass(frozen=True, slots=True)
class CorporateActionRevision:
    """One immutable split or dividend revision with exact units."""

    action_id: str
    instrument_id: InstrumentId
    kind: CorporateActionKind
    effective_time_ns: int
    version: int
    provenance: ReferenceProvenance
    split_new_shares_per_old: ExactRatio | None = None
    cash_dividend_currency_nanos: int | None = None
    currency: str | None = None

    def __post_init__(self) -> None:
        """Require exactly the fields belonging to the action kind."""
        _bounded_text(self.action_id, "action_id")
        _positive_ns(self.effective_time_ns, "effective_time_ns")
        if self.version <= 0:
            raise ReferenceDataError(
                ReferenceErrorCode.INVALID_INPUT, "action version must be positive"
            )
        if self.kind is CorporateActionKind.SPLIT:
            if self.split_new_shares_per_old is None or any(
                value is not None
                for value in (self.cash_dividend_currency_nanos, self.currency)
            ):
                raise ReferenceDataError(
                    ReferenceErrorCode.INVALID_INPUT, "split payload is inconsistent"
                )
        else:
            if (
                self.split_new_shares_per_old is not None
                or self.cash_dividend_currency_nanos is None
                or not 0 <= self.cash_dividend_currency_nanos <= MAX_INT64
                or self.currency is None
            ):
                raise ReferenceDataError(
                    ReferenceErrorCode.INVALID_INPUT, "dividend payload is inconsistent"
                )
            _bounded_text(self.currency, "currency", 8)


@dataclass(frozen=True, slots=True)
class CalendarDayRevision:
    """One explicit market date and its known session or closure state."""

    calendar_id: str
    session_date: date
    kind: CalendarDayKind
    open_exchange_time_ns: int | None
    close_exchange_time_ns: int | None
    version: int
    provenance: ReferenceProvenance

    def __post_init__(self) -> None:
        """Validate explicit session bounds or an explicit closure."""
        _bounded_text(self.calendar_id, "calendar_id", 64)
        if self.version <= 0:
            raise ReferenceDataError(
                ReferenceErrorCode.INVALID_INPUT, "calendar version must be positive"
            )
        is_session = self.kind in {
            CalendarDayKind.REGULAR_SESSION,
            CalendarDayKind.EARLY_CLOSE_SESSION,
        }
        if is_session:
            if (
                self.open_exchange_time_ns is None
                or self.close_exchange_time_ns is None
            ):
                raise ReferenceDataError(
                    ReferenceErrorCode.INVALID_INPUT,
                    "session lacks explicit timestamps",
                )
            _positive_ns(self.open_exchange_time_ns, "open_exchange_time_ns")
            _positive_ns(self.close_exchange_time_ns, "close_exchange_time_ns")
            if self.close_exchange_time_ns <= self.open_exchange_time_ns:
                raise ReferenceDataError(
                    ReferenceErrorCode.INVALID_INPUT, "calendar session is reversed"
                )
        elif (
            self.open_exchange_time_ns is not None
            or self.close_exchange_time_ns is not None
        ):
            raise ReferenceDataError(
                ReferenceErrorCode.INVALID_INPUT,
                "closed day carries session timestamps",
            )


@dataclass(frozen=True, slots=True)
class HaltRevision:
    """Known half-open halt interval for one instrument."""

    halt_id: str
    instrument_id: InstrumentId
    start_exchange_time_ns: int
    end_exchange_time_ns: int
    version: int
    provenance: ReferenceProvenance

    def __post_init__(self) -> None:
        """Require a positive, nonempty halt interval."""
        _bounded_text(self.halt_id, "halt_id")
        _positive_ns(self.start_exchange_time_ns, "start_exchange_time_ns")
        _positive_ns(self.end_exchange_time_ns, "end_exchange_time_ns")
        if (
            self.end_exchange_time_ns <= self.start_exchange_time_ns
            or self.version <= 0
        ):
            raise ReferenceDataError(
                ReferenceErrorCode.INVALID_INPUT, "halt interval/version is invalid"
            )


@dataclass(frozen=True, slots=True)
class ReferenceSnapshot:
    """Bounded immutable reference snapshot for one universe."""

    universe_source_sha256: str
    source_report_sha256: str
    as_of_ns: int
    instruments: tuple[InstrumentRevision, ...]
    mappings: tuple[SymbolMappingRevision, ...]
    corporate_actions: tuple[CorporateActionRevision, ...]
    calendar_days: tuple[CalendarDayRevision, ...]
    halts: tuple[HaltRevision, ...]
    halt_coverage_complete: bool
    schema_version: str = REFERENCE_SCHEMA_VERSION

    def __post_init__(self) -> None:
        """Validate version, hashes, time, and minimum coverage."""
        if self.schema_version != REFERENCE_SCHEMA_VERSION:
            raise ReferenceDataError(
                ReferenceErrorCode.UNSUPPORTED_SCHEMA, "snapshot version is unsupported"
            )
        if any(
            _SHA256.fullmatch(value) is None
            for value in (
                self.universe_source_sha256,
                self.source_report_sha256,
            )
        ):
            raise ReferenceDataError(
                ReferenceErrorCode.INVALID_INPUT, "snapshot source hash is malformed"
            )
        _positive_ns(self.as_of_ns, "as_of_ns")
        if not self.mappings or not self.calendar_days:
            raise ReferenceDataError(
                ReferenceErrorCode.INVALID_INPUT,
                "snapshot requires mappings and calendar coverage",
            )


type ReferenceRevision = (
    InstrumentRevision
    | SymbolMappingRevision
    | CorporateActionRevision
    | CalendarDayRevision
    | HaltRevision
)


class _VersionedReference(Protocol):
    @property
    def version(self) -> int:
        """Return the positive revision number."""

    @property
    def provenance(self) -> ReferenceProvenance:
        """Return immutable source and availability evidence."""


def _revision_identity(record: ReferenceRevision) -> str:
    if isinstance(record, InstrumentRevision):
        return record.revision_id
    if isinstance(record, SymbolMappingRevision):
        return record.mapping_id
    if isinstance(record, CorporateActionRevision):
        return record.action_id
    if isinstance(record, CalendarDayRevision):
        return f"{record.calendar_id}:{record.session_date.isoformat()}"
    return record.halt_id


class PointInTimeReferenceStore:
    """Deterministic read-only bitemporal index over one immutable snapshot."""

    def __init__(self, snapshot: ReferenceSnapshot) -> None:
        """Validate and index an immutable snapshot."""
        self.snapshot = snapshot
        self._validate_lineage()
        self._mappings_by_symbol: dict[str, list[SymbolMappingRevision]] = {}
        self._instruments_by_id: dict[InstrumentId, list[InstrumentRevision]] = {}
        self._actions_by_instrument: dict[
            InstrumentId, list[CorporateActionRevision]
        ] = {}
        self._calendar_by_date: dict[date, list[CalendarDayRevision]] = {}
        self._halts_by_instrument: dict[InstrumentId, list[HaltRevision]] = {}
        for instrument in snapshot.instruments:
            self._instruments_by_id.setdefault(instrument.instrument_id, []).append(
                instrument
            )
        for mapping in snapshot.mappings:
            self._mappings_by_symbol.setdefault(mapping.symbol, []).append(mapping)
        for action in snapshot.corporate_actions:
            self._actions_by_instrument.setdefault(action.instrument_id, []).append(
                action
            )
        for calendar_day in snapshot.calendar_days:
            self._calendar_by_date.setdefault(calendar_day.session_date, []).append(
                calendar_day
            )
        for halt in snapshot.halts:
            self._halts_by_instrument.setdefault(halt.instrument_id, []).append(halt)

    def _validate_lineage(self) -> None:
        groups: dict[tuple[str, str], list[tuple[int, int]]] = {}
        revision_groups: tuple[tuple[str, Sequence[ReferenceRevision]], ...] = (
            ("instrument", self.snapshot.instruments),
            ("mapping", self.snapshot.mappings),
            ("action", self.snapshot.corporate_actions),
            ("calendar", self.snapshot.calendar_days),
            ("halt", self.snapshot.halts),
        )
        for record_type, records in revision_groups:
            for record in records:
                revision_identity = _revision_identity(record)
                groups.setdefault((record_type, revision_identity), []).append(
                    (record.version, record.provenance.revision_at_ns)
                )
        for group_identity, versions in groups.items():
            ordered = sorted(versions)
            if [item[0] for item in ordered] != list(range(1, len(ordered) + 1)):
                raise ReferenceDataError(
                    ReferenceErrorCode.CONFLICT,
                    (
                        "noncontiguous revisions for "
                        f"{group_identity[0]}:{group_identity[1]}"
                    ),
                )
            if any(
                current[1] <= previous[1] for previous, current in pairwise(ordered)
            ):
                raise ReferenceDataError(
                    ReferenceErrorCode.CONFLICT,
                    (
                        "revision time does not increase for "
                        f"{group_identity[0]}:{group_identity[1]}"
                    ),
                )

    @staticmethod
    def _known_latest[T: _VersionedReference](
        records: Sequence[T], known_at_ns: int
    ) -> tuple[T, ...]:
        _positive_ns(known_at_ns, "known_at_ns")
        latest: dict[str, T] = {}
        for record in sorted(
            records,
            key=lambda item: (
                item.provenance.available_at_ns,
                item.version,
            ),
        ):
            if record.provenance.available_at_ns <= known_at_ns:
                identity = _revision_identity(cast("ReferenceRevision", record))
                latest[identity] = record
        return tuple(latest[key] for key in sorted(latest))

    def symbol_mapping_at(
        self, symbol: str, *, effective_at_ns: int, known_at_ns: int
    ) -> SymbolMappingRevision | None:
        """Resolve exactly one symbol without following aliases."""
        candidates = [
            record
            for record in self._known_latest(
                self._mappings_by_symbol.get(symbol, ()), known_at_ns
            )
            if record.effective.contains(effective_at_ns)
        ]
        identities = {
            _instrument_hex(item.instrument_id)
            for item in candidates
            if item.instrument_id
        }
        if len(candidates) > 1 and len(identities) > 1:
            raise ReferenceDataError(
                ReferenceErrorCode.CONFLICT, "overlapping symbol mappings disagree"
            )
        if not candidates:
            return None
        return max(candidates, key=lambda item: (item.version, item.mapping_id))

    def instrument_at(
        self, instrument_id: InstrumentId, *, effective_at_ns: int, known_at_ns: int
    ) -> InstrumentRevision | None:
        """Return one instrument revision effective and known at both cutoffs."""
        candidates = [
            record
            for record in self._known_latest(
                self._instruments_by_id.get(instrument_id, ()), known_at_ns
            )
            if record.effective.contains(effective_at_ns)
        ]
        if not candidates:
            return None
        identities = {
            (
                item.primary_symbol,
                item.listing_venue,
                item.security_type,
                item.listing_date,
                item.delisting_date,
            )
            for item in candidates
        }
        if len(identities) != 1:
            raise ReferenceDataError(
                ReferenceErrorCode.CONFLICT, "instrument revisions conflict"
            )
        return max(candidates, key=lambda item: (item.version, item.revision_id))

    def corporate_actions_at(
        self, instrument_id: InstrumentId, *, effective_at_ns: int, known_at_ns: int
    ) -> tuple[CorporateActionRevision, ...]:
        """Return actions both effective and known at the supplied cutoffs."""
        return tuple(
            sorted(
                (
                    action
                    for action in self._known_latest(
                        self._actions_by_instrument.get(instrument_id, ()), known_at_ns
                    )
                    if action.effective_time_ns <= effective_at_ns
                ),
                key=lambda item: (item.effective_time_ns, item.action_id),
            )
        )

    def adjust_price_for_splits(  # noqa: PLR0913
        self,
        price_currency_nanos: int,
        instrument_id: InstrumentId,
        *,
        after_ns: int,
        through_ns: int,
        known_at_ns: int,
        rounding: RoundingPolicy = RoundingPolicy.REJECT_INEXACT,
    ) -> int:
        """Express a pre-action price on the post-action share basis."""
        if through_ns < after_ns:
            raise ReferenceDataError(
                ReferenceErrorCode.INVALID_INPUT, "adjustment interval is reversed"
            )
        multiplier = ExactRatio(1, 1)
        for action in self.corporate_actions_at(
            instrument_id, effective_at_ns=through_ns, known_at_ns=known_at_ns
        ):
            if (
                action.kind is CorporateActionKind.SPLIT
                and after_ns < action.effective_time_ns <= through_ns
                and action.split_new_shares_per_old is not None
            ):
                multiplier = multiplier.multiply(
                    action.split_new_shares_per_old.inverse()
                )
        return multiplier.apply(price_currency_nanos, rounding)

    def cash_dividends_at(
        self, instrument_id: InstrumentId, *, effective_at_ns: int, known_at_ns: int
    ) -> tuple[CorporateActionRevision, ...]:
        """Return dividend metadata separately from split price adjustment."""
        return tuple(
            action
            for action in self.corporate_actions_at(
                instrument_id,
                effective_at_ns=effective_at_ns,
                known_at_ns=known_at_ns,
            )
            if action.kind is CorporateActionKind.CASH_DIVIDEND
        )

    def halts_at(
        self,
        instrument_id: InstrumentId,
        *,
        start_exchange_time_ns: int,
        end_exchange_time_ns: int,
        known_at_ns: int,
    ) -> tuple[HaltRevision, ...]:
        """Return known halts intersecting one half-open event-time interval."""
        if end_exchange_time_ns <= start_exchange_time_ns:
            raise ReferenceDataError(
                ReferenceErrorCode.INVALID_INPUT, "halt query interval is empty"
            )
        return tuple(
            sorted(
                (
                    halt
                    for halt in self._known_latest(
                        self._halts_by_instrument.get(instrument_id, ()), known_at_ns
                    )
                    if halt.start_exchange_time_ns < end_exchange_time_ns
                    and halt.end_exchange_time_ns > start_exchange_time_ns
                ),
                key=lambda item: (item.start_exchange_time_ns, item.halt_id),
            )
        )

    def calendar_day_at(
        self, session_date: date, *, known_at_ns: int
    ) -> CalendarDayRevision:
        """Return an explicit session or closure, rejecting missing coverage."""
        candidates = list(
            self._known_latest(
                self._calendar_by_date.get(session_date, ()), known_at_ns
            )
        )
        if not candidates:
            raise ReferenceDataError(
                ReferenceErrorCode.MISSING_CALENDAR,
                f"calendar date {session_date.isoformat()} is unavailable",
            )
        states = {
            (item.kind, item.open_exchange_time_ns, item.close_exchange_time_ns)
            for item in candidates
        }
        if len(states) != 1:
            raise ReferenceDataError(
                ReferenceErrorCode.CONFLICT, "calendar sources conflict"
            )
        return max(candidates, key=lambda item: item.version)

    def as_universe_resolver(
        self, *, effective_at_ns: int, known_at_ns: int
    ) -> InstrumentResolver:
        """Bind point-in-time cutoffs into the existing universe contract."""
        return _BoundReferenceResolver(self, effective_at_ns, known_at_ns)


class _BoundReferenceResolver:
    def __init__(
        self,
        store: PointInTimeReferenceStore,
        effective_at_ns: int,
        known_at_ns: int,
    ) -> None:
        self._store = store
        self._effective_at_ns = effective_at_ns
        self._known_at_ns = known_at_ns

    def resolve(self, symbol: str) -> InstrumentResolution:
        mapping = self._store.symbol_mapping_at(
            symbol,
            effective_at_ns=self._effective_at_ns,
            known_at_ns=self._known_at_ns,
        )
        if mapping is None:
            return InstrumentResolution(
                InstrumentResolutionCode.MISSING_REFERENCE_DATA, None
            )
        code = {
            MappingStatus.RESOLVED_CURRENT_ONLY: InstrumentResolutionCode.RESOLVED,
            MappingStatus.RECENTLY_LISTED: InstrumentResolutionCode.RECENTLY_LISTED,
            MappingStatus.DELISTED: InstrumentResolutionCode.DELISTED,
            MappingStatus.UNSUPPORTED_VENUE: InstrumentResolutionCode.UNSUPPORTED,
            MappingStatus.AMBIGUOUS: InstrumentResolutionCode.AMBIGUOUS_REFERENCE,
            MappingStatus.MISSING_REFERENCE: (
                InstrumentResolutionCode.MISSING_REFERENCE_DATA
            ),
        }[mapping.status]
        instrument_id = (
            mapping.instrument_id if code is InstrumentResolutionCode.RESOLVED else None
        )
        return InstrumentResolution(code, instrument_id)


def _utc_ns(value: str) -> int:
    if not isinstance(value, str) or not value.endswith("Z"):
        raise ReferenceDataError(
            ReferenceErrorCode.INVALID_INPUT, "timestamp is not explicit UTC"
        )
    try:
        parsed = datetime.fromisoformat(value)
    except ValueError as error:
        raise ReferenceDataError(
            ReferenceErrorCode.INVALID_INPUT, "timestamp is malformed"
        ) from error
    delta = parsed - datetime(1970, 1, 1, tzinfo=UTC)
    result = (
        delta.days * 86_400 * NANOSECONDS_PER_SECOND
        + delta.seconds * NANOSECONDS_PER_SECOND
        + delta.microseconds * 1_000
    )
    _positive_ns(result, "timestamp")
    return result


def _venue(value: object) -> ListingVenue:
    mapping = {
        "NASDAQ": ListingVenue.NASDAQ,
        "NYSE": ListingVenue.NYSE,
        "AMEX": ListingVenue.NYSE_AMERICAN,
        "OTC": ListingVenue.OTC,
    }
    return mapping.get(value if isinstance(value, str) else "", ListingVenue.UNKNOWN)


def _provenance(
    source_sha256: str, observed_at_ns: int, *, dataset: str
) -> ReferenceProvenance:
    return ReferenceProvenance(
        provider="Alpaca",
        dataset=dataset,
        document_id=f"backfill-report:{source_sha256}",
        content_sha256=source_sha256,
        observed_at_ns=observed_at_ns,
        processed_at_ns=observed_at_ns,
        revision_at_ns=observed_at_ns,
        source_publication_time_ns=None,
        historical_completeness=False,
        limitation=(
            "current provider observation retained at report completion; source "
            "publication time and historical revision availability were not observed"
        ),
    )


def _read_hashed_json(path: Path) -> tuple[dict[str, object], str]:
    try:
        with path.open("rb") as source:
            payload = source.read(MAX_INPUT_BYTES + 1)
    except OSError as error:
        raise ReferenceDataError(
            ReferenceErrorCode.INVALID_INPUT, "source report cannot be read"
        ) from error
    if not payload or len(payload) > MAX_INPUT_BYTES:
        raise ReferenceDataError(
            ReferenceErrorCode.INVALID_INPUT, "source report size is invalid"
        )
    try:
        value = json.loads(payload)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ReferenceDataError(
            ReferenceErrorCode.INVALID_INPUT, "source report is malformed JSON"
        ) from error
    document = _verify_document(value, expected_schema="1.0.0")
    return document, hashlib.sha256(payload).hexdigest()


def build_snapshot_from_backfill_report(  # noqa: C901, PLR0912, PLR0915
    path: Path,
) -> ReferenceSnapshot:
    """Build current-only mappings and retrospective calendar evidence offline."""
    report, source_file_sha256 = _read_hashed_json(path)
    if report.get("mode") != "BACKFILL" or report.get("status") != "COMPLETED":
        raise ReferenceDataError(
            ReferenceErrorCode.INVALID_INPUT,
            "source report is not a completed backfill",
        )
    universe = load_ticker_universe(_AllUnresolvedResolver())
    if report.get("ticker_source_sha256") != universe.source_file_sha256.hex():
        raise ReferenceDataError(
            ReferenceErrorCode.INTEGRITY_FAILURE, "ticker.txt digest changed"
        )
    raw_assets = report.get("assets")
    raw_sessions = report.get("sessions")
    if not isinstance(raw_assets, dict) or set(raw_assets) != set(
        universe.canonical_symbols
    ):
        raise ReferenceDataError(
            ReferenceErrorCode.INTEGRITY_FAILURE,
            "asset coverage does not match ticker.txt",
        )
    if not isinstance(raw_sessions, list) or not raw_sessions:
        raise ReferenceDataError(
            ReferenceErrorCode.INVALID_INPUT, "calendar sessions are unavailable"
        )
    observed_at_ns = _utc_ns(cast("str", report.get("finished_at_utc")))
    asset_provenance = _provenance(
        source_file_sha256, observed_at_ns, dataset="Alpaca current asset endpoint"
    )
    calendar_provenance = _provenance(
        cast("str", report.get("calendar_source_response_sha256")),
        observed_at_ns,
        dataset="Alpaca market calendar response",
    )

    instruments: list[InstrumentRevision] = []
    mappings: list[SymbolMappingRevision] = []
    for symbol in universe.canonical_symbols:
        raw = raw_assets[symbol]
        if not isinstance(raw, dict):
            raise ReferenceDataError(
                ReferenceErrorCode.INVALID_INPUT, "asset record is malformed"
            )
        asset_id = raw.get("asset_id")
        venue = _venue(raw.get("exchange"))
        status = raw.get("status")
        if not isinstance(asset_id, str):
            mapping_status = MappingStatus.MISSING_REFERENCE
            instrument_id = None
        else:
            instrument_id = alpaca_instrument_id(asset_id)
            mapping_status = (
                MappingStatus.UNSUPPORTED_VENUE
                if status != "RESOLVED" or venue is ListingVenue.OTC
                else MappingStatus.RESOLVED_CURRENT_ONLY
            )
            instruments.append(
                InstrumentRevision(
                    revision_id=f"alpaca-current:{_instrument_hex(instrument_id)}",
                    instrument_id=instrument_id,
                    primary_symbol=symbol,
                    listing_venue=venue,
                    security_type=(
                        SecurityType.US_EQUITY_UNSPECIFIED
                        if raw.get("asset_class") == "us_equity"
                        else SecurityType.UNKNOWN
                    ),
                    listing_date=None,
                    delisting_date=None,
                    effective=BusinessInterval(observed_at_ns),
                    version=1,
                    unresolved_fields=(
                        "historical_symbol_mappings",
                        "listing_date",
                        "delisting_date",
                        "security_subtype",
                        "split_history",
                        "dividend_history",
                    ),
                    provenance=asset_provenance,
                )
            )
        mappings.append(
            SymbolMappingRevision(
                mapping_id=f"alpaca-current-symbol:{symbol}",
                symbol=symbol,
                instrument_id=instrument_id,
                listing_venue=venue,
                status=mapping_status,
                effective=BusinessInterval(observed_at_ns),
                version=1,
                provenance=asset_provenance,
            )
        )

    sessions: dict[date, tuple[int, int]] = {}
    for raw in raw_sessions:
        if not isinstance(raw, dict):
            raise ReferenceDataError(
                ReferenceErrorCode.INVALID_INPUT, "calendar entry is malformed"
            )
        try:
            session_date = date.fromisoformat(cast("str", raw["date"]))
            opened_ns = _utc_ns(cast("str", raw["open_utc"]))
            closed_ns = _utc_ns(cast("str", raw["close_utc"]))
        except (KeyError, TypeError, ValueError) as error:
            raise ReferenceDataError(
                ReferenceErrorCode.INVALID_INPUT, "calendar entry fields are malformed"
            ) from error
        if session_date in sessions or closed_ns <= opened_ns:
            raise ReferenceDataError(
                ReferenceErrorCode.CONFLICT, "calendar sessions duplicate or overlap"
            )
        sessions[session_date] = (opened_ns, closed_ns)
    start_date = date.fromisoformat(cast("str", report["start_date"]))
    end_date = date.fromisoformat(cast("str", report["end_date"]))
    if min(sessions) != start_date or max(sessions) != end_date:
        raise ReferenceDataError(
            ReferenceErrorCode.INTEGRITY_FAILURE, "calendar range is incomplete"
        )
    calendar_days: list[CalendarDayRevision] = []
    current = start_date
    while current <= end_date:
        bounds = sessions.get(current)
        day_open_ns: int | None
        day_close_ns: int | None
        if bounds is not None:
            close_local = datetime.fromtimestamp(
                bounds[1] // NANOSECONDS_PER_SECOND, tz=UTC
            ).astimezone(SESSION_ZONE)
            kind = (
                CalendarDayKind.EARLY_CLOSE_SESSION
                if close_local.hour < REGULAR_CLOSE_HOUR
                else CalendarDayKind.REGULAR_SESSION
            )
            day_open_ns, day_close_ns = bounds
        else:
            kind = (
                CalendarDayKind.WEEKEND
                if current.weekday() >= WEEKEND_START_DAY
                else CalendarDayKind.UNCLASSIFIED_CLOSURE
            )
            day_open_ns = None
            day_close_ns = None
        calendar_days.append(
            CalendarDayRevision(
                calendar_id="US-EQUITIES-ALPACA-RETROSPECTIVE",
                session_date=current,
                kind=kind,
                open_exchange_time_ns=day_open_ns,
                close_exchange_time_ns=day_close_ns,
                version=1,
                provenance=calendar_provenance,
            )
        )
        current += timedelta(days=1)
    return ReferenceSnapshot(
        universe_source_sha256=universe.source_file_sha256.hex(),
        source_report_sha256=source_file_sha256,
        as_of_ns=observed_at_ns,
        instruments=tuple(
            sorted(instruments, key=lambda item: _instrument_hex(item.instrument_id))
        ),
        mappings=tuple(sorted(mappings, key=lambda item: item.symbol)),
        corporate_actions=(),
        calendar_days=tuple(calendar_days),
        halts=(),
        halt_coverage_complete=False,
    )


class _AllUnresolvedResolver:
    def resolve(self, symbol: str) -> InstrumentResolution:
        del symbol
        return InstrumentResolution(
            InstrumentResolutionCode.MISSING_REFERENCE_DATA, None
        )


def _provenance_dict(value: ReferenceProvenance) -> dict[str, object]:
    return {
        "available_at_ns": value.available_at_ns,
        "content_sha256": value.content_sha256,
        "dataset": value.dataset,
        "document_id": value.document_id,
        "historical_completeness": value.historical_completeness,
        "limitation": value.limitation,
        "observed_at_ns": value.observed_at_ns,
        "processed_at_ns": value.processed_at_ns,
        "provider": value.provider,
        "revision_at_ns": value.revision_at_ns,
        "source_publication_time_ns": value.source_publication_time_ns,
    }


def snapshot_document(snapshot: ReferenceSnapshot) -> dict[str, object]:
    """Serialize a snapshot into canonical, checksummed JSON-compatible values."""
    instruments = [
        {
            "delisting_date": item.delisting_date.isoformat()
            if item.delisting_date
            else None,
            "effective_from_ns": item.effective.effective_from_ns,
            "effective_to_ns": item.effective.effective_to_ns,
            "instrument_id": _instrument_hex(item.instrument_id),
            "listing_date": item.listing_date.isoformat()
            if item.listing_date
            else None,
            "listing_venue": item.listing_venue.value,
            "primary_symbol": item.primary_symbol,
            "provenance": _provenance_dict(item.provenance),
            "revision_id": item.revision_id,
            "security_type": item.security_type.value,
            "unresolved_fields": list(item.unresolved_fields),
            "version": item.version,
        }
        for item in snapshot.instruments
    ]
    mappings = [
        {
            "effective_from_ns": item.effective.effective_from_ns,
            "effective_to_ns": item.effective.effective_to_ns,
            "instrument_id": (
                _instrument_hex(item.instrument_id) if item.instrument_id else None
            ),
            "listing_venue": item.listing_venue.value,
            "mapping_id": item.mapping_id,
            "provenance": _provenance_dict(item.provenance),
            "status": item.status.value,
            "symbol": item.symbol,
            "version": item.version,
        }
        for item in snapshot.mappings
    ]
    calendar = [
        {
            "calendar_id": item.calendar_id,
            "close_exchange_time_ns": item.close_exchange_time_ns,
            "kind": item.kind.value,
            "open_exchange_time_ns": item.open_exchange_time_ns,
            "provenance": _provenance_dict(item.provenance),
            "session_date": item.session_date.isoformat(),
            "version": item.version,
        }
        for item in snapshot.calendar_days
    ]
    actions = [
        {
            "action_id": item.action_id,
            "cash_dividend_currency_nanos": item.cash_dividend_currency_nanos,
            "currency": item.currency,
            "effective_time_ns": item.effective_time_ns,
            "instrument_id": _instrument_hex(item.instrument_id),
            "kind": item.kind.value,
            "provenance": _provenance_dict(item.provenance),
            "split_denominator": (
                item.split_new_shares_per_old.denominator
                if item.split_new_shares_per_old
                else None
            ),
            "split_numerator": (
                item.split_new_shares_per_old.numerator
                if item.split_new_shares_per_old
                else None
            ),
            "version": item.version,
        }
        for item in snapshot.corporate_actions
    ]
    halts = [
        {
            "end_exchange_time_ns": item.end_exchange_time_ns,
            "halt_id": item.halt_id,
            "instrument_id": _instrument_hex(item.instrument_id),
            "provenance": _provenance_dict(item.provenance),
            "start_exchange_time_ns": item.start_exchange_time_ns,
            "version": item.version,
        }
        for item in snapshot.halts
    ]
    payload: dict[str, object] = {
        "as_of_ns": snapshot.as_of_ns,
        "calendar_days": calendar,
        "corporate_actions": actions,
        "halt_coverage_complete": snapshot.halt_coverage_complete,
        "halts": halts,
        "instruments": instruments,
        "live_trading_capable": False,
        "mappings": mappings,
        "schema_version": snapshot.schema_version,
        "source_report_sha256": snapshot.source_report_sha256,
        "universe_source_sha256": snapshot.universe_source_sha256,
    }
    return _document(payload)


def resolution_report_document(
    snapshot: ReferenceSnapshot, snapshot_sha256: str
) -> dict[str, object]:
    """Create complete-universe coverage without claiming historical truth."""
    entries = [
        {
            "current_mapping_status": item.status.value,
            "historical_mapping_status": "UNRESOLVED",
            "instrument_id": (
                _instrument_hex(item.instrument_id) if item.instrument_id else None
            ),
            "listing_venue": item.listing_venue.value,
            "symbol": item.symbol,
            "unresolved_fields": [
                "historical_symbol_mappings",
                "listing_date",
                "delisting_date",
                "security_subtype",
                "split_history",
                "dividend_history",
                "halt_history",
            ],
        }
        for item in snapshot.mappings
    ]
    sessions = sum(
        item.kind
        in {CalendarDayKind.REGULAR_SESSION, CalendarDayKind.EARLY_CLOSE_SESSION}
        for item in snapshot.calendar_days
    )
    early_closes = sum(
        item.kind is CalendarDayKind.EARLY_CLOSE_SESSION
        for item in snapshot.calendar_days
    )
    payload: dict[str, object] = {
        "as_of_ns": snapshot.as_of_ns,
        "calendar": {
            "calendar_days": len(snapshot.calendar_days),
            "early_close_sessions": early_closes,
            "halt_coverage_complete": snapshot.halt_coverage_complete,
            "sessions": sessions,
            "weekday_closures_unclassified": sum(
                item.kind is CalendarDayKind.UNCLASSIFIED_CLOSURE
                for item in snapshot.calendar_days
            ),
        },
        "counts": {
            "current_only_resolved": sum(
                item.status is MappingStatus.RESOLVED_CURRENT_ONLY
                for item in snapshot.mappings
            ),
            "historically_complete": 0,
            "requested": len(snapshot.mappings),
            "stable_instrument_ids": sum(
                item.instrument_id is not None for item in snapshot.mappings
            ),
            "unsupported_venue": sum(
                item.status is MappingStatus.UNSUPPORTED_VENUE
                for item in snapshot.mappings
            ),
        },
        "economic_value_claimed": False,
        "entries": entries,
        "live_trading_capable": False,
        "reference_snapshot_document_sha256": snapshot_sha256,
        "safe_for_historical_training": False,
        "schema_version": REFERENCE_REPORT_SCHEMA_VERSION,
        "status": "PARTIAL_REFERENCE_COVERAGE",
        "unresolved_source_boundaries": [
            "Nasdaq Trader persistent automated use is not authorized",
            "SEC current ticker mappings are not historical symbology",
            (
                "authoritative historical actions, delistings, mappings, and "
                "halts are absent"
            ),
        ],
        "universe_source_sha256": snapshot.universe_source_sha256,
    }
    return _document(payload)


def _atomic_write(path: Path, payload: bytes) -> None:
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    if path.exists():
        try:
            existing = path.read_bytes()
        except OSError as error:
            raise ReferenceDataError(
                ReferenceErrorCode.INTEGRITY_FAILURE,
                "existing immutable artifact cannot be verified",
            ) from error
        if existing != payload:
            raise ReferenceDataError(
                ReferenceErrorCode.CONFLICT,
                "immutable artifact path already contains different bytes",
            )
        return
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", dir=path.parent
    )
    temporary = Path(temporary_name)
    try:
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, "wb", closefd=True) as output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
        temporary.replace(path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise


def _write_json(path: Path, value: Mapping[str, object]) -> str:
    encoded = _canonical_bytes(value) + b"\n"
    _atomic_write(path, encoded)
    return hashlib.sha256(encoded).hexdigest()


def _human_report(report: Mapping[str, object]) -> str:
    counts = cast("Mapping[str, object]", report["counts"])
    calendar = cast("Mapping[str, object]", report["calendar"])
    entries = cast("Sequence[Mapping[str, object]]", report["entries"])
    lines = [
        "# Prompt 52 instrument-resolution report",
        "",
        f"- Status: `{report['status']}`",
        f"- Requested symbols: {counts['requested']}",
        f"- Current-only resolved: {counts['current_only_resolved']}",
        f"- Unsupported venue: {counts['unsupported_venue']}",
        f"- Historically complete: {counts['historically_complete']}",
        f"- Calendar sessions: {calendar['sessions']}",
        f"- Early closes: {calendar['early_close_sessions']}",
        f"- Halt coverage complete: {str(calendar['halt_coverage_complete']).lower()}",
        "- Safe for historical training: false",
        "- Live trading capable: false",
        "",
        (
            "Current observations are not backdated. Historical mappings, actions, "
            "delistings, security subtypes, and halt history remain explicitly "
            "unresolved."
        ),
        "",
        "| Symbol | Current mapping | Venue | Stable ID |",
        "| --- | --- | --- | --- |",
    ]
    for entry in entries:
        identifier = entry["instrument_id"] or "UNRESOLVED"
        lines.append(
            f"| {entry['symbol']} | {entry['current_mapping_status']} | "
            f"{entry['listing_venue']} | `{identifier}` |"
        )
    return "\n".join(lines) + "\n"


def build_reference_artifacts(
    source_report: Path = DEFAULT_BACKFILL_REPORT,
    output_directory: Path = DEFAULT_REFERENCE_DIRECTORY,
) -> dict[str, object]:
    """Build owner-only immutable snapshot and complete-universe reports."""
    snapshot = build_snapshot_from_backfill_report(source_report)
    snapshot_value = snapshot_document(snapshot)
    snapshot_document_sha = cast("str", snapshot_value["document_sha256"])
    report = resolution_report_document(snapshot, snapshot_document_sha)
    snapshot_path = output_directory / "reference-snapshot-v1.json"
    report_path = output_directory / "instrument-resolution-report-v1.json"
    human_path = output_directory / "instrument-resolution-report.md"
    snapshot_file_sha = _write_json(snapshot_path, snapshot_value)
    report_file_sha = _write_json(report_path, report)
    _atomic_write(human_path, _human_report(report).encode())
    return {
        "instrument_resolution_report": str(report_path),
        "instrument_resolution_report_file_sha256": report_file_sha,
        "reference_snapshot": str(snapshot_path),
        "reference_snapshot_document_sha256": snapshot_document_sha,
        "reference_snapshot_file_sha256": snapshot_file_sha,
        "requested_symbols": cast("Mapping[str, object]", report["counts"])[
            "requested"
        ],
        "status": report["status"],
    }


def verify_reference_artifact(path: Path, *, expected_schema: str) -> str:
    """Verify a bounded canonical reference artifact and return its file hash."""
    try:
        payload = path.read_bytes()
    except OSError as error:
        raise ReferenceDataError(
            ReferenceErrorCode.INVALID_INPUT, "reference artifact cannot be read"
        ) from error
    if not payload or len(payload) > MAX_INPUT_BYTES:
        raise ReferenceDataError(
            ReferenceErrorCode.INVALID_INPUT, "reference artifact size is invalid"
        )
    try:
        value = json.loads(payload)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ReferenceDataError(
            ReferenceErrorCode.INVALID_INPUT, "reference artifact is malformed"
        ) from error
    _verify_document(value, expected_schema=expected_schema)
    if payload != _canonical_bytes(value) + b"\n":
        raise ReferenceDataError(
            ReferenceErrorCode.INTEGRITY_FAILURE, "artifact is not canonical JSON"
        )
    return hashlib.sha256(payload).hexdigest()


def _latency_summary(
    samples_ns: Sequence[int], operations_per_sample: int
) -> dict[str, object]:
    if not samples_ns or operations_per_sample <= 0:
        raise ReferenceDataError(
            ReferenceErrorCode.INVALID_INPUT, "benchmark sample set is invalid"
        )
    ordered = sorted(samples_ns)

    def percentile(numerator: int, denominator: int) -> int:
        index = min(len(ordered) - 1, (len(ordered) * numerator) // denominator)
        return ordered[index]

    elapsed = sum(ordered)
    operations = len(ordered) * operations_per_sample
    return {
        "max_batch_ns": ordered[-1],
        "operations": operations,
        "operations_per_second": (operations * NANOSECONDS_PER_SECOND) / elapsed,
        "p50_batch_ns": percentile(50, 100),
        "p95_batch_ns": percentile(95, 100),
        "p99_batch_ns": percentile(99, 100),
        "raw_batch_samples_ns": list(samples_ns),
    }


def benchmark_reference_store(
    snapshot: ReferenceSnapshot, *, iterations: int = 100
) -> dict[str, object]:
    """Measure bounded reference lookups over a fixed immutable snapshot."""
    if not 0 < iterations <= MAX_BENCHMARK_ITERATIONS:
        raise ReferenceDataError(
            ReferenceErrorCode.INVALID_INPUT, "benchmark iterations are out of range"
        )
    store = PointInTimeReferenceStore(snapshot)
    resolver = store.as_universe_resolver(
        effective_at_ns=snapshot.as_of_ns, known_at_ns=snapshot.as_of_ns
    )
    session_dates = tuple(item.session_date for item in snapshot.calendar_days)
    symbols = tuple(item.symbol for item in snapshot.mappings)
    instrument_ids = tuple(
        item.instrument_id
        for item in snapshot.mappings
        if item.instrument_id is not None
    )

    def measure(operation: Callable[[], int]) -> list[int]:
        samples: list[int] = []
        checksum = 0
        for _ in range(iterations):
            started = time.perf_counter_ns()
            checksum ^= operation()
            samples.append(time.perf_counter_ns() - started)
        del checksum
        return samples

    resolution_samples = measure(
        lambda: sum(resolver.resolve(symbol).code.value for symbol in symbols)
    )
    mapping_samples = measure(
        lambda: sum(
            store.symbol_mapping_at(
                symbol,
                effective_at_ns=snapshot.as_of_ns,
                known_at_ns=snapshot.as_of_ns,
            )
            is not None
            for symbol in symbols
        )
    )
    calendar_samples = measure(
        lambda: sum(
            store.calendar_day_at(item, known_at_ns=snapshot.as_of_ns).version
            for item in session_dates
        )
    )
    adjustment_samples = measure(
        lambda: sum(
            store.adjust_price_for_splits(
                1_000_000_000,
                instrument_id,
                after_ns=1,
                through_ns=snapshot.as_of_ns,
                known_at_ns=snapshot.as_of_ns,
            )
            for instrument_id in instrument_ids
        )
    )
    return _document(
        {
            "benchmark_kind": "POINT_IN_TIME_REFERENCE",
            "calendar_lookup": _latency_summary(calendar_samples, len(session_dates)),
            "instrument_adjustment": _latency_summary(
                adjustment_samples, max(1, len(instrument_ids))
            ),
            "iterations": iterations,
            "live_trading_capable": False,
            "mapping_lookup": _latency_summary(mapping_samples, len(symbols)),
            "platform": platform.platform(),
            "process_max_rss_kib": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
            "schema_version": "1.0.0",
            "universe_resolution": _latency_summary(resolution_samples, len(symbols)),
            "universe_source_sha256": snapshot.universe_source_sha256,
        }
    )


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    build = subparsers.add_parser("build", help="build Prompt 52 artifacts offline")
    build.add_argument("--source-report", type=Path, default=DEFAULT_BACKFILL_REPORT)
    build.add_argument(
        "--output-directory", type=Path, default=DEFAULT_REFERENCE_DIRECTORY
    )
    verify = subparsers.add_parser("verify", help="verify one reference artifact")
    verify.add_argument("path", type=Path)
    verify.add_argument(
        "--schema-version",
        choices=(REFERENCE_SCHEMA_VERSION, REFERENCE_REPORT_SCHEMA_VERSION),
        default=REFERENCE_SCHEMA_VERSION,
    )
    benchmark = subparsers.add_parser(
        "benchmark", help="benchmark the real offline reference snapshot"
    )
    benchmark.add_argument(
        "--source-report", type=Path, default=DEFAULT_BACKFILL_REPORT
    )
    benchmark.add_argument("--iterations", type=int, default=100)
    benchmark.add_argument(
        "--output",
        type=Path,
        default=DEFAULT_REFERENCE_DIRECTORY / "reference-benchmark-v1.json",
    )
    return parser


def main(arguments: Sequence[str] | None = None) -> int:
    """Run the offline reference-data command."""
    values = _parser().parse_args(arguments)
    if values.command == "build":
        result = build_reference_artifacts(
            values.source_report, values.output_directory
        )
    elif values.command == "verify":
        result = {
            "file_sha256": verify_reference_artifact(
                values.path, expected_schema=values.schema_version
            ),
            "status": "VERIFIED",
        }
    else:
        snapshot = build_snapshot_from_backfill_report(values.source_report)
        result = benchmark_reference_store(snapshot, iterations=values.iterations)
        _write_json(values.output, result)
    print(json.dumps(result, sort_keys=True, separators=(",", ":")))  # noqa: T201
    return 0


def cli_main() -> int:
    """Console-script entry point."""
    return main()
