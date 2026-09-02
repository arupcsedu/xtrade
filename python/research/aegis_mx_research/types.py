"""Immutable point-in-time data and dataset contracts."""

from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
from typing import Final

POINT_IN_TIME_SCHEMA_VERSION: Final = "1.0.0"
MAX_TEXT_BYTES: Final = 160
SHA256_BYTES: Final = 32


class SourceKind(IntEnum):
    """Provider-neutral source trust category."""

    OFFICIAL_PRIMARY = 1
    LICENSED_PROVIDER = 2
    PUBLIC_SECONDARY = 3
    SYNTHETIC_REPLAY = 4


class RecordKind(IntEnum):
    """Supported immutable point-in-time record families."""

    CORPORATE_ACTION = 1
    SYMBOL_MAPPING = 2
    DELISTING = 3
    INDEX_MEMBERSHIP = 4
    ANALYST_ESTIMATE = 5
    MACRO_VINTAGE = 6
    NEWS_REVISION = 7
    FILING_REVISION = 8


class CorporateActionType(IntEnum):
    """Provider-neutral corporate-action categories."""

    SPLIT = 1
    CASH_DIVIDEND = 2
    MERGER = 3
    SPINOFF = 4


class DatasetSplit(IntEnum):
    """Chronologically ordered model-evaluation partitions."""

    TRAIN = 1
    VALIDATION = 2
    TEST = 3


class SplitMethod(IntEnum):
    """Dataset split construction policy."""

    WALK_FORWARD = 1
    BLOCKED_TIME = 2
    RANDOMIZED = 3


def _bounded_text(value: str, name: str, maximum: int = MAX_TEXT_BYTES) -> None:
    if not value or "\x00" in value or len(value.encode("utf-8")) > maximum:
        msg = f"{name} is empty, contains NUL, or exceeds {maximum} UTF-8 bytes"
        raise ValueError(msg)


def _positive(value: int, name: str) -> None:
    if value <= 0:
        msg = f"{name} must be positive"
        raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class DataSource:
    """Exact source and content identity without retaining licensed payloads."""

    source_id: str
    kind: SourceKind
    provider: str
    document_id: str
    content_sha256: bytes
    authenticated: bool

    def __post_init__(self) -> None:
        """Validate bounded provenance and a nonzero content digest."""
        _bounded_text(self.source_id, "source_id")
        _bounded_text(self.provider, "provider")
        _bounded_text(self.document_id, "document_id")
        if len(self.content_sha256) != SHA256_BYTES or not any(self.content_sha256):
            msg = "content_sha256 must be a nonzero SHA-256 digest"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class ValidityInterval:
    """Half-open business-effective interval in wall-clock UTC nanoseconds."""

    valid_from_ns: int
    valid_to_ns: int | None

    def __post_init__(self) -> None:
        """Require a positive nonempty half-open interval."""
        _positive(self.valid_from_ns, "valid_from_ns")
        if self.valid_to_ns is not None and self.valid_to_ns <= self.valid_from_ns:
            msg = "valid_to_ns must be greater than valid_from_ns"
            raise ValueError(msg)

    def contains(self, timestamp_ns: int) -> bool:
        """Return whether the timestamp is inside the half-open interval."""
        return self.valid_from_ns <= timestamp_ns and (
            self.valid_to_ns is None or timestamp_ns < self.valid_to_ns
        )


@dataclass(frozen=True, slots=True)
class CorporateAction:
    """One effective corporate action; ratios remain integer fractions."""

    action_id: str
    instrument_id: str
    action_type: CorporateActionType
    ratio_numerator: int
    ratio_denominator: int

    def __post_init__(self) -> None:
        """Validate identity and a positive exact ratio."""
        _bounded_text(self.action_id, "action_id")
        _bounded_text(self.instrument_id, "instrument_id")
        _positive(self.ratio_numerator, "ratio_numerator")
        _positive(self.ratio_denominator, "ratio_denominator")


@dataclass(frozen=True, slots=True)
class SymbolMapping:
    """A symbol-to-stable-instrument mapping for one validity interval."""

    symbol: str
    instrument_id: str
    venue: str

    def __post_init__(self) -> None:
        """Validate bounded symbology fields."""
        _bounded_text(self.symbol, "symbol", 32)
        _bounded_text(self.instrument_id, "instrument_id")
        _bounded_text(self.venue, "venue", 32)


@dataclass(frozen=True, slots=True)
class Delisting:
    """An effective delisting retained for survivorship-bias checks."""

    instrument_id: str
    reason: str

    def __post_init__(self) -> None:
        """Validate stable instrument and bounded reason text."""
        _bounded_text(self.instrument_id, "instrument_id")
        _bounded_text(self.reason, "reason")


