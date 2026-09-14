"""Leakage-safe, bounded feature and multi-horizon label dataset construction."""

from __future__ import annotations

import hashlib
import json
import math
import os
from collections import defaultdict
from dataclasses import dataclass
from enum import StrEnum
from itertools import groupby, pairwise
from pathlib import Path
from typing import TYPE_CHECKING, Final, Protocol, cast

import pyarrow as pa  # type: ignore[import-untyped]
import pyarrow.dataset as ds  # type: ignore[import-untyped]
import pyarrow.parquet as pq  # type: ignore[import-untyped]

from aegis_mx_research.canonical_minute import (
    CANONICAL_MINUTE_SCHEMA_VERSION,
    CanonicalMinuteRecord,
    MinuteDataQuality,
)
from aegis_mx_research.data_repository import (
    DataRepository,
    QuotaEvidence,
    StorageRequest,
)
from aegis_mx_research.forecast_contracts import (
    REQUIRED_HORIZON_LABELS,
    ExchangeCalendar,
    HorizonContractError,
    InstrumentResolutionCode,
    UniverseEntry,
    UniverseSnapshot,
    required_horizon_specs,
    resolve_horizon,
)

if TYPE_CHECKING:
    from collections.abc import Callable, Iterable, Iterator, Mapping, Sequence

FEATURE_DATASET_SCHEMA_VERSION: Final = "1.0.0"
FEATURE_RECORD_SCHEMA_VERSION: Final = "1.0.0"
SESSION_SUMMARY_SCHEMA_VERSION: Final = "1.0.0"
DATASET_SEED: Final = 20_260_831
PURGE_SESSIONS: Final = 42
PPM_SCALE: Final = 1_000_000
MAX_INT64: Final = (1 << 63) - 1
MINUTE_NS: Final = 60_000_000_000
MAX_EVENTS: Final = 1_000_000
MAX_SECTOR_REVISIONS: Final = 100_000
MAX_RECORDS_PER_INSTRUMENT: Final = 1_000_000
DEFAULT_OUTPUT_BYTES_PER_SAMPLE: Final = 1_024
DEFAULT_TEMPORARY_BYTES: Final = 64_000_000
SHA256_LENGTH: Final = 64
MAX_PARQUET_BATCH_ROWS: Final = 1_000_000
MAX_DATASET_INSTRUMENTS: Final = 10_000
MINIMUM_USABLE_SPLIT_SESSIONS: Final = 3
MAX_AGGREGATE_POINTS: Final = 5_000_000
RELATIVE_VOLUME_WINDOW: Final = 20
REALIZED_VOLATILITY_WINDOW: Final = 30
ROLLING_SESSION_WINDOW: Final = 5
FEATURE_NAMES: Final = (
    "return_1m_ppm",
    "return_5m_ppm",
    "return_15m_ppm",
    "return_30m_ppm",
    "return_60m_ppm",
    "volume_shares",
    "relative_volume_20m_ppm",
    "realized_volatility_30m_ppm",
    "high_low_range_ppm",
    "intraday_gap_ppm",
    "minute_of_session",
    "session_position_ppm",
    "market_return_1m_ppm",
    "market_relative_return_1m_ppm",
    "sector_return_1m_ppm",
    "sector_relative_return_1m_ppm",
    "rolling_session_return_5d_ppm",
    "rolling_session_volume_5d_shares",
    "news_event_flag",
    "macro_event_flag",
    "data_quality_valid_flag",
    "data_quality_reason_count",
)
RETURN_WINDOWS: Final = (1, 5, 15, 30, 60)


class DatasetBuildCode(StrEnum):
    """Stable fail-closed build and leakage reason codes."""

    CORRUPT_CANONICAL_INPUT = "CORRUPT_CANONICAL_INPUT"
    CORPORATE_ACTION_CHANGED = "CORPORATE_ACTION_CHANGED"
    DUPLICATE_MINUTE = "DUPLICATE_MINUTE"
    FUTURE_EVENT_REVISION = "FUTURE_EVENT_REVISION"
    FUTURE_REFERENCE = "FUTURE_REFERENCE"
    INSUFFICIENT_CALENDAR = "INSUFFICIENT_CALENDAR"
    INSUFFICIENT_SPLIT_SESSIONS = "INSUFFICIENT_SPLIT_SESSIONS"
    INTEGER_OVERFLOW = "INTEGER_OVERFLOW"
    INVALID_CONFIGURATION = "INVALID_CONFIGURATION"
    LABEL_FEATURE_OVERLAP = "LABEL_FEATURE_OVERLAP"
    MISSING_CANONICAL_DATA = "MISSING_CANONICAL_DATA"
    MISSING_TARGET_BAR = "MISSING_TARGET_BAR"
    NON_CHRONOLOGICAL_INPUT = "NON_CHRONOLOGICAL_INPUT"
    NON_CHRONOLOGICAL_SPLIT = "NON_CHRONOLOGICAL_SPLIT"
    NORMALIZATION_LEAKAGE = "NORMALIZATION_LEAKAGE"
    PURGE_EMBARGO_VIOLATION = "PURGE_EMBARGO_VIOLATION"
    STORAGE_LIMIT = "STORAGE_LIMIT"
    TICK_GRID_CHANGED = "TICK_GRID_CHANGED"
    UNRESOLVED_INSTRUMENT = "UNRESOLVED_INSTRUMENT"


class DatasetBuildError(ValueError):
    """Dataset rejection retaining a bounded machine-readable reason."""

    def __init__(self, code: DatasetBuildCode, detail: str) -> None:
        """Create a deterministic fail-closed build error."""
        self.code = code
        super().__init__(f"{code.value}: {detail}"[:512])


class FeatureValidity(StrEnum):
    """Feature-row state before model-specific completeness checks."""

    VALID = "VALID"
    DEGRADED = "DEGRADED"
    WARMUP = "WARMUP"


class LabelValidity(StrEnum):
    """Whether one target was constructed without unsafe inference."""

    VALID = "VALID"
    MISSING = "MISSING"
    INVALID = "INVALID"


class FeatureEventKind(StrEnum):
    """Advisory event families permitted as offline feature inputs."""

    NEWS = "NEWS"
    MACRO = "MACRO"


class _HexIdentifier(Protocol):
    def hex(self) -> str:
        """Return a stable hexadecimal identifier."""


class _Hasher(Protocol):
    def update(self, value: bytes) -> None:
        """Add bytes to the digest state."""

    def hexdigest(self) -> str:
        """Return the lowercase hexadecimal digest."""