@dataclass(frozen=True, slots=True)
class IndexMembership:
    """Historical inclusion state for one instrument and index."""

    index_id: str
    instrument_id: str
    included: bool

    def __post_init__(self) -> None:
        """Validate stable index and instrument identities."""
        _bounded_text(self.index_id, "index_id")
        _bounded_text(self.instrument_id, "instrument_id")


@dataclass(frozen=True, slots=True)
class AnalystEstimateVintage:
    """One analyst-estimate vintage with exact metric and units."""

    instrument_id: str
    metric_id: str
    fiscal_period: str
    value: int
    unit: str

    def __post_init__(self) -> None:
        """Validate comparability identifiers and explicit units."""
        _bounded_text(self.instrument_id, "instrument_id")
        _bounded_text(self.metric_id, "metric_id", 64)
        _bounded_text(self.fiscal_period, "fiscal_period", 32)
        _bounded_text(self.unit, "unit", 32)


@dataclass(frozen=True, slots=True)
class MacroVintage:
    """One release or revision for an exact macro series and period."""

    series_id: str
    reference_period: str
    value: int
    unit: str

    def __post_init__(self) -> None:
        """Validate macro identity and explicit unit."""
        _bounded_text(self.series_id, "series_id", 64)
        _bounded_text(self.reference_period, "reference_period", 32)
        _bounded_text(self.unit, "unit", 32)


@dataclass(frozen=True, slots=True)
class NewsRevision:
    """A news publication or correction linked to the prior revision."""

    document_id: str
    correction_of_record_id: str | None

    def __post_init__(self) -> None:
        """Validate document and optional correction identity."""
        _bounded_text(self.document_id, "document_id")
        if self.correction_of_record_id is not None:
            _bounded_text(self.correction_of_record_id, "correction_of_record_id")


@dataclass(frozen=True, slots=True)
class FilingRevision:
    """A filing or amendment linked to the prior accepted revision."""

    accession_id: str
    form_type: str
    amendment_of_record_id: str | None

    def __post_init__(self) -> None:
        """Validate filing identity, form, and optional amendment link."""
        _bounded_text(self.accession_id, "accession_id")
        _bounded_text(self.form_type, "form_type", 32)
        if self.amendment_of_record_id is not None:
            _bounded_text(self.amendment_of_record_id, "amendment_of_record_id")


type RecordPayload = (
    CorporateAction
    | SymbolMapping
    | Delisting
    | IndexMembership
    | AnalystEstimateVintage
    | MacroVintage
    | NewsRevision
    | FilingRevision
)


def _payload_kind(payload: RecordPayload) -> RecordKind:
    kind: RecordKind
    if isinstance(payload, CorporateAction):
        kind = RecordKind.CORPORATE_ACTION
    elif isinstance(payload, SymbolMapping):
        kind = RecordKind.SYMBOL_MAPPING
    elif isinstance(payload, Delisting):
        kind = RecordKind.DELISTING
    elif isinstance(payload, IndexMembership):
        kind = RecordKind.INDEX_MEMBERSHIP
    elif isinstance(payload, AnalystEstimateVintage):
        kind = RecordKind.ANALYST_ESTIMATE
    elif isinstance(payload, MacroVintage):
        kind = RecordKind.MACRO_VINTAGE
    elif isinstance(payload, NewsRevision):
        kind = RecordKind.NEWS_REVISION
    else:
        kind = RecordKind.FILING_REVISION
    return kind


def _payload_logical_key(payload: RecordPayload) -> str:
    key: str
    if isinstance(payload, CorporateAction):
        key = payload.action_id
    elif isinstance(payload, SymbolMapping):
        key = payload.symbol
    elif isinstance(payload, Delisting):
        key = payload.instrument_id
    elif isinstance(payload, IndexMembership):
        key = f"{payload.index_id}:{payload.instrument_id}"
    elif isinstance(payload, AnalystEstimateVintage):
        key = f"{payload.instrument_id}:{payload.metric_id}:{payload.fiscal_period}"
    elif isinstance(payload, MacroVintage):
        key = f"{payload.series_id}:{payload.reference_period}"
    elif isinstance(payload, NewsRevision):
        key = payload.document_id
    else:
        key = payload.accession_id
    return key


@dataclass(frozen=True, slots=True)
class PointInTimeRecord:
    """Immutable bitemporal record with complete availability provenance."""

    record_id: str
    logical_key: str
    kind: RecordKind
    event_time_ns: int
    publication_time_ns: int
    receive_time_ns: int
    processing_time_ns: int
    revision_time_ns: int
    validity: ValidityInterval
    source: DataSource
    version: int
    payload: RecordPayload
    schema_version: str = POINT_IN_TIME_SCHEMA_VERSION

    def __post_init__(self) -> None:
        """Validate timestamp semantics, payload identity, and schema version."""
        _bounded_text(self.record_id, "record_id")
        _bounded_text(self.logical_key, "logical_key")
        for name, value in (
            ("event_time_ns", self.event_time_ns),
            ("publication_time_ns", self.publication_time_ns),
            ("receive_time_ns", self.receive_time_ns),
            ("processing_time_ns", self.processing_time_ns),
            ("revision_time_ns", self.revision_time_ns),
        ):
            _positive(value, name)
        if self.receive_time_ns < self.publication_time_ns:
            msg = "receive_time_ns cannot precede publication_time_ns"
            raise ValueError(msg)
        if self.processing_time_ns < self.receive_time_ns:
            msg = "processing_time_ns cannot precede receive_time_ns"
            raise ValueError(msg)
        if self.revision_time_ns < self.publication_time_ns:
            msg = "revision_time_ns cannot precede publication_time_ns"
            raise ValueError(msg)
        if not self.validity.contains(self.event_time_ns):
            msg = "event_time_ns must be inside the business validity interval"
            raise ValueError(msg)
        if self.version <= 0:
            msg = "version must be positive"
            raise ValueError(msg)
        if self.schema_version != POINT_IN_TIME_SCHEMA_VERSION:
            msg = "unsupported point-in-time schema_version"
            raise ValueError(msg)
        if self.kind is not _payload_kind(self.payload):
            msg = "record kind does not match payload type"
            raise ValueError(msg)
        if self.logical_key != _payload_logical_key(self.payload):
            msg = "logical_key does not match the canonical payload key"
            raise ValueError(msg)

    @property
    def available_at_ns(self) -> int:
        """Return when both revision and local processing were known."""
        return max(self.processing_time_ns, self.revision_time_ns)


@dataclass(frozen=True, slots=True)
class DatasetSample:
    """Auditable feature/label window and point-in-time source selection."""

    sample_id: str
    event_time_ns: int
    feature_start_ns: int
    feature_end_ns: int
    label_start_ns: int
    label_end_ns: int
    split: DatasetSplit
    record_ids: tuple[str, ...]
    universe_instrument_ids: tuple[str, ...]

    def __post_init__(self) -> None:
        """Validate ranges and deterministic provenance ordering."""
        _bounded_text(self.sample_id, "sample_id")
        for name, value in (
            ("event_time_ns", self.event_time_ns),
            ("feature_start_ns", self.feature_start_ns),
            ("feature_end_ns", self.feature_end_ns),
            ("label_start_ns", self.label_start_ns),
            ("label_end_ns", self.label_end_ns),
        ):
            _positive(value, name)
        if self.feature_start_ns > self.feature_end_ns:
            msg = "feature interval is reversed"
            raise ValueError(msg)
        if self.label_start_ns > self.label_end_ns:
            msg = "label interval is reversed"
            raise ValueError(msg)
        if not self.record_ids or len(set(self.record_ids)) != len(self.record_ids):
            msg = "record_ids must be nonempty and unique"
            raise ValueError(msg)
        if len(set(self.universe_instrument_ids)) != len(self.universe_instrument_ids):
            msg = "universe_instrument_ids must be unique"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class DatasetManifest:
    """Point-in-time dataset construction and split provenance."""

    dataset_id: str
    created_at_ns: int
    split_method: SplitMethod
    samples: tuple[DatasetSample, ...]
    universe_index_id: str
    embargo_ns: int = 0
    random_seed: int | None = None

    def __post_init__(self) -> None:
        """Validate deterministic manifest identity and sample uniqueness."""
        _bounded_text(self.dataset_id, "dataset_id")
        _positive(self.created_at_ns, "created_at_ns")
        _bounded_text(self.universe_index_id, "universe_index_id")
        if not self.samples:
            msg = "dataset manifest requires samples"
            raise ValueError(msg)
        if len({sample.sample_id for sample in self.samples}) != len(self.samples):
            msg = "dataset sample identifiers must be unique"
            raise ValueError(msg)
        if self.embargo_ns < 0:
            msg = "embargo_ns cannot be negative"
            raise ValueError(msg)
        if self.split_method is SplitMethod.RANDOMIZED and self.random_seed is None:
            msg = "randomized split metadata must retain its random seed"
            raise ValueError(msg)