def _canonical_bytes(value: object) -> bytes:
    return json.dumps(
        value, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode("ascii")


def _digest(value: object) -> str:
    return hashlib.sha256(_canonical_bytes(value)).hexdigest()


def _require_text(value: str, field: str, *, maximum: int = 256) -> None:
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as error:
        raise DatasetBuildError(
            DatasetBuildCode.INVALID_CONFIGURATION, f"{field} is not ASCII"
        ) from error
    if not encoded or len(encoded) > maximum or b"\x00" in encoded:
        raise DatasetBuildError(
            DatasetBuildCode.INVALID_CONFIGURATION, f"{field} is invalid"
        )


def _require_sha256(value: str, field: str) -> None:
    if (
        len(value) != SHA256_LENGTH
        or value != value.lower()
        or any(character not in "0123456789abcdef" for character in value)
        or value == "0" * 64
    ):
        raise DatasetBuildError(
            DatasetBuildCode.INVALID_CONFIGURATION, f"{field} is not SHA-256"
        )


def _checked_int64(value: int, field: str, *, nonnegative: bool = False) -> int:
    lower = 0 if nonnegative else -MAX_INT64
    if type(value) is not int or not lower <= value <= MAX_INT64:
        raise DatasetBuildError(
            DatasetBuildCode.INTEGER_OVERFLOW, f"{field} exceeds int64"
        )
    return value


def _divide_round_half_away(numerator: int, denominator: int) -> int:
    if denominator <= 0:
        raise DatasetBuildError(
            DatasetBuildCode.CORRUPT_CANONICAL_INPUT,
            "ratio denominator must be positive",
        )
    magnitude, remainder = divmod(abs(numerator), denominator)
    if remainder * 2 >= denominator:
        magnitude += 1
    return magnitude if numerator >= 0 else -magnitude


def _return_ppm(current_ticks: int, prior_ticks: int) -> int:
    if current_ticks <= 0 or prior_ticks <= 0:
        raise DatasetBuildError(
            DatasetBuildCode.CORRUPT_CANONICAL_INPUT,
            "return prices must be positive ticks",
        )
    return _checked_int64(
        _divide_round_half_away((current_ticks - prior_ticks) * PPM_SCALE, prior_ticks),
        "return_ppm",
    )


def _mean_int(values: Sequence[int]) -> int:
    if not values:
        raise DatasetBuildError(
            DatasetBuildCode.INVALID_CONFIGURATION, "mean requires observations"
        )
    return _divide_round_half_away(sum(values), len(values))


@dataclass(frozen=True, slots=True)
class SectorRevision:
    """Point-in-time sector membership used only when known at feature cutoff."""

    revision_id: str
    instrument_id: str
    sector_id: str
    effective_from_ns: int
    effective_to_ns: int | None
    available_at_ns: int
    version: int
    source_sha256: str

    def __post_init__(self) -> None:
        """Validate the bitemporal sector revision."""
        for field, value in (
            ("revision_id", self.revision_id),
            ("instrument_id", self.instrument_id),
            ("sector_id", self.sector_id),
        ):
            _require_text(value, field)
        for timestamp_field, timestamp_value in (
            ("effective_from_ns", self.effective_from_ns),
            ("available_at_ns", self.available_at_ns),
        ):
            if not 0 < timestamp_value <= MAX_INT64:
                raise DatasetBuildError(
                    DatasetBuildCode.INVALID_CONFIGURATION,
                    f"{timestamp_field} is invalid",
                )
        if self.effective_to_ns is not None and not (
            self.effective_from_ns < self.effective_to_ns <= MAX_INT64
        ):
            raise DatasetBuildError(
                DatasetBuildCode.INVALID_CONFIGURATION,
                "sector validity interval is invalid",
            )
        if self.version <= 0:
            raise DatasetBuildError(
                DatasetBuildCode.INVALID_CONFIGURATION,
                "sector version must be positive",
            )
        _require_sha256(self.source_sha256, "sector source_sha256")

    def applies(self, event_time_ns: int, known_at_ns: int) -> bool:
        """Return true only when effective and already available."""
        return (
            self.available_at_ns <= known_at_ns
            and self.effective_from_ns <= event_time_ns
            and (self.effective_to_ns is None or event_time_ns < self.effective_to_ns)
        )


class SectorIndex:
    """Bounded immutable point-in-time sector resolver."""

    def __init__(self, revisions: Sequence[SectorRevision]) -> None:
        """Validate contiguous knowledge-time revision lineages."""
        if len(revisions) > MAX_SECTOR_REVISIONS:
            raise DatasetBuildError(
                DatasetBuildCode.INVALID_CONFIGURATION,
                "sector revision bound is exceeded",
            )
        grouped: dict[str, list[SectorRevision]] = defaultdict(list)
        for revision in revisions:
            grouped[revision.instrument_id].append(revision)
        for instrument_id, values in grouped.items():
            ordered = sorted(values, key=lambda item: item.version)
            if tuple(item.version for item in ordered) != tuple(
                range(1, len(ordered) + 1)
            ) or any(
                current.available_at_ns <= previous.available_at_ns
                for previous, current in pairwise(ordered)
            ):
                raise DatasetBuildError(
                    DatasetBuildCode.FUTURE_REFERENCE,
                    f"sector lineage for {instrument_id} is not contiguous",
                )
            grouped[instrument_id] = ordered
        self._by_instrument = dict(grouped)
        self.sha256 = _digest(
            [
                {
                    "available_at_ns": item.available_at_ns,
                    "effective_from_ns": item.effective_from_ns,
                    "effective_to_ns": item.effective_to_ns,
                    "instrument_id": item.instrument_id,
                    "revision_id": item.revision_id,
                    "sector_id": item.sector_id,
                    "source_sha256": item.source_sha256,
                    "version": item.version,
                }
                for item in sorted(
                    revisions, key=lambda value: (value.instrument_id, value.version)
                )
            ]
        )

    def at(
        self, instrument_id: str, *, event_time_ns: int, known_at_ns: int
    ) -> SectorRevision | None:
        """Resolve the latest eligible revision without future knowledge."""
        eligible = [
            item
            for item in self._by_instrument.get(instrument_id, ())
            if item.applies(event_time_ns, known_at_ns)
        ]
        return None if not eligible else max(eligible, key=lambda item: item.version)


@dataclass(frozen=True, slots=True)
class TemporalFeatureEvent:
    """Bounded advisory event flag with explicit availability and validity."""

    record_id: str
    revision_id: str
    kind: FeatureEventKind
    instrument_id: str | None
    event_time_ns: int
    available_at_ns: int
    valid_until_ns: int
    source_sha256: str

    def __post_init__(self) -> None:
        """Reject ambiguous or reversed event time semantics."""
        _require_text(self.record_id, "record_id")
        _require_text(self.revision_id, "revision_id")
        if self.instrument_id is not None:
            _require_text(self.instrument_id, "instrument_id")
        if not isinstance(self.kind, FeatureEventKind):
            raise DatasetBuildError(
                DatasetBuildCode.INVALID_CONFIGURATION, "event kind is invalid"
            )
        if not (
            0 < self.event_time_ns < self.valid_until_ns <= MAX_INT64
            and 0 < self.available_at_ns <= MAX_INT64
        ):
            raise DatasetBuildError(
                DatasetBuildCode.INVALID_CONFIGURATION,
                "event time or validity is invalid",
            )
        _require_sha256(self.source_sha256, "event source_sha256")


class EventIndex:
    """Immutable advisory-event index that exposes no future revisions."""

    def __init__(self, events: Sequence[TemporalFeatureEvent]) -> None:
        """Index bounded events and reject repeated record identities."""
        if len(events) > MAX_EVENTS or len({item.record_id for item in events}) != len(
            events
        ):
            raise DatasetBuildError(
                DatasetBuildCode.INVALID_CONFIGURATION,
                "event count or identity is invalid",
            )
        by_instrument: dict[str, list[TemporalFeatureEvent]] = defaultdict(list)
        macro: list[TemporalFeatureEvent] = []
        for event in events:
            if event.kind is FeatureEventKind.MACRO:
                macro.append(event)
            elif event.instrument_id is not None:
                by_instrument[event.instrument_id].append(event)
        self._news = {
            key: tuple(
                sorted(values, key=lambda item: (item.event_time_ns, item.record_id))
            )
            for key, values in by_instrument.items()
        }
        self._macro = tuple(
            sorted(macro, key=lambda item: (item.event_time_ns, item.record_id))
        )
        self.sha256 = _digest(
            [
                {
                    "available_at_ns": item.available_at_ns,
                    "event_time_ns": item.event_time_ns,
                    "instrument_id": item.instrument_id,
                    "kind": item.kind.value,
                    "record_id": item.record_id,
                    "revision_id": item.revision_id,
                    "source_sha256": item.source_sha256,
                    "valid_until_ns": item.valid_until_ns,
                }
                for item in sorted(events, key=lambda value: value.record_id)
            ]
        )

    def active(
        self, instrument_id: str, as_of_ns: int
    ) -> tuple[bool, bool, tuple[str, ...], int]:
        """Return news/macro flags and exact point-in-time provenance."""
        news = tuple(
            item
            for item in self._news.get(instrument_id, ())
            if item.available_at_ns <= as_of_ns
            and item.event_time_ns <= as_of_ns < item.valid_until_ns
        )
        macro = tuple(
            item
            for item in self._macro
            if item.available_at_ns <= as_of_ns
            and item.event_time_ns <= as_of_ns < item.valid_until_ns
        )
        selected = news + macro
        return (
            bool(news),
            bool(macro),
            tuple(sorted(item.record_id for item in selected)),
            max((item.available_at_ns for item in selected), default=0),
        )


class CanonicalDatasetSource(Protocol):
    """Repeatable bounded canonical source required by the three-pass builder."""

    @property
    def instrument_ids(self) -> tuple[str, ...]:
        """Return deterministic stable instruments represented by the source."""

    @property
    def record_count(self) -> int:
        """Return the verified total canonical record count."""

    @property
    def source_partition_ids(self) -> tuple[str, ...]:
        """Return immutable canonical partition identities."""

    def iter_instrument(self, instrument_id: str) -> Iterator[CanonicalMinuteRecord]:
        """Yield one instrument in increasing minute-end order."""


@dataclass(frozen=True, slots=True)
class CanonicalPartitionInput:
    """Verified Prompt 56 Parquet object admitted to the dataset builder."""

    path: Path
    object_sha256: str
    manifest_id: str

    def __post_init__(self) -> None:
        """Reject paths and identities that cannot be authenticated."""
        _require_sha256(self.object_sha256, "partition object_sha256")
        _require_text(self.manifest_id, "partition manifest_id")
        if not self.path.is_absolute():
            raise DatasetBuildError(
                DatasetBuildCode.INVALID_CONFIGURATION,
                "canonical partition path must be absolute",
            )


def _record_from_arrow(row: Mapping[str, object]) -> CanonicalMinuteRecord:
    payload = dict(row)
    payload.pop("price_unit", None)
    payload.pop("quantity_unit", None)
    trading_date = payload.get("trading_date")
    if hasattr(trading_date, "isoformat"):
        payload["trading_date"] = trading_date.isoformat()
    return CanonicalMinuteRecord.from_bytes(_canonical_bytes(payload))


class ParquetCanonicalSource:
    """Repeatable, hash-verified, bounded-batch reader for Prompt 56 output."""

    def __init__(
        self,
        partitions: Sequence[CanonicalPartitionInput],
        *,
        batch_rows: int = 65_536,
        maximum_records: int = 100_000_000,
    ) -> None:
        """Authenticate Parquet metadata without loading complete partitions."""
        if not partitions or not 0 < batch_rows <= MAX_PARQUET_BATCH_ROWS:
            raise DatasetBuildError(
                DatasetBuildCode.INVALID_CONFIGURATION,
                "partition list or batch size is invalid",
            )
        if len({item.manifest_id for item in partitions}) != len(partitions):
            raise DatasetBuildError(
                DatasetBuildCode.INVALID_CONFIGURATION,
                "canonical partition manifest identity repeats",
            )
        instruments: set[str] = set()
        partitions_by_instrument: dict[str, list[CanonicalPartitionInput]] = (
            defaultdict(list)
        )
        total = 0
        ordered = tuple(sorted(partitions, key=lambda item: str(item.path)))
        for item in ordered:
            try:
                status = item.path.lstat()
                if not item.path.is_file() or item.path.is_symlink():
                    raise DatasetBuildError(
                        DatasetBuildCode.CORRUPT_CANONICAL_INPUT,
                        "canonical partition is not a regular file",
                    )
                with item.path.open("rb") as source_file:
                    digest = hashlib.file_digest(source_file, "sha256").hexdigest()
                if digest != item.object_sha256:
                    raise DatasetBuildError(
                        DatasetBuildCode.CORRUPT_CANONICAL_INPUT,
                        "canonical partition SHA-256 mismatch",
                    )
                parquet = pq.ParquetFile(item.path)
                metadata = parquet.metadata
                schema_metadata = parquet.schema_arrow.metadata or {}
                if schema_metadata.get(
                    b"aegis.schema"
                ) != b"canonical-minute-record" or schema_metadata.get(
                    b"aegis.schema_version"
                ) != CANONICAL_MINUTE_SCHEMA_VERSION.encode("ascii"):
                    raise DatasetBuildError(
                        DatasetBuildCode.CORRUPT_CANONICAL_INPUT,
                        "canonical partition schema identity is invalid",
                    )
                compression = {
                    metadata.row_group(group).column(column).compression
                    for group in range(metadata.num_row_groups)
                    for column in range(metadata.num_columns)
                }
                if compression != {"ZSTD"}:
                    raise DatasetBuildError(
                        DatasetBuildCode.CORRUPT_CANONICAL_INPUT,
                        "canonical partition is not uniformly Zstandard compressed",
                    )
                total += metadata.num_rows
                if total > maximum_records or status.st_size <= 0:
                    raise DatasetBuildError(
                        DatasetBuildCode.STORAGE_LIMIT,
                        "canonical source exceeds its configured bound",
                    )
                partition_instruments: set[str] = set()
                for batch in parquet.iter_batches(
                    batch_size=batch_rows, columns=("instrument_id",)
                ):
                    partition_instruments.update(
                        cast("list[str]", batch.column(0).to_pylist())
                    )
                    instruments.update(partition_instruments)
                    if len(instruments) > MAX_DATASET_INSTRUMENTS:
                        raise DatasetBuildError(
                            DatasetBuildCode.STORAGE_LIMIT,
                            "canonical instrument bound is exceeded",
                        )
                for instrument_id in sorted(partition_instruments):
                    partitions_by_instrument[instrument_id].append(item)
            except DatasetBuildError:
                raise
            except (OSError, pa.ArrowException) as error:
                raise DatasetBuildError(
                    DatasetBuildCode.CORRUPT_CANONICAL_INPUT,
                    "canonical partition cannot be inspected",
                ) from error
        self._partitions = ordered
        self._batch_rows = batch_rows
        self._record_count = total
        self._instrument_ids = tuple(sorted(instruments))
        self._partitions_by_instrument = {
            instrument_id: tuple(values)
            for instrument_id, values in partitions_by_instrument.items()
        }

    @property
    def instrument_ids(self) -> tuple[str, ...]:
        """Return bounded stable IDs discovered from verified Arrow batches."""
        return self._instrument_ids

    @property
    def record_count(self) -> int:
        """Return verified Parquet row count."""
        return self._record_count

    @property
    def source_partition_ids(self) -> tuple[str, ...]:
        """Return accepted Prompt 56 manifest identities."""
        return tuple(item.manifest_id for item in self._partitions)

    def iter_instrument(self, instrument_id: str) -> Iterator[CanonicalMinuteRecord]:
        """Scan ordered date fragments with a vectorized instrument filter."""
        items = self._partitions_by_instrument.get(instrument_id, ())
        if not items:
            return
        dataset = ds.dataset([str(item.path) for item in items], format="parquet")
        scanner = dataset.scanner(
            filter=ds.field("instrument_id") == instrument_id,
            batch_size=self._batch_rows,
            batch_readahead=1,
            fragment_readahead=1,
            use_threads=False,
        )
        previous = 0
        for batch in scanner.to_batches():
            for row in batch.to_pylist():
                record = _record_from_arrow(cast("Mapping[str, object]", row))
                endpoint = record.minute_end_exchange_time_ns
                if endpoint <= previous:
                    code = (
                        DatasetBuildCode.DUPLICATE_MINUTE
                        if endpoint == previous
                        else DatasetBuildCode.NON_CHRONOLOGICAL_INPUT
                    )
                    raise DatasetBuildError(code, "canonical source minute repeats")
                previous = endpoint
                yield record


class InMemoryCanonicalSource:
    """Bounded deterministic source for tests and small offline fixtures."""

    def __init__(
        self,
        records: Iterable[CanonicalMinuteRecord],
        *,
        maximum_records: int = MAX_RECORDS_PER_INSTRUMENT,
        source_partition_ids: Sequence[str] = ("partition-synthetic",),
    ) -> None:
        """Copy a bounded fixture and enforce unique chronological minutes."""
        if maximum_records <= 0:
            raise DatasetBuildError(
                DatasetBuildCode.INVALID_CONFIGURATION,
                "source record bound must be positive",
            )
        grouped: dict[str, list[CanonicalMinuteRecord]] = defaultdict(list)
        total = 0
        for record in records:
            total += 1
            if total > maximum_records:
                raise DatasetBuildError(
                    DatasetBuildCode.STORAGE_LIMIT, "source record bound is exceeded"
                )
            grouped[record.instrument_id].append(record)
        normalized: dict[str, tuple[CanonicalMinuteRecord, ...]] = {}
        for instrument_id, values in grouped.items():
            ordered = tuple(
                sorted(values, key=lambda item: item.minute_end_exchange_time_ns)
            )
            endpoints = tuple(item.minute_end_exchange_time_ns for item in ordered)
            if len(endpoints) != len(set(endpoints)):
                raise DatasetBuildError(
                    DatasetBuildCode.DUPLICATE_MINUTE,
                    f"duplicate canonical minute for {instrument_id}",
                )
            normalized[instrument_id] = ordered
        if not source_partition_ids or len(set(source_partition_ids)) != len(
            source_partition_ids
        ):
            raise DatasetBuildError(
                DatasetBuildCode.INVALID_CONFIGURATION,
                "source partition identities are invalid",
            )
        for identifier in source_partition_ids:
            _require_text(identifier, "source_partition_id")
        self._records = normalized
        self._record_count = total
        self._source_partition_ids = tuple(sorted(source_partition_ids))

    @property
    def instrument_ids(self) -> tuple[str, ...]:
        """Return stable IDs in canonical order."""
        return tuple(sorted(self._records))

    @property
    def record_count(self) -> int:
        """Return fixture record count."""
        return self._record_count

    @property
    def source_partition_ids(self) -> tuple[str, ...]:
        """Return immutable source partition IDs."""
        return self._source_partition_ids

    def iter_instrument(self, instrument_id: str) -> Iterator[CanonicalMinuteRecord]:
        """Yield one copied instrument sequence."""
        yield from self._records.get(instrument_id, ())


@dataclass(frozen=True, slots=True)
class SplitPlan:
    """Chronological session assignment with two exact purge intervals."""

    session_splits: Mapping[str, str]
    train_sessions: tuple[str, ...]
    validation_sessions: tuple[str, ...]
    test_sessions: tuple[str, ...]
    first_purge_sessions: tuple[str, ...]
    second_purge_sessions: tuple[str, ...]


def build_split_plan(observed_session_ids: Sequence[str]) -> SplitPlan:
    """Allocate chronological splits with a 42-session purge at each boundary."""
    sessions = tuple(observed_session_ids)
    if len(sessions) != len(set(sessions)):
        raise DatasetBuildError(
            DatasetBuildCode.NON_CHRONOLOGICAL_SPLIT,
            "observed sessions repeat identity",
        )
    usable = len(sessions) - 2 * PURGE_SESSIONS
    if usable < MINIMUM_USABLE_SPLIT_SESSIONS:
        raise DatasetBuildError(
            DatasetBuildCode.INSUFFICIENT_SPLIT_SESSIONS,
            "at least 87 observed sessions are required",
        )
    train_count = min(max(1, usable * 70 // 100), usable - 2)
    validation_count = min(max(1, usable * 15 // 100), usable - train_count - 1)
    test_count = usable - train_count - validation_count
    first_purge_start = train_count
    validation_start = first_purge_start + PURGE_SESSIONS
    second_purge_start = validation_start + validation_count
    test_start = second_purge_start + PURGE_SESSIONS
    train = sessions[:train_count]
    first_purge = sessions[first_purge_start:validation_start]
    validation = sessions[validation_start:second_purge_start]
    second_purge = sessions[second_purge_start:test_start]
    test = sessions[test_start : test_start + test_count]
    assignments = {
        **dict.fromkeys(train, "TRAIN"),
        **dict.fromkeys(validation, "VALIDATION"),
        **dict.fromkeys(test, "TEST"),
    }
    return SplitPlan(assignments, train, validation, test, first_purge, second_purge)


@dataclass(frozen=True, slots=True)
class FeatureLabel:
    """One explicit, point-in-time-safe future target."""

    horizon: str
    target_exchange_time_ns: int | None
    return_ppm: int | None
    direction: int | None
    future_price_ticks: int | None
    corporate_action_version: str
    validity: LabelValidity
    missing_reason: str | None

    def __post_init__(self) -> None:
        """Require all values for valid labels and none for missing labels."""
        if self.horizon not in REQUIRED_HORIZON_LABELS:
            raise DatasetBuildError(
                DatasetBuildCode.INVALID_CONFIGURATION, "unknown label horizon"
            )
        values = (self.return_ppm, self.direction, self.future_price_ticks)
        if self.validity is LabelValidity.VALID:
            if (
                self.target_exchange_time_ns is None
                or any(value is None for value in values)
                or self.direction not in {-1, 0, 1}
                or self.missing_reason is not None
            ):
                raise DatasetBuildError(
                    DatasetBuildCode.CORRUPT_CANONICAL_INPUT,
                    "valid label is incomplete",
                )
        elif any(value is not None for value in values) or self.missing_reason is None:
            raise DatasetBuildError(
                DatasetBuildCode.CORRUPT_CANONICAL_INPUT,
                "non-valid label carries a target value or lacks a reason",
            )
        _require_sha256(self.corporate_action_version, "corporate_action_version")


@dataclass(frozen=True, slots=True)
class FeatureSample:
    """Immutable feature cutoff plus all requested labels and provenance."""

    sample_id: str
    split: str
    instrument_id: str
    source_ticker: str
    session_id: str
    trading_date: str
    as_of_exchange_time_ns: int
    knowledge_cutoff_time_ns: int
    feature_start_exchange_time_ns: int
    feature_end_exchange_time_ns: int
    raw_features: tuple[int | None, ...]
    normalized_features_ppm: tuple[int | None, ...]
    feature_validity: FeatureValidity
    feature_reason_codes: tuple[str, ...]
    advisory_event_ids: tuple[str, ...]
    max_event_availability_time_ns: int
    source_record_sha256: str
    labels: tuple[FeatureLabel, ...]
    schema_version: str = FEATURE_RECORD_SCHEMA_VERSION

    def __post_init__(self) -> None:
        """Validate sizes, timestamp domains, ordering, and immutable identity."""
        if self.split not in {"TRAIN", "VALIDATION", "TEST"}:
            raise DatasetBuildError(
                DatasetBuildCode.NON_CHRONOLOGICAL_SPLIT, "sample split is invalid"
            )
        if len(self.raw_features) != len(FEATURE_NAMES) or len(
            self.normalized_features_ppm
        ) != len(FEATURE_NAMES):
            raise DatasetBuildError(
                DatasetBuildCode.CORRUPT_CANONICAL_INPUT,
                "feature vector does not match its schema",
            )
        if tuple(label.horizon for label in self.labels) != REQUIRED_HORIZON_LABELS:
            raise DatasetBuildError(
                DatasetBuildCode.CORRUPT_CANONICAL_INPUT,
                "label vector is not in canonical horizon order",
            )
        if any(
            label.validity is LabelValidity.VALID
            and cast("int", label.target_exchange_time_ns)
            <= self.feature_end_exchange_time_ns
            for label in self.labels
        ):
            raise DatasetBuildError(
                DatasetBuildCode.LABEL_FEATURE_OVERLAP,
                "a label target overlaps its feature interval",
            )
        if not (
            0
            < self.feature_start_exchange_time_ns
            <= self.feature_end_exchange_time_ns
            == self.as_of_exchange_time_ns
            <= MAX_INT64
        ):
            raise DatasetBuildError(
                DatasetBuildCode.LABEL_FEATURE_OVERLAP,
                "feature interval is inconsistent with its cutoff",
            )
        if self.max_event_availability_time_ns > self.knowledge_cutoff_time_ns:
            raise DatasetBuildError(
                DatasetBuildCode.FUTURE_EVENT_REVISION,
                "feature row uses a future event revision",
            )
        _require_sha256(self.sample_id, "sample_id")
        _require_sha256(self.source_record_sha256, "source_record_sha256")


@dataclass(frozen=True, slots=True)
class SessionSummary:
    """Daily-like OHLCV derived exclusively from canonical minute records."""

    instrument_id: str
    source_ticker: str
    session_id: str
    trading_date: str
    open_ticks: int
    high_ticks: int
    low_ticks: int
    close_ticks: int
    volume_shares: int
    observed_minutes: int
    expected_minutes: int
    data_quality_state: FeatureValidity
    source_record_sequence_sha256: str
    schema_version: str = SESSION_SUMMARY_SCHEMA_VERSION


@dataclass(frozen=True, slots=True)
class NormalizationStat:
    """Integer sufficient statistics fitted on TRAIN rows only."""

    feature_name: str
    count: int
    total: int
    sum_squares: int
    maximum_fit_exchange_time_ns: int

    def normalize(self, value: int | None) -> int | None:
        """Return a deterministic z-score in parts per million."""
        if value is None:
            return None
        if self.count <= 0:
            return None
        centered = value * self.count - self.total
        variance_numerator = self.sum_squares * self.count - self.total * self.total
        if variance_numerator <= 0:
            return 0
        return _checked_int64(
            _divide_round_half_away(
                centered * PPM_SCALE, math.isqrt(variance_numerator)
            ),
            "normalized_feature_ppm",
        )


@dataclass(frozen=True, slots=True)
class AggregatePoint:
    """Bounded cross-sectional return average at one exact minute endpoint."""

    market_return_ppm: int | None
    sector_returns_ppm: Mapping[str, int]
    market_available_at_ns: int
    sector_available_at_ns: Mapping[str, int]


@dataclass(frozen=True, slots=True)
class LabelCoverage:
    """Visible coverage for one universe entry and one required horizon."""

    symbol: str
    instrument_id: str | None
    resolution_code: str
    horizon: str
    samples: int
    valid: int
    missing: int
    invalid: int
    reason_counts: Mapping[str, int]


@dataclass(frozen=True, slots=True)
class LeakageReport:
    """Machine-readable validation result produced before dataset acceptance."""

    checked_samples: int
    violations: tuple[str, ...]
    status: str
    schema_version: str = "1.0.0"


@dataclass(slots=True)
class _MutableCoverage:
    instrument_id: str | None
    resolution_code: str
    samples: int
    valid: int
    missing: int
    invalid: int
    reason_counts: dict[str, int]


class _CoverageAccumulator:
    def __init__(self, universe: UniverseSnapshot) -> None:
        self._universe = universe
        self._entry_by_instrument: dict[str, UniverseEntry] = {}
        self._counts: dict[tuple[str, str], _MutableCoverage] = {}
        for entry in universe.ordered_entries:
            instrument_id = (
                None
                if entry.instrument_id is None
                else cast("_HexIdentifier", entry.instrument_id).hex()
            )
            if instrument_id is not None:
                self._entry_by_instrument[instrument_id] = entry
            default_reason = (
                DatasetBuildCode.MISSING_CANONICAL_DATA.value
                if instrument_id is not None
                else entry.resolution_code.name
            )
            for horizon in REQUIRED_HORIZON_LABELS:
                self._counts[(entry.symbol, horizon)] = _MutableCoverage(
                    instrument_id,
                    entry.resolution_code.name,
                    0,
                    0,
                    0,
                    0,
                    {default_reason: 1},
                )

    def add(self, sample: FeatureSample) -> None:
        entry = self._entry_by_instrument.get(sample.instrument_id)
        if entry is None:
            raise DatasetBuildError(
                DatasetBuildCode.UNRESOLVED_INSTRUMENT,
                "sample instrument is outside the universe",
            )
        for label in sample.labels:
            values = self._counts[(entry.symbol, label.horizon)]
            values.samples += 1
            if values.samples == 1:
                values.reason_counts.clear()
            if label.validity is LabelValidity.VALID:
                values.valid += 1
            elif label.validity is LabelValidity.MISSING:
                values.missing += 1
            else:
                values.invalid += 1
            if label.missing_reason is not None:
                values.reason_counts[label.missing_reason] = (
                    values.reason_counts.get(label.missing_reason, 0) + 1
                )

    def freeze(self) -> tuple[LabelCoverage, ...]:
        return tuple(
            LabelCoverage(
                entry.symbol,
                values.instrument_id,
                values.resolution_code,
                horizon,
                values.samples,
                values.valid,
                values.missing,
                values.invalid,
                dict(sorted(values.reason_counts.items())),
            )
            for entry in self._universe.ordered_entries
            for horizon in REQUIRED_HORIZON_LABELS
            for values in (self._counts[(entry.symbol, horizon)],)
        )


class _StatAccumulator:
    __slots__ = ("count", "maximum_time_ns", "sum_squares", "total")

    def __init__(self) -> None:
        self.count = 0
        self.total = 0
        self.sum_squares = 0
        self.maximum_time_ns = 0

    def add(self, value: int | None, as_of_ns: int) -> None:
        if value is None:
            return
        self.count += 1
        self.total += value
        self.sum_squares += value * value
        self.maximum_time_ns = max(self.maximum_time_ns, as_of_ns)


def fit_normalization(
    samples: Iterable[FeatureSample],
) -> tuple[NormalizationStat, ...]:
    """Fit integer normalization statistics exclusively from TRAIN samples."""
    accumulators = [_StatAccumulator() for _ in FEATURE_NAMES]
    for sample in samples:
        if sample.split != "TRAIN":
            raise DatasetBuildError(
                DatasetBuildCode.NORMALIZATION_LEAKAGE,
                "normalization input includes a non-TRAIN row",
            )
        for accumulator, value in zip(accumulators, sample.raw_features, strict=True):
            accumulator.add(value, sample.as_of_exchange_time_ns)
    return tuple(
        NormalizationStat(
            name,
            accumulator.count,
            accumulator.total,
            accumulator.sum_squares,
            accumulator.maximum_time_ns,
        )
        for name, accumulator in zip(FEATURE_NAMES, accumulators, strict=True)
    )


def validate_leakage(
    samples: Iterable[FeatureSample],
    split_plan: SplitPlan,
    normalization: Sequence[NormalizationStat],
) -> LeakageReport:
    """Recheck temporal, split, event, target, and fit-cutoff invariants."""
    violations: list[str] = []
    checked = 0
    train_cutoff = 0
    if split_plan.train_sessions:
        train_cutoff_session = split_plan.train_sessions[-1]
    else:
        train_cutoff_session = ""
    for sample in samples:
        checked += 1
        expected = split_plan.session_splits.get(sample.session_id)
        if expected != sample.split:
            violations.append(DatasetBuildCode.NON_CHRONOLOGICAL_SPLIT.value)
        if sample.session_id == train_cutoff_session:
            train_cutoff = max(train_cutoff, sample.as_of_exchange_time_ns)
        if sample.feature_end_exchange_time_ns != sample.as_of_exchange_time_ns:
            violations.append(DatasetBuildCode.LABEL_FEATURE_OVERLAP.value)
        if sample.max_event_availability_time_ns > sample.knowledge_cutoff_time_ns:
            violations.append(DatasetBuildCode.FUTURE_EVENT_REVISION.value)
        for label in sample.labels:
            if (
                label.validity is LabelValidity.VALID
                and cast("int", label.target_exchange_time_ns)
                <= sample.feature_end_exchange_time_ns
            ):
                violations.append(DatasetBuildCode.LABEL_FEATURE_OVERLAP.value)
    if (
        len(split_plan.first_purge_sessions) != PURGE_SESSIONS
        or len(split_plan.second_purge_sessions) != PURGE_SESSIONS
    ):
        violations.append(DatasetBuildCode.PURGE_EMBARGO_VIOLATION.value)
    if any(stat.maximum_fit_exchange_time_ns > train_cutoff for stat in normalization):
        violations.append(DatasetBuildCode.NORMALIZATION_LEAKAGE.value)
    unique = tuple(sorted(set(violations)))
    return LeakageReport(checked, unique, "PASS" if not unique else "REJECTED")


@dataclass(slots=True)
class _MutableSessionSummary:
    instrument_id: str
    source_ticker: str
    session_id: str
    trading_date: str
    open_ticks: int
    high_ticks: int
    low_ticks: int
    close_ticks: int
    volume_shares: int
    observed_minutes: int
    expected_minutes: int
    degraded: bool
    sequence_hasher: _Hasher

    @classmethod
    def start(cls, record: CanonicalMinuteRecord) -> _MutableSessionSummary:
        return cls(
            record.instrument_id,
            record.source_ticker,
            record.session_id,
            record.trading_date.isoformat(),
            record.open_ticks,
            record.high_ticks,
            record.low_ticks,
            record.close_ticks,
            record.volume_shares,
            1,
            (
                record.session_close_exchange_time_ns
                - record.session_open_exchange_time_ns
            )
            // MINUTE_NS,
            record.data_quality_state is MinuteDataQuality.DEGRADED,
            hashlib.sha256(bytes.fromhex(record.record_sha256)),
        )

    def add(self, record: CanonicalMinuteRecord) -> None:
        self.high_ticks = max(self.high_ticks, record.high_ticks)
        self.low_ticks = min(self.low_ticks, record.low_ticks)
        self.close_ticks = record.close_ticks
        self.volume_shares = _checked_int64(
            self.volume_shares + record.volume_shares,
            "session volume",
            nonnegative=True,
        )
        self.observed_minutes += 1
        self.degraded = self.degraded or (
            record.data_quality_state is MinuteDataQuality.DEGRADED
        )
        self.sequence_hasher.update(bytes.fromhex(record.record_sha256))

    def freeze(self) -> SessionSummary:
        state = (
            FeatureValidity.VALID
            if not self.degraded and self.observed_minutes == self.expected_minutes
            else FeatureValidity.DEGRADED
        )
        return SessionSummary(
            self.instrument_id,
            self.source_ticker,
            self.session_id,
            self.trading_date,
            self.open_ticks,
            self.high_ticks,
            self.low_ticks,
            self.close_ticks,
            self.volume_shares,
            self.observed_minutes,
            self.expected_minutes,
            state,
            self.sequence_hasher.hexdigest(),
        )


class FeatureDatasetBuilder:
    """Multi-pass, repeatable builder over immutable canonical minute records."""

    def __init__(
        self,
        universe: UniverseSnapshot,
        calendar: ExchangeCalendar,
        *,
        sectors: SectorIndex | None = None,
        events: EventIndex | None = None,
        maximum_records_per_instrument: int = MAX_RECORDS_PER_INSTRUMENT,
    ) -> None:
        """Bind immutable universe, calendar, sector, and event snapshots."""
        if maximum_records_per_instrument <= 0:
            raise DatasetBuildError(
                DatasetBuildCode.INVALID_CONFIGURATION,
                "per-instrument record bound must be positive",
            )
        self.universe = universe
        self.calendar = calendar
        self.sectors = SectorIndex(()) if sectors is None else sectors
        self.events = EventIndex(()) if events is None else events
        self.horizons = required_horizon_specs(calendar.calendar_version)
        self.maximum_records_per_instrument = maximum_records_per_instrument
        self._session_by_id = {item.session_id: item for item in calendar.sessions}
        self._calendar_session_order = {
            item.session_id: index for index, item in enumerate(calendar.sessions)
        }
        self._resolved = {
            cast("_HexIdentifier", entry.instrument_id).hex(): entry
            for entry in universe.ordered_entries
            if entry.resolution_code is InstrumentResolutionCode.RESOLVED
            and entry.instrument_id is not None
        }

    def _records(
        self, source: CanonicalDatasetSource, instrument_id: str
    ) -> list[CanonicalMinuteRecord]:
        records: list[CanonicalMinuteRecord] = []
        previous = 0
        endpoints: set[int] = set()
        for record in source.iter_instrument(instrument_id):
            if len(records) >= self.maximum_records_per_instrument:
                raise DatasetBuildError(
                    DatasetBuildCode.STORAGE_LIMIT,
                    "per-instrument canonical record bound is exceeded",
                )
            if record.instrument_id != instrument_id:
                raise DatasetBuildError(
                    DatasetBuildCode.CORRUPT_CANONICAL_INPUT,
                    "source emitted the wrong instrument",
                )
            endpoint = record.minute_end_exchange_time_ns
            if endpoint in endpoints:
                raise DatasetBuildError(
                    DatasetBuildCode.DUPLICATE_MINUTE,
                    "canonical minute repeats",
                )
            if endpoint <= previous:
                raise DatasetBuildError(
                    DatasetBuildCode.NON_CHRONOLOGICAL_INPUT,
                    "canonical minutes are not chronological",
                )
            session = self._session_by_id.get(record.session_id)
            if (
                session is None
                or record.session_open_exchange_time_ns
                != session.open_exchange_event_time_ns
                or record.session_close_exchange_time_ns
                != session.close_exchange_event_time_ns
                or not session.open_exchange_event_time_ns
                < endpoint
                <= session.close_exchange_event_time_ns
                or record.minute_start_exchange_time_ns + MINUTE_NS != endpoint
                or endpoint > record.source_availability_time_ns
                or record.source_availability_time_ns > record.local_receipt_time_ns
                or record.local_receipt_time_ns > record.local_processing_time_ns
                or (
                    record.source_publication_time_ns is not None
                    and record.source_publication_time_ns
                    > record.source_availability_time_ns
                )
                or _digest(record.payload(include_hash=False)) != record.record_sha256
            ):
                raise DatasetBuildError(
                    DatasetBuildCode.CORRUPT_CANONICAL_INPUT,
                    "canonical row conflicts with calendar or record hash",
                )
            previous = endpoint
            endpoints.add(endpoint)
            records.append(record)
        return records

    def observed_sessions(self, source: CanonicalDatasetSource) -> tuple[str, ...]:
        """Return calendar-ordered session identities observed by any symbol."""
        observed: set[str] = set()
        for instrument_id in source.instrument_ids:
            if instrument_id not in self._resolved:
                raise DatasetBuildError(
                    DatasetBuildCode.UNRESOLVED_INSTRUMENT,
                    "canonical source includes an instrument outside the universe",
                )
            observed.update(
                record.session_id for record in self._records(source, instrument_id)
            )
        return tuple(
            session.session_id
            for session in self.calendar.sessions
            if session.session_id in observed
        )

    def session_summaries(
        self, source: CanonicalDatasetSource
    ) -> Iterator[SessionSummary]:
        """Derive session OHLCV from minute rows without a daily data source."""
        for instrument_id in source.instrument_ids:
            current: _MutableSessionSummary | None = None
            for record in self._records(source, instrument_id):
                if current is None or current.session_id != record.session_id:
                    if current is not None:
                        yield current.freeze()
                    current = _MutableSessionSummary.start(record)
                else:
                    current.add(record)
            if current is not None:
                yield current.freeze()

    def cross_sectional_aggregates(
        self, source: CanonicalDatasetSource
    ) -> Mapping[int, AggregatePoint]:
        """Compute market and PIT-sector means in a bounded first pass."""
        market: dict[int, list[int]] = defaultdict(lambda: [0, 0, 0])
        sector: dict[tuple[int, str], list[int]] = defaultdict(lambda: [0, 0, 0])
        for instrument_id in source.instrument_ids:
            previous: CanonicalMinuteRecord | None = None
            for record in self._records(source, instrument_id):
                if (
                    previous is not None
                    and previous.session_id == record.session_id
                    and previous.minute_end_exchange_time_ns + MINUTE_NS
                    == record.minute_end_exchange_time_ns
                ):
                    value = _return_ppm(record.close_ticks, previous.close_ticks)
                    point = market[record.minute_end_exchange_time_ns]
                    point[0] += value
                    point[1] += 1
                    point[2] = max(point[2], record.local_processing_time_ns)
                    revision = self.sectors.at(
                        instrument_id,
                        event_time_ns=record.minute_end_exchange_time_ns,
                        known_at_ns=record.minute_end_exchange_time_ns,
                    )
                    if revision is not None:
                        sector_point = sector[
                            (record.minute_end_exchange_time_ns, revision.sector_id)
                        ]
                        sector_point[0] += value
                        sector_point[1] += 1
                        sector_point[2] = max(
                            sector_point[2], record.local_processing_time_ns
                        )
                previous = record
        if len(market) + len(sector) > MAX_AGGREGATE_POINTS:
            raise DatasetBuildError(
                DatasetBuildCode.STORAGE_LIMIT,
                "cross-sectional aggregate point bound is exceeded",
            )
        result: dict[int, AggregatePoint] = {}
        sector_by_endpoint: dict[int, dict[str, int]] = defaultdict(dict)
        sector_availability: dict[int, dict[str, int]] = defaultdict(dict)
        for (endpoint, sector_id), (total, count, available_at_ns) in sector.items():
            sector_by_endpoint[endpoint][sector_id] = _divide_round_half_away(
                total, count
            )
            sector_availability[endpoint][sector_id] = available_at_ns
        for endpoint, values in market.items():
            result[endpoint] = AggregatePoint(
                _divide_round_half_away(values[0], values[1]),
                sector_by_endpoint.get(endpoint, {}),
                values[2],
                sector_availability.get(endpoint, {}),
            )
        return result

    @staticmethod
    def _corporate_action_version(record: CanonicalMinuteRecord) -> str:
        return _digest(
            {
                "as_of_exchange_time_ns": record.minute_end_exchange_time_ns,
                "corporate_action_ids": sorted(record.corporate_action_ids),
                "instrument_revision_id": record.instrument_revision_id,
                "tick_revision_id": record.tick_revision_id,
            }
        )

    def _labels(
        self,
        record: CanonicalMinuteRecord,
        records_by_endpoint: Mapping[int, CanonicalMinuteRecord],
    ) -> tuple[FeatureLabel, ...]:
        labels: list[FeatureLabel] = []
        action_version = self._corporate_action_version(record)
        for horizon in self.horizons:
            try:
                target_time = resolve_horizon(
                    horizon, record.minute_end_exchange_time_ns, self.calendar
                ).target_exchange_event_time_ns
            except HorizonContractError as error:
                labels.append(
                    FeatureLabel(
                        horizon.label,
                        None,
                        None,
                        None,
                        None,
                        action_version,
                        LabelValidity.MISSING,
                        error.code.name,
                    )
                )
                continue
            target = records_by_endpoint.get(target_time)
            if target is None:
                labels.append(
                    FeatureLabel(
                        horizon.label,
                        target_time,
                        None,
                        None,
                        None,
                        action_version,
                        LabelValidity.MISSING,
                        DatasetBuildCode.MISSING_TARGET_BAR.value,
                    )
                )
                continue
            reason: DatasetBuildCode | None = None
            if (
                target.tick_revision_id != record.tick_revision_id
                or target.tick_value_currency_nanos != record.tick_value_currency_nanos
            ):
                reason = DatasetBuildCode.TICK_GRID_CHANGED
            elif target.corporate_action_ids != record.corporate_action_ids:
                reason = DatasetBuildCode.CORPORATE_ACTION_CHANGED
            if reason is not None:
                labels.append(
                    FeatureLabel(
                        horizon.label,
                        target_time,
                        None,
                        None,
                        None,
                        action_version,
                        LabelValidity.INVALID,
                        reason.value,
                    )
                )
                continue
            value = _return_ppm(target.close_ticks, record.close_ticks)
            labels.append(
                FeatureLabel(
                    horizon.label,
                    target_time,
                    value,
                    1 if value > 0 else -1 if value < 0 else 0,
                    target.close_ticks,
                    action_version,
                    LabelValidity.VALID,
                    None,
                )
            )
        return tuple(labels)

    @staticmethod
    def _summaries_for_records(
        records: Sequence[CanonicalMinuteRecord],
    ) -> tuple[SessionSummary, ...]:
        summaries: list[SessionSummary] = []
        current: _MutableSessionSummary | None = None
        for record in records:
            if current is None or current.session_id != record.session_id:
                if current is not None:
                    summaries.append(current.freeze())
                current = _MutableSessionSummary.start(record)
            else:
                current.add(record)
        if current is not None:
            summaries.append(current.freeze())
        return tuple(summaries)

    def _feature_values(
        self,
        record: CanonicalMinuteRecord,
        records_by_endpoint: Mapping[int, CanonicalMinuteRecord],
        prior_session_summaries: Sequence[SessionSummary],
        aggregates: Mapping[int, AggregatePoint],
    ) -> tuple[
        tuple[int | None, ...], FeatureValidity, tuple[str, ...], tuple[str, ...], int
    ]:
        reasons = list(record.data_quality_reasons)
        session_minute = (
            record.minute_end_exchange_time_ns - record.session_open_exchange_time_ns
        ) // MINUTE_NS
        returns: list[int | None] = []
        for minutes in RETURN_WINDOWS:
            prior_endpoint = record.minute_end_exchange_time_ns - minutes * MINUTE_NS
            prior = records_by_endpoint.get(prior_endpoint)
            if prior is None or prior.session_id != record.session_id:
                returns.append(None)
            else:
                returns.append(_return_ppm(record.close_ticks, prior.close_ticks))
        prior_volumes: list[int] = []
        return_squares: list[int] = []
        prior = record
        for offset in range(1, REALIZED_VOLATILITY_WINDOW + 1):
            candidate = records_by_endpoint.get(
                record.minute_end_exchange_time_ns - offset * MINUTE_NS
            )
            if candidate is None or candidate.session_id != record.session_id:
                break
            if offset <= RELATIVE_VOLUME_WINDOW:
                prior_volumes.append(candidate.volume_shares)
            one_minute_return = _return_ppm(prior.close_ticks, candidate.close_ticks)
            return_squares.append(one_minute_return * one_minute_return)
            prior = candidate
        relative_volume = (
            None
            if not prior_volumes or _mean_int(prior_volumes) == 0
            else _divide_round_half_away(
                record.volume_shares * PPM_SCALE, _mean_int(prior_volumes)
            )
        )
        realized_volatility = (
            None if not return_squares else math.isqrt(_mean_int(return_squares))
        )
        high_low = _divide_round_half_away(
            (record.high_ticks - record.low_ticks) * PPM_SCALE,
            record.low_ticks,
        )
        previous_summary = (
            prior_session_summaries[-1] if prior_session_summaries else None
        )
        intraday_gap = (
            None
            if previous_summary is None
            else _return_ppm(record.open_ticks, previous_summary.close_ticks)
        )
        rolling = tuple(prior_session_summaries[-ROLLING_SESSION_WINDOW:])
        rolling_return = (
            None
            if len(rolling) < ROLLING_SESSION_WINDOW
            else _mean_int(
                [_return_ppm(item.close_ticks, item.open_ticks) for item in rolling]
            )
        )
        rolling_volume = (
            None
            if len(rolling) < ROLLING_SESSION_WINDOW
            else _mean_int([item.volume_shares for item in rolling])
        )
        aggregate = aggregates.get(record.minute_end_exchange_time_ns)
        market_return = (
            None
            if aggregate is None
            or aggregate.market_available_at_ns > record.local_processing_time_ns
            else aggregate.market_return_ppm
        )
        own_return = returns[0]
        market_relative = (
            None
            if own_return is None or market_return is None
            else own_return - market_return
        )
        sector_revision = self.sectors.at(
            record.instrument_id,
            event_time_ns=record.minute_end_exchange_time_ns,
            known_at_ns=record.minute_end_exchange_time_ns,
        )
        sector_return = (
            None
            if aggregate is None
            or sector_revision is None
            or aggregate.sector_available_at_ns.get(
                sector_revision.sector_id, MAX_INT64
            )
            > record.local_processing_time_ns
            else aggregate.sector_returns_ppm.get(sector_revision.sector_id)
        )
        sector_relative = (
            None
            if own_return is None or sector_return is None
            else own_return - sector_return
        )
        if market_return is None:
            reasons.append("MARKET_AGGREGATE_UNAVAILABLE")
        if sector_return is None:
            reasons.append("SECTOR_AGGREGATE_UNAVAILABLE")
        news, macro, event_ids, event_availability = self.events.active(
            record.instrument_id, record.minute_end_exchange_time_ns
        )
        expected_minutes = (
            record.session_close_exchange_time_ns - record.session_open_exchange_time_ns
        ) // MINUTE_NS
        features: tuple[int | None, ...] = (
            *returns,
            record.volume_shares,
            relative_volume,
            realized_volatility,
            high_low,
            intraday_gap,
            session_minute,
            _divide_round_half_away(session_minute * PPM_SCALE, expected_minutes),
            market_return,
            market_relative,
            sector_return,
            sector_relative,
            rolling_return,
            rolling_volume,
            int(news),
            int(macro),
            int(record.data_quality_state is MinuteDataQuality.VALID),
            len(record.data_quality_reasons),
        )
        if any(
            value is None for value in (*returns, relative_volume, realized_volatility)
        ):
            validity = FeatureValidity.WARMUP
            reasons.append("ROLLING_WINDOW_WARMUP")
        elif record.data_quality_state is MinuteDataQuality.DEGRADED or reasons:
            validity = FeatureValidity.DEGRADED
        else:
            validity = FeatureValidity.VALID
        return (
            features,
            validity,
            tuple(sorted(set(reasons))),
            event_ids,
            event_availability,
        )

    def iter_samples(
        self,
        source: CanonicalDatasetSource,
        split_plan: SplitPlan,
        aggregates: Mapping[int, AggregatePoint],
        *,
        normalization: Sequence[NormalizationStat] | None = None,
    ) -> Iterator[FeatureSample]:
        """Stream deterministic rows; at most one instrument history is resident."""
        if (
            normalization is not None
            and tuple(item.feature_name for item in normalization) != FEATURE_NAMES
        ):
            raise DatasetBuildError(
                DatasetBuildCode.INVALID_CONFIGURATION,
                "normalization feature order is incompatible",
            )
        for instrument_id in source.instrument_ids:
            entry = self._resolved.get(instrument_id)
            if entry is None:
                raise DatasetBuildError(
                    DatasetBuildCode.UNRESOLVED_INSTRUMENT,
                    "source instrument is not resolved in the universe",
                )
            records = self._records(source, instrument_id)
            records_by_endpoint = {
                record.minute_end_exchange_time_ns: record for record in records
            }
            summaries = self._summaries_for_records(records)
            summaries_by_session = {item.session_id: item for item in summaries}
            for record in records:
                split = split_plan.session_splits.get(record.session_id)
                if split is None:
                    continue
                session_index = self._calendar_session_order[record.session_id]
                prior_summaries = tuple(
                    summaries_by_session[item.session_id]
                    for item in self.calendar.sessions[:session_index]
                    if item.session_id in summaries_by_session
                )
                raw, validity, reasons, event_ids, event_availability = (
                    self._feature_values(
                        record,
                        records_by_endpoint,
                        prior_summaries,
                        aggregates,
                    )
                )
                normalized = (
                    tuple(None for _ in FEATURE_NAMES)
                    if normalization is None
                    else tuple(
                        stat.normalize(value)
                        for stat, value in zip(normalization, raw, strict=True)
                    )
                )
                labels = self._labels(record, records_by_endpoint)
                earliest = max(
                    record.session_open_exchange_time_ns,
                    record.minute_end_exchange_time_ns - 60 * MINUTE_NS,
                )
                if len(prior_summaries) >= ROLLING_SESSION_WINDOW:
                    earliest_session = self._session_by_id[
                        prior_summaries[-ROLLING_SESSION_WINDOW].session_id
                    ]
                    earliest = min(
                        earliest, earliest_session.open_exchange_event_time_ns
                    )
                identity = {
                    "as_of_exchange_time_ns": record.minute_end_exchange_time_ns,
                    "dataset_seed": DATASET_SEED,
                    "instrument_id": instrument_id,
                    "labels": [
                        {
                            "corporate_action_version": label.corporate_action_version,
                            "future_price_ticks": label.future_price_ticks,
                            "horizon": label.horizon,
                            "missing_reason": label.missing_reason,
                            "return_ppm": label.return_ppm,
                            "target_exchange_time_ns": label.target_exchange_time_ns,
                            "validity": label.validity.value,
                        }
                        for label in labels
                    ],
                    "source_record_sha256": record.record_sha256,
                    "split": split,
                }
                yield FeatureSample(
                    _digest(identity),
                    split,
                    instrument_id,
                    entry.symbol,
                    record.session_id,
                    record.trading_date.isoformat(),
                    record.minute_end_exchange_time_ns,
                    record.minute_end_exchange_time_ns,
                    earliest,
                    record.minute_end_exchange_time_ns,
                    raw,
                    normalized,
                    validity,
                    reasons,
                    event_ids,
                    event_availability,
                    record.record_sha256,
                    labels,
                )

    def fit(
        self,
        source: CanonicalDatasetSource,
        split_plan: SplitPlan,
        aggregates: Mapping[int, AggregatePoint],
    ) -> tuple[NormalizationStat, ...]:
        """Run the second pass and fit only rows assigned to TRAIN."""
        samples = (
            sample
            for sample in self.iter_samples(source, split_plan, aggregates)
            if sample.split == "TRAIN"
        )
        return fit_normalization(samples)


def build_label_coverage(
    universe: UniverseSnapshot, samples: Iterable[FeatureSample]
) -> tuple[LabelCoverage, ...]:
    """Report every source-order ticker/horizon, including absent instruments."""
    accumulator = _CoverageAccumulator(universe)
    for sample in samples:
        accumulator.add(sample)
    return accumulator.freeze()


_LABEL_ARROW_TYPE = pa.struct(
    [
        pa.field("horizon", pa.string(), nullable=False),
        pa.field("target_exchange_time_ns", pa.int64()),
        pa.field("return_ppm", pa.int64()),
        pa.field("direction", pa.int8()),
        pa.field("future_price_ticks", pa.int64()),
        pa.field("corporate_action_version", pa.string(), nullable=False),
        pa.field("validity", pa.string(), nullable=False),
        pa.field("missing_reason", pa.string()),
    ]
)
_FEATURE_ARROW_SCHEMA = pa.schema(
    [
        pa.field("schema_version", pa.string(), nullable=False),
        pa.field("sample_id", pa.string(), nullable=False),
        pa.field("split", pa.string(), nullable=False),
        pa.field("instrument_id", pa.string(), nullable=False),
        pa.field("source_ticker", pa.string(), nullable=False),
        pa.field("session_id", pa.string(), nullable=False),
        pa.field("trading_date", pa.string(), nullable=False),
        pa.field("as_of_exchange_time_ns", pa.int64(), nullable=False),
        pa.field("knowledge_cutoff_time_ns", pa.int64(), nullable=False),
        pa.field("feature_start_exchange_time_ns", pa.int64(), nullable=False),
        pa.field("feature_end_exchange_time_ns", pa.int64(), nullable=False),
        pa.field("raw_features", pa.list_(pa.int64()), nullable=False),
        pa.field("normalized_features_ppm", pa.list_(pa.int64()), nullable=False),
        pa.field("feature_validity", pa.string(), nullable=False),
        pa.field("feature_reason_codes", pa.list_(pa.string()), nullable=False),
        pa.field("advisory_event_ids", pa.list_(pa.string()), nullable=False),
        pa.field("max_event_availability_time_ns", pa.int64(), nullable=False),
        pa.field("source_record_sha256", pa.string(), nullable=False),
        pa.field("labels", pa.list_(_LABEL_ARROW_TYPE), nullable=False),
    ],
    metadata={
        b"aegis.schema": b"leakage-safe-feature-label-row",
        b"aegis.schema_version": FEATURE_RECORD_SCHEMA_VERSION.encode("ascii"),
        b"aegis.feature_names": ",".join(FEATURE_NAMES).encode("ascii"),
        b"aegis.dataset_seed": str(DATASET_SEED).encode("ascii"),
    },
)
_SESSION_ARROW_SCHEMA = pa.schema(
    [
        pa.field("schema_version", pa.string(), nullable=False),
        pa.field("instrument_id", pa.string(), nullable=False),
        pa.field("source_ticker", pa.string(), nullable=False),
        pa.field("session_id", pa.string(), nullable=False),
        pa.field("trading_date", pa.string(), nullable=False),
        pa.field("open_ticks", pa.int64(), nullable=False),
        pa.field("high_ticks", pa.int64(), nullable=False),
        pa.field("low_ticks", pa.int64(), nullable=False),
        pa.field("close_ticks", pa.int64(), nullable=False),
        pa.field("volume_shares", pa.int64(), nullable=False),
        pa.field("observed_minutes", pa.int32(), nullable=False),
        pa.field("expected_minutes", pa.int32(), nullable=False),
        pa.field("data_quality_state", pa.string(), nullable=False),
        pa.field("source_record_sequence_sha256", pa.string(), nullable=False),
    ],
    metadata={
        b"aegis.schema": b"derived-session-summary",
        b"aegis.schema_version": SESSION_SUMMARY_SCHEMA_VERSION.encode("ascii"),
        b"aegis.source": b"canonical minute data only",
    },
)


def _label_document(label: FeatureLabel) -> dict[str, object]:
    return {
        "corporate_action_version": label.corporate_action_version,
        "direction": label.direction,
        "future_price_ticks": label.future_price_ticks,
        "horizon": label.horizon,
        "missing_reason": label.missing_reason,
        "return_ppm": label.return_ppm,
        "target_exchange_time_ns": label.target_exchange_time_ns,
        "validity": label.validity.value,
    }


def _sample_document(sample: FeatureSample) -> dict[str, object]:
    return {
        "advisory_event_ids": list(sample.advisory_event_ids),
        "as_of_exchange_time_ns": sample.as_of_exchange_time_ns,
        "feature_end_exchange_time_ns": sample.feature_end_exchange_time_ns,
        "feature_reason_codes": list(sample.feature_reason_codes),
        "feature_start_exchange_time_ns": sample.feature_start_exchange_time_ns,
        "feature_validity": sample.feature_validity.value,
        "instrument_id": sample.instrument_id,
        "knowledge_cutoff_time_ns": sample.knowledge_cutoff_time_ns,
        "labels": [_label_document(label) for label in sample.labels],
        "max_event_availability_time_ns": sample.max_event_availability_time_ns,
        "normalized_features_ppm": list(sample.normalized_features_ppm),
        "raw_features": list(sample.raw_features),
        "sample_id": sample.sample_id,
        "schema_version": sample.schema_version,
        "session_id": sample.session_id,
        "source_record_sha256": sample.source_record_sha256,
        "source_ticker": sample.source_ticker,
        "split": sample.split,
        "trading_date": sample.trading_date,
    }


def _summary_document(summary: SessionSummary) -> dict[str, object]:
    return {
        "close_ticks": summary.close_ticks,
        "data_quality_state": summary.data_quality_state.value,
        "expected_minutes": summary.expected_minutes,
        "high_ticks": summary.high_ticks,
        "instrument_id": summary.instrument_id,
        "low_ticks": summary.low_ticks,
        "observed_minutes": summary.observed_minutes,
        "open_ticks": summary.open_ticks,
        "schema_version": summary.schema_version,
        "session_id": summary.session_id,
        "source_record_sequence_sha256": summary.source_record_sequence_sha256,
        "source_ticker": summary.source_ticker,
        "trading_date": summary.trading_date,
        "volume_shares": summary.volume_shares,
    }


@dataclass(frozen=True, slots=True)
class _StagedParquet:
    path: Path
    sha256: str
    size_bytes: int
    record_count: int


def _write_parquet(
    path: Path,
    schema: pa.Schema,
    rows: Iterable[Mapping[str, object]],
    *,
    batch_rows: int,
) -> _StagedParquet:
    if path.exists() or path.is_symlink() or batch_rows <= 0:
        raise DatasetBuildError(
            DatasetBuildCode.INVALID_CONFIGURATION,
            "staged output path or row-group size is invalid",
        )
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    writer = pq.ParquetWriter(
        path,
        schema,
        version="2.6",
        compression="zstd",
        compression_level=3,
        write_statistics=True,
        data_page_version="2.0",
        write_page_index=True,
    )
    batch: list[Mapping[str, object]] = []
    count = 0
    try:
        for row in rows:
            batch.append(row)
            if len(batch) == batch_rows:
                writer.write_table(pa.Table.from_pylist(batch, schema=schema))
                count += len(batch)
                batch.clear()
        if batch:
            writer.write_table(pa.Table.from_pylist(batch, schema=schema))
            count += len(batch)
    except BaseException:
        writer.close()
        path.unlink(missing_ok=True)
        raise
    writer.close()
    if count <= 0:
        path.unlink(missing_ok=True)
        raise DatasetBuildError(
            DatasetBuildCode.CORRUPT_CANONICAL_INPUT,
            "dataset partition contains no rows",
        )
    with path.open("rb") as source_file:
        digest = hashlib.file_digest(source_file, "sha256").hexdigest()
    return _StagedParquet(path, digest, path.stat().st_size, count)


def _write_bytes(path: Path, payload: bytes) -> None:
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


@dataclass(frozen=True, slots=True)
class PublishedFeatureDataset:
    """Accepted dataset identity and manifest-last publication evidence."""

    dataset_id: str
    manifest_sha256: str
    manifest_path: Path
    sample_count: int
    session_summary_count: int
    object_count: int
    coverage: tuple[LabelCoverage, ...]
    leakage: LeakageReport


def _normalization_document(stat: NormalizationStat) -> dict[str, object]:
    return {
        "count": stat.count,
        "feature_name": stat.feature_name,
        "maximum_fit_exchange_time_ns": stat.maximum_fit_exchange_time_ns,
        "sum": stat.total,
        "sum_squares": stat.sum_squares,
    }


def _coverage_document(item: LabelCoverage) -> dict[str, object]:
    return {
        "horizon": item.horizon,
        "instrument_id": item.instrument_id,
        "invalid": item.invalid,
        "missing": item.missing,
        "reason_counts": dict(item.reason_counts),
        "resolution_code": item.resolution_code,
        "samples": item.samples,
        "symbol": item.symbol,
        "valid": item.valid,
    }


def _verify_published_objects(
    repository: DataRepository, objects: Sequence[Mapping[str, object]]
) -> None:
    """Re-hash every manifest object before accepting an idempotent rebuild."""
    root = repository.root.resolve(strict=True)
    for item in objects:
        relative = Path(cast("str", item["storage_path"]))
        if (
            relative.is_absolute()
            or not relative.parts
            or any(part in {"", ".", ".."} for part in relative.parts)
        ):
            raise DatasetBuildError(
                DatasetBuildCode.CORRUPT_CANONICAL_INPUT,
                "unsafe published object path",
            )
        candidate = repository.root / relative
        if candidate.is_symlink() or not candidate.is_file():
            raise DatasetBuildError(
                DatasetBuildCode.CORRUPT_CANONICAL_INPUT,
                "published object is not a regular file",
            )
        resolved = candidate.resolve(strict=True)
        if not resolved.is_relative_to(root):
            raise DatasetBuildError(
                DatasetBuildCode.CORRUPT_CANONICAL_INPUT,
                "published object escapes repository root",
            )
        status = candidate.stat()
        if status.st_size != cast("int", item["size_bytes_decimal"]):
            raise DatasetBuildError(
                DatasetBuildCode.CORRUPT_CANONICAL_INPUT,
                "published object size mismatch",
            )
        with candidate.open("rb") as source_file:
            digest = hashlib.file_digest(source_file, "sha256").hexdigest()
        if digest != cast("str", item["object_sha256"]):
            raise DatasetBuildError(
                DatasetBuildCode.CORRUPT_CANONICAL_INPUT,
                "published object SHA-256 mismatch",
            )


def _load_published_dataset(
    repository: DataRepository, path: Path, build_key: str
) -> PublishedFeatureDataset:
    try:
        encoded = path.read_bytes()
        document = cast("Mapping[str, object]", json.loads(encoded))
        supplied_hash = cast("str", document["manifest_sha256"])
        body = dict(document)
        body.pop("manifest_sha256")
        if (
            document["build_key"] != build_key
            or supplied_hash != _digest(body)
            or document["dataset_id"] != _digest(body["dataset_identity"])
        ):
            raise ValueError
        objects = cast("Sequence[Mapping[str, object]]", document["objects"])
        _verify_published_objects(repository, objects)
        coverage = tuple(
            LabelCoverage(
                cast("str", item["symbol"]),
                cast("str | None", item["instrument_id"]),
                cast("str", item["resolution_code"]),
                cast("str", item["horizon"]),
                cast("int", item["samples"]),
                cast("int", item["valid"]),
                cast("int", item["missing"]),
                cast("int", item["invalid"]),
                cast("Mapping[str, int]", item["reason_counts"]),
            )
            for item in cast("Sequence[Mapping[str, object]]", document["coverage"])
        )
        leakage_document = cast("Mapping[str, object]", document["leakage"])
        leakage = LeakageReport(
            cast("int", leakage_document["checked_samples"]),
            tuple(cast("Sequence[str]", leakage_document["violations"])),
            cast("str", leakage_document["status"]),
        )
        return PublishedFeatureDataset(
            document["dataset_id"],
            supplied_hash,
            path,
            cast("int", document["sample_count"]),
            cast("int", document["session_summary_count"]),
            len(objects),
            coverage,
            leakage,
        )
    except DatasetBuildError:
        raise
    except (KeyError, OSError, TypeError, ValueError, json.JSONDecodeError) as error:
        raise DatasetBuildError(
            DatasetBuildCode.CORRUPT_CANONICAL_INPUT,
            "published feature dataset manifest is invalid",
        ) from error


def _sample_leakage_reason(sample: FeatureSample, split_plan: SplitPlan) -> str | None:
    if split_plan.session_splits.get(sample.session_id) != sample.split:
        return DatasetBuildCode.NON_CHRONOLOGICAL_SPLIT.value
    if sample.feature_end_exchange_time_ns != sample.as_of_exchange_time_ns:
        return DatasetBuildCode.LABEL_FEATURE_OVERLAP.value
    if sample.max_event_availability_time_ns > sample.knowledge_cutoff_time_ns:
        return DatasetBuildCode.FUTURE_EVENT_REVISION.value
    if any(
        label.validity is LabelValidity.VALID
        and cast("int", label.target_exchange_time_ns)
        <= sample.feature_end_exchange_time_ns
        for label in sample.labels
    ):
        return DatasetBuildCode.LABEL_FEATURE_OVERLAP.value
    return None


def build_and_publish_feature_dataset(
    repository: DataRepository,
    quota: QuotaEvidence,
    builder: FeatureDatasetBuilder,
    source: CanonicalDatasetSource,
    *,
    batch_rows: int = 8_192,
    fault_injector: Callable[[str], None] | None = None,
) -> PublishedFeatureDataset:
    """Build bounded Parquet artifacts and publish their manifest last."""
    observed_sessions = builder.observed_sessions(source)
    split_plan = build_split_plan(observed_sessions)
    universe_sha = builder.universe.universe_snapshot_sha256.hex()
    calendar_version = cast("_HexIdentifier", builder.calendar.calendar_version).hex()
    build_identity = {
        "calendar_id": builder.calendar.calendar_id,
        "calendar_version": calendar_version,
        "dataset_seed": DATASET_SEED,
        "event_snapshot_sha256": builder.events.sha256,
        "feature_names": list(FEATURE_NAMES),
        "horizons": list(REQUIRED_HORIZON_LABELS),
        "schema_version": FEATURE_DATASET_SCHEMA_VERSION,
        "sector_snapshot_sha256": builder.sectors.sha256,
        "source_partition_ids": list(source.source_partition_ids),
        "split_sessions": {
            "first_purge": list(split_plan.first_purge_sessions),
            "second_purge": list(split_plan.second_purge_sessions),
            "test": list(split_plan.test_sessions),
            "train": list(split_plan.train_sessions),
            "validation": list(split_plan.validation_sessions),
        },
        "universe_snapshot_sha256": universe_sha,
    }
    build_key = _digest(build_identity)
    manifest_relative = f"datasets/feature-poc/manifests/{build_key}.json"
    manifest_path = repository.root / manifest_relative
    if manifest_path.exists() or manifest_path.is_symlink():
        if manifest_path.is_symlink():
            raise DatasetBuildError(
                DatasetBuildCode.CORRUPT_CANONICAL_INPUT,
                "feature dataset manifest path is a symlink",
            )
        return _load_published_dataset(repository, manifest_path, build_key)
    output_estimate = source.record_count * DEFAULT_OUTPUT_BYTES_PER_SAMPLE + 10_000_000
    temporary_estimate = min(
        output_estimate,
        builder.maximum_records_per_instrument * DEFAULT_OUTPUT_BYTES_PER_SAMPLE
        + 10_000_000,
    )
    request = StorageRequest(
        f"feature-dataset:{build_key[:32]}",
        output_bytes=output_estimate,
        temporary_bytes=temporary_estimate,
    )
    aggregates = builder.cross_sectional_aggregates(source)
    normalization = builder.fit(source, split_plan, aggregates)
    coverage_accumulator = _CoverageAccumulator(builder.universe)
    objects: list[dict[str, object]] = []
    sample_count = 0
    summary_count = 0
    written_bytes = 0
    leakage_reasons: set[str] = set()
    train_cutoff = 0
    stage_prefix = f"tmp/feature-dataset/{build_key}-{os.getpid()}"
    with repository.acquire_admission(request, quota) as lease:
        try:
            samples = builder.iter_samples(
                source, split_plan, aggregates, normalization=normalization
            )
            for ordinal, ((instrument_id, split), sample_group) in enumerate(
                groupby(samples, key=lambda item: (item.instrument_id, item.split))
            ):

                def sample_rows(
                    row_group: Iterator[FeatureSample] = sample_group,
                ) -> Iterator[Mapping[str, object]]:
                    nonlocal sample_count, train_cutoff
                    for sample in row_group:
                        sample_count += 1
                        coverage_accumulator.add(sample)
                        reason = _sample_leakage_reason(sample, split_plan)
                        if reason is not None:
                            leakage_reasons.add(reason)
                        if sample.split == "TRAIN":
                            train_cutoff = max(
                                train_cutoff, sample.as_of_exchange_time_ns
                            )
                        yield _sample_document(sample)

                staged_relative = f"{stage_prefix}-feature-{ordinal:04d}.parquet.part"
                staged = _write_parquet(
                    repository.root / staged_relative,
                    _FEATURE_ARROW_SCHEMA,
                    sample_rows(),
                    batch_rows=batch_rows,
                )
                written_bytes += staged.size_bytes
                final_relative = (
                    f"datasets/feature-poc/split={split.lower()}/"
                    f"instrument={instrument_id}/part-{staged.sha256}.parquet"
                )
                final_path = repository.publish_staged_object(
                    staged_relative,
                    final_relative,
                    expected_sha256=staged.sha256,
                    expected_size_bytes=staged.size_bytes,
                    lease=lease,
                )
                objects.append(
                    {
                        "instrument_id": instrument_id,
                        "kind": "FEATURE_LABEL",
                        "object_sha256": staged.sha256,
                        "record_count": staged.record_count,
                        "size_bytes_decimal": staged.size_bytes,
                        "split": split,
                        "storage_path": str(final_path.relative_to(repository.root)),
                    }
                )
                if fault_injector is not None:
                    fault_injector("FEATURE_PARTITION_PUBLISHED")
            summaries = builder.session_summaries(source)
            for ordinal, (instrument_id, summary_group) in enumerate(
                groupby(summaries, key=lambda item: item.instrument_id)
            ):

                def summary_rows(
                    row_group: Iterator[SessionSummary] = summary_group,
                ) -> Iterator[Mapping[str, object]]:
                    nonlocal summary_count
                    for summary in row_group:
                        summary_count += 1
                        yield _summary_document(summary)

                staged_relative = f"{stage_prefix}-summary-{ordinal:04d}.parquet.part"
                staged = _write_parquet(
                    repository.root / staged_relative,
                    _SESSION_ARROW_SCHEMA,
                    summary_rows(),
                    batch_rows=batch_rows,
                )
                written_bytes += staged.size_bytes
                final_relative = (
                    f"derived/feature-poc/session-summary/instrument={instrument_id}/"
                    f"part-{staged.sha256}.parquet"
                )
                final_path = repository.publish_staged_object(
                    staged_relative,
                    final_relative,
                    expected_sha256=staged.sha256,
                    expected_size_bytes=staged.size_bytes,
                    lease=lease,
                )
                objects.append(
                    {
                        "instrument_id": instrument_id,
                        "kind": "SESSION_SUMMARY",
                        "object_sha256": staged.sha256,
                        "record_count": staged.record_count,
                        "size_bytes_decimal": staged.size_bytes,
                        "split": None,
                        "storage_path": str(final_path.relative_to(repository.root)),
                    }
                )
            if written_bytes > output_estimate:
                raise DatasetBuildError(
                    DatasetBuildCode.STORAGE_LIMIT,
                    "dataset objects exceeded admitted output bytes",
                )
            if (
                len(split_plan.first_purge_sessions) != PURGE_SESSIONS
                or len(split_plan.second_purge_sessions) != PURGE_SESSIONS
            ):
                leakage_reasons.add(DatasetBuildCode.PURGE_EMBARGO_VIOLATION.value)
            if any(
                stat.maximum_fit_exchange_time_ns > train_cutoff
                for stat in normalization
            ):
                leakage_reasons.add(DatasetBuildCode.NORMALIZATION_LEAKAGE.value)
            leakage = LeakageReport(
                sample_count,
                tuple(sorted(leakage_reasons)),
                "PASS" if not leakage_reasons else "REJECTED",
            )
            if leakage.status != "PASS":
                raise DatasetBuildError(
                    DatasetBuildCode.LABEL_FEATURE_OVERLAP,
                    "leakage validation rejected dataset publication",
                )
            coverage = coverage_accumulator.freeze()
            dataset_identity = {
                **build_identity,
                "normalization": [
                    _normalization_document(stat) for stat in normalization
                ],
                "objects": objects,
            }
            dataset_id = _digest(dataset_identity)
            body: dict[str, object] = {
                "build_key": build_key,
                "coverage": [_coverage_document(item) for item in coverage],
                "dataset_id": dataset_id,
                "dataset_identity": dataset_identity,
                "derived_daily_bars_downloaded": False,
                "economic_value_claimed": False,
                "label_unit": "RETURN_PPM_AND_INTEGER_PRICE_TICKS",
                "leakage": {
                    "checked_samples": leakage.checked_samples,
                    "schema_version": leakage.schema_version,
                    "status": leakage.status,
                    "violations": list(leakage.violations),
                },
                "live_trading_capable": False,
                "objects": objects,
                "sample_count": sample_count,
                "schema_version": FEATURE_DATASET_SCHEMA_VERSION,
                "session_summary_count": summary_count,
                "storage_admission": {
                    "admitted_output_bytes_decimal": output_estimate,
                    "actual_output_bytes_decimal": written_bytes,
                    "hard_data_root_limit_bytes_decimal": 100_000_000_000,
                },
            }
            manifest_sha256 = _digest(body)
            document = {**body, "manifest_sha256": manifest_sha256}
            encoded = _canonical_bytes(document) + b"\n"
            staged_relative = f"{stage_prefix}-manifest.json.part"
            _write_bytes(repository.root / staged_relative, encoded)
            published_manifest = repository.publish_staged_object(
                staged_relative,
                manifest_relative,
                expected_sha256=hashlib.sha256(encoded).hexdigest(),
                expected_size_bytes=len(encoded),
                lease=lease,
            )
            if fault_injector is not None:
                fault_injector("DATASET_MANIFEST_PUBLISHED")
            return PublishedFeatureDataset(
                dataset_id,
                manifest_sha256,
                published_manifest,
                sample_count,
                summary_count,
                len(objects),
                coverage,
                leakage,
            )
        finally:
            for path in repository.root.glob(f"{stage_prefix}*"):
                if path.is_file() and not path.is_symlink():
                    path.unlink(missing_ok=True)
