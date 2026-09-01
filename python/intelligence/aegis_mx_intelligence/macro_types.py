"""Versioned integer-only contracts for macroeconomic event processing."""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass, fields, is_dataclass
from enum import IntEnum
from typing import TYPE_CHECKING, Final, cast

from aegis_mx_intelligence.contracts import (
    ConfigurationVersion,
    FeatureSnapshotId,
    ForecastId,
    GlobalEventId,
    Identifier128,
    InstrumentId,
    ModelId,
    ModelVersion,
    SessionId,
)

if TYPE_CHECKING:
    from aegis_mx_intelligence.news_types import EvidenceExcerpt

MACRO_SCHEMA_VERSION: Final = "1.0.0"
PPM: Final = 1_000_000
SHA256_BYTES: Final = 32
MAX_TEXT_BYTES: Final = 128
MAX_SOURCES: Final = 48
MAX_FIELDS: Final = 64
MAX_OBSERVATIONS: Final = 128
MAX_PRIOR_VALUES: Final = 64
MAX_MARKET_OBSERVATIONS: Final = 64
MAX_HISTORY: Final = 256
MAX_EVIDENCE_REFERENCES: Final = 16
MAX_ABSOLUTE_RESPONSE_PPM: Final = 10_000_000
MAX_INT64: Final = (1 << 63) - 1
MIN_INT64: Final = -(1 << 63)


class MacroEventType(IntEnum):
    """Initial scheduled macroeconomic release families."""

    CPI = 1
    PPI = 2
    EMPLOYMENT_REPORT = 3
    GDP = 4
    RETAIL_SALES = 5
    FOMC_DECISION = 6
    FEDERAL_RESERVE_STATEMENT = 7
    TREASURY_AUCTION = 8
    PMI = 9


class MacroMetricRole(IntEnum):
    """Semantic role of a field within a release."""

    HEADLINE = 1
    CORE = 2
    SUBCOMPONENT = 3


class MacroMetricUnit(IntEnum):
    """Explicit integer units used by macro fields."""

    PERCENT_PPM = 1
    RATE_BASIS_POINTS = 2
    INDEX_MILLI = 3
    CURRENCY_MILLIONS = 4
    QUANTITY_THOUSANDS = 5
    RATIO_PPM = 6


class SeasonalAdjustment(IntEnum):
    """Seasonal-adjustment convention included in field identity."""

    SEASONALLY_ADJUSTED = 1
    NOT_SEASONALLY_ADJUSTED = 2
    NOT_APPLICABLE = 3


class Annualization(IntEnum):
    """Annualization convention included in field identity."""

    NOT_ANNUALIZED = 1
    ANNUALIZED = 2
    NOT_APPLICABLE = 3


class ReleaseCompletenessState(IntEnum):
    """Publisher-declared completeness of an actual release revision."""

    PARTIAL = 1
    COMPLETE = 2


class MacroEventPhase(IntEnum):
    """Observable point-in-time state of one calendar event."""

    SCHEDULED = 1
    DELAYED = 2
    PARTIAL_RELEASE = 3
    RELEASE_COMPLETE = 4
    CORRECTED = 5


class MacroDecision(IntEnum):
    """Whether the specialist produced a usable common forecast."""

    ABSTAIN = 1
    PUBLISH = 2


class MacroSourceKind(IntEnum):
    """Provenance category for normalized macro inputs."""

    OFFICIAL_CALENDAR = 1
    CONSENSUS_SNAPSHOT = 2
    PRIOR_RELEASE = 3
    OFFICIAL_RELEASE = 4
    DATA_PROVIDER = 5
    MARKET_FEATURES = 6
    HISTORICAL_SURPRISE = 7


class MacroSourceQuality(IntEnum):
    """Explicit source-quality class; ordering is not used as a score."""

    UNVERIFIED = 1
    PUBLIC_SECONDARY = 2
    LICENSED_AGGREGATOR = 3
    OFFICIAL_PRIMARY = 4
    SYNTHETIC_REPLAY = 5


class MacroReasonCode(IntEnum):
    """Machine-readable abstention and informational reasons."""

    NO_RELEASE = 1
    DELAYED_RELEASE = 2
    PARTIAL_RELEASE = 3
    MISSING_REQUIRED_FIELD = 4
    MISSING_CONSENSUS = 5
    INCOMPATIBLE_FIELD = 6
    CONFLICTING_PROVIDER_VALUES = 7
    LOW_SOURCE_QUALITY = 8
    UNAUTHENTICATED_SOURCE = 9
    MISSING_CROSS_ASSET_GROUP = 10
    INSUFFICIENT_HISTORY = 11
    CORRECTED_RELEASE = 12
    NO_COMPARABLE_SURPRISE = 13
    INVALID_MARKET_FEATURES = 14
    FIELD_NOT_RELEASED = 15


class CrossAssetKind(IntEnum):
    """Cross-asset observations supported by the first macro specialist."""

    EQUITY_INDEX_FUTURE = 1
    TREASURY_FUTURE = 2
    TREASURY_YIELD = 3
    FX_PROXY = 4
    VOLATILITY_INSTRUMENT = 5
    SECTOR_ETF = 6


class CrossAssetGroup(IntEnum):
    """Completeness groups; either Treasury representation satisfies rates."""

    EQUITY_INDEX = 1
    RATES = 2
    FX = 3
    VOLATILITY = 4
    SECTOR = 5


class CrossAssetUnit(IntEnum):
    """Explicit units for a release-response observation."""

    RETURN_PPM = 1
    YIELD_CHANGE_BASIS_POINTS = 2


def _bounded_text(value: str, name: str, maximum: int = MAX_TEXT_BYTES) -> None:
    if not value or "\x00" in value or len(value.encode("utf-8")) > maximum:
        msg = f"{name} is empty, contains NUL, or exceeds {maximum} UTF-8 bytes"
        raise ValueError(msg)


def _valid_digest(value: bytes, name: str) -> None:
    if len(value) != SHA256_BYTES or not any(value):
        msg = f"{name} must be a nonzero SHA-256 digest"
        raise ValueError(msg)


def _int64(value: int, name: str) -> None:
    if not MIN_INT64 <= value <= MAX_INT64:
        msg = f"{name} is outside the signed 64-bit range"
        raise ValueError(msg)


def _positive_timestamp(value: int, name: str) -> None:
    if value <= 0:
        msg = f"{name} must be positive"
        raise ValueError(msg)


@dataclass(frozen=True, slots=True, order=True)
class MacroMetricIdentity:
    """Exact comparability key for actual, consensus, and prior values."""

    event_type: MacroEventType
    field_id: str
    role: MacroMetricRole
    unit: MacroMetricUnit
    reference_period: str
    seasonal_adjustment: SeasonalAdjustment
    annualization: Annualization

    def __post_init__(self) -> None:
        """Validate bounded provider-independent field and period identifiers."""
        _bounded_text(self.field_id, "field_id", 64)
        _bounded_text(self.reference_period, "reference_period", 32)


@dataclass(frozen=True, slots=True, order=True)
class MacroEvidenceReference:
    """Reference to an exact excerpt in one normalized source."""

    source_id: str
    excerpt_id: int

    def __post_init__(self) -> None:
        """Validate bounded source identity and positive excerpt identity."""
        _bounded_text(self.source_id, "evidence source_id", 64)
        if self.excerpt_id <= 0:
            msg = "evidence excerpt_id must be positive"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class MacroSourceEvidence:
    """Authenticated or replay source retained with exact sanitized excerpts."""

    source_id: str
    source_global_event_id: GlobalEventId
    kind: MacroSourceKind
    quality: MacroSourceQuality
    authenticated: bool
    provider_id: str
    document_id: str
    source_content_sha256: bytes
    sanitized_content_sha256: bytes
    publication_wall_clock_utc_ns: int
    received_wall_clock_utc_ns: int
    excerpts: tuple[EvidenceExcerpt, ...]

    def __post_init__(self) -> None:
        """Validate source identity, timestamps, digests, and excerpt bounds."""
        _bounded_text(self.source_id, "source_id", 64)
        _bounded_text(self.provider_id, "provider_id", 64)
        _bounded_text(self.document_id, "document_id", 128)
        _valid_digest(self.source_content_sha256, "source_content_sha256")
        _valid_digest(self.sanitized_content_sha256, "sanitized_content_sha256")
        _positive_timestamp(
            self.publication_wall_clock_utc_ns,
            "publication_wall_clock_utc_ns",
        )
        if self.received_wall_clock_utc_ns < self.publication_wall_clock_utc_ns:
            msg = "macro source receipt precedes publication"
            raise ValueError(msg)
        if not self.excerpts or len(self.excerpts) > MAX_EVIDENCE_REFERENCES:
            msg = "macro source requires one to sixteen exact excerpts"
            raise ValueError(msg)
        identifiers = {item.excerpt_id for item in self.excerpts}
        if len(identifiers) != len(self.excerpts):
            msg = "macro source excerpt identifiers must be unique"
            raise ValueError(msg)


def _validate_reference_count(
    references: tuple[MacroEvidenceReference, ...], owner: str
) -> None:
    if not references or len(references) > MAX_EVIDENCE_REFERENCES:
        msg = f"{owner} requires one to sixteen evidence references"
        raise ValueError(msg)
    if len(set(references)) != len(references):
        msg = f"{owner} evidence references must be unique"
        raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class ExpectedReleaseField:
    """One versioned calendar field and its target-direction convention."""

    identity: MacroMetricIdentity
    required: bool
    positive_surprise_direction: int
    forecast_weight_ppm: int
    evidence: tuple[MacroEvidenceReference, ...]

    def __post_init__(self) -> None:
        """Validate direction, bounded weight, and calendar evidence."""
        if self.positive_surprise_direction not in {-1, 0, 1}:
            msg = "positive_surprise_direction must be -1, 0, or 1"
            raise ValueError(msg)
        if not 0 <= self.forecast_weight_ppm <= PPM:
            msg = "forecast_weight_ppm is outside [0, 1,000,000]"
            raise ValueError(msg)
        _validate_reference_count(self.evidence, "expected release field")


@dataclass(frozen=True, slots=True)
class MacroCalendarEvent:
    """Immutable scheduled event definition known before release."""

    calendar_event_id: GlobalEventId
    event_type: MacroEventType
    calendar_version: str
    scheduled_release_wall_clock_utc_ns: int
    expected_fields: tuple[ExpectedReleaseField, ...]

    def __post_init__(self) -> None:
        """Validate schedule, field identity, cardinality, and role invariants."""
        _bounded_text(self.calendar_version, "calendar_version", 32)
        _positive_timestamp(
            self.scheduled_release_wall_clock_utc_ns,
            "scheduled_release_wall_clock_utc_ns",
        )
        if not self.expected_fields or len(self.expected_fields) > MAX_FIELDS:
            msg = "macro calendar requires one to sixty-four expected fields"
            raise ValueError(msg)
        identities = {item.identity for item in self.expected_fields}
        if len(identities) != len(self.expected_fields):
            msg = "macro calendar expected field identities must be unique"
            raise ValueError(msg)
        if any(
            item.identity.event_type is not self.event_type
            for item in self.expected_fields
        ):
            msg = "macro calendar field event type mismatch"
            raise ValueError(msg)
        headline_count = sum(
            item.identity.role is MacroMetricRole.HEADLINE
            for item in self.expected_fields
        )
        core_count = sum(
            item.identity.role is MacroMetricRole.CORE for item in self.expected_fields
        )
        if headline_count != 1 or core_count > 1:
            msg = "macro calendar requires one headline and at most one core field"
            raise ValueError(msg)
        if not any(
            item.required and item.identity.role is MacroMetricRole.HEADLINE
            for item in self.expected_fields
        ):
            msg = "macro calendar headline field must be required"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class MacroConsensusEstimate:
    """Point-in-time consensus distribution for one exact field identity."""

    identity: MacroMetricIdentity
    consensus_value: int
    dispersion: int
    minimum_value: int
    maximum_value: int
    contributor_count: int
    available_wall_clock_utc_ns: int
    evidence: tuple[MacroEvidenceReference, ...]

    def __post_init__(self) -> None:
        """Validate range, dispersion, contributor count, time, and evidence."""
        for name, value in (
            ("consensus_value", self.consensus_value),
            ("minimum_value", self.minimum_value),
            ("maximum_value", self.maximum_value),
        ):
            _int64(value, name)
        if (
            self.dispersion < 0
            or self.contributor_count <= 0
            or not self.minimum_value <= self.consensus_value <= self.maximum_value
        ):
            msg = "macro consensus distribution or contributor count is invalid"
            raise ValueError(msg)
        _positive_timestamp(
            self.available_wall_clock_utc_ns,
            "consensus available_wall_clock_utc_ns",
        )
        _validate_reference_count(self.evidence, "macro consensus")


@dataclass(frozen=True, slots=True)
class MacroConsensusSnapshot:
    """Frozen pre-release consensus; correction processing reuses its hash."""

    snapshot_id: GlobalEventId
    calendar_event_id: GlobalEventId
    frozen_wall_clock_utc_ns: int
    estimates: tuple[MacroConsensusEstimate, ...]

    def __post_init__(self) -> None:
        """Validate unique estimates and their availability at freeze time."""
        _positive_timestamp(
            self.frozen_wall_clock_utc_ns,
            "consensus frozen_wall_clock_utc_ns",
        )
        if len(self.estimates) > MAX_FIELDS:
            msg = "macro consensus estimate count exceeds its bound"
            raise ValueError(msg)
        identities = {item.identity for item in self.estimates}
        if len(identities) != len(self.estimates):
            msg = "macro consensus field identities must be unique"
            raise ValueError(msg)
        if any(
            item.available_wall_clock_utc_ns > self.frozen_wall_clock_utc_ns
            for item in self.estimates
        ):
            msg = "macro consensus contains an estimate unavailable at freeze time"
            raise ValueError(msg)

    def canonical_bytes(self) -> bytes:
        """Return deterministic snapshot bytes for freeze enforcement."""
        return _canonical_json_bytes(self)

    @property
    def sha256(self) -> bytes:
        """Return the immutable consensus snapshot digest."""
        return hashlib.sha256(self.canonical_bytes()).digest()


@dataclass(frozen=True, slots=True)
class MacroValueObservation:
    """One official or provider-observed value from an actual release."""

    identity: MacroMetricIdentity
    value: int
    source_id: str
    evidence: tuple[MacroEvidenceReference, ...]

    def __post_init__(self) -> None:
        """Validate the value, source identity, and exact evidence references."""
        _int64(self.value, "macro observed value")
        _bounded_text(self.source_id, "macro observation source_id", 64)
        _validate_reference_count(self.evidence, "macro value observation")


@dataclass(frozen=True, slots=True)
class MacroPriorValue:
    """Prior value as known before release plus a separately retained revision."""

    identity: MacroMetricIdentity
    value_known_before_release: int
    known_available_wall_clock_utc_ns: int
    revised_value_in_release: int | None
    evidence: tuple[MacroEvidenceReference, ...]

    def __post_init__(self) -> None:
        """Validate prior/revised values, known time, and evidence."""
        _int64(self.value_known_before_release, "prior value known before release")
        if self.revised_value_in_release is not None:
            _int64(self.revised_value_in_release, "revised value in release")
        _positive_timestamp(
            self.known_available_wall_clock_utc_ns,
            "prior known_available_wall_clock_utc_ns",
        )
        _validate_reference_count(self.evidence, "macro prior value")


@dataclass(frozen=True, slots=True)
class MacroActualRelease:
    """One append-only official release revision and provider observations."""

    revision: int
    completeness: ReleaseCompletenessState
    official_publication_wall_clock_utc_ns: int
    receipt_wall_clock_utc_ns: int
    observations: tuple[MacroValueObservation, ...]
    prior_values: tuple[MacroPriorValue, ...]
    correction_of_release_sha256: bytes | None = None

    def __post_init__(self) -> None:
        """Validate revision lineage, timestamps, and bounded collections."""
        if self.revision <= 0:
            msg = "macro release revision must be positive"
            raise ValueError(msg)
        if (self.revision == 1) != (self.correction_of_release_sha256 is None):
            msg = "only corrected macro revisions require a parent release digest"
            raise ValueError(msg)
        if self.correction_of_release_sha256 is not None:
            _valid_digest(
                self.correction_of_release_sha256,
                "correction_of_release_sha256",
            )
        _positive_timestamp(
            self.official_publication_wall_clock_utc_ns,
            "official_publication_wall_clock_utc_ns",
        )
        if self.receipt_wall_clock_utc_ns < (
            self.official_publication_wall_clock_utc_ns
        ):
            msg = "macro release receipt precedes its official publication"
            raise ValueError(msg)
        if len(self.observations) > MAX_OBSERVATIONS:
            msg = "macro release observation count exceeds its bound"
            raise ValueError(msg)
        if len(self.prior_values) > MAX_PRIOR_VALUES:
            msg = "macro release prior-value count exceeds its bound"
            raise ValueError(msg)
        prior_identities = {item.identity for item in self.prior_values}
        if len(prior_identities) != len(self.prior_values):
            msg = "macro prior-value identities must be unique"
            raise ValueError(msg)
        source_fields = {(item.source_id, item.identity) for item in self.observations}
        if len(source_fields) != len(self.observations):
            msg = "a macro source cannot publish duplicate values for one identity"
            raise ValueError(msg)

    def canonical_bytes(self) -> bytes:
        """Return deterministic release-revision bytes."""
        return _canonical_json_bytes(self)

    @property
    def sha256(self) -> bytes:
        """Return the immutable release revision digest."""
        return hashlib.sha256(self.canonical_bytes()).digest()


@dataclass(frozen=True, slots=True)
class CrossAssetObservation:
    """One bounded market response around the actual receipt time."""

    instrument_id: InstrumentId
    kind: CrossAssetKind
    unit: CrossAssetUnit
    response_value: int
    window_start_wall_clock_utc_ns: int
    window_end_wall_clock_utc_ns: int

    def __post_init__(self) -> None:
        """Validate unit compatibility, value, and ordered wall-clock window."""
        _int64(self.response_value, "cross-asset response_value")
        _positive_timestamp(
            self.window_start_wall_clock_utc_ns,
            "response window_start_wall_clock_utc_ns",
        )
        if self.window_end_wall_clock_utc_ns <= self.window_start_wall_clock_utc_ns:
            msg = "cross-asset response window is not strictly ordered"
            raise ValueError(msg)
        if (self.kind is CrossAssetKind.TREASURY_YIELD) != (
            self.unit is CrossAssetUnit.YIELD_CHANGE_BASIS_POINTS
        ):
            msg = "only Treasury yields use yield-change basis points"
            raise ValueError(msg)
        if (
            self.unit is CrossAssetUnit.RETURN_PPM
            and abs(self.response_value) > MAX_ABSOLUTE_RESPONSE_PPM
        ):
            msg = "cross-asset return exceeds its PPM bound"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class MacroMarketSnapshot:
    """Immutable cross-asset snapshot with common forecast provenance."""

    feature_snapshot_id: FeatureSnapshotId
    source_id: str
    as_of_exchange_event_time_ns: int
    available_wall_clock_utc_ns: int
    observations: tuple[CrossAssetObservation, ...]
    valid: bool = True

    def __post_init__(self) -> None:
        """Validate source, timestamps, cardinality, and observation uniqueness."""
        _bounded_text(self.source_id, "market snapshot source_id", 64)
        _positive_timestamp(
            self.as_of_exchange_event_time_ns,
            "market as_of_exchange_event_time_ns",
        )
        _positive_timestamp(
            self.available_wall_clock_utc_ns,
            "market available_wall_clock_utc_ns",
        )
        if not self.observations or len(self.observations) > MAX_MARKET_OBSERVATIONS:
            msg = "macro market snapshot requires one to sixty-four observations"
            raise ValueError(msg)
        identities = {(item.instrument_id, item.kind) for item in self.observations}
        if len(identities) != len(self.observations):
            msg = "macro market observations must be unique by instrument and kind"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class HistoricalMacroSurprise:
    """Previously known standardized headline surprise for percentile replay."""

    historical_event_id: GlobalEventId
    event_type: MacroEventType
    official_publication_wall_clock_utc_ns: int
    available_wall_clock_utc_ns: int
    headline_surprise_ppm: int
    source_id: str

    def __post_init__(self) -> None:
        """Validate event times, signed surprise, and source identity."""
        _positive_timestamp(
            self.official_publication_wall_clock_utc_ns,
            "historical official_publication_wall_clock_utc_ns",
        )
        if self.available_wall_clock_utc_ns < (
            self.official_publication_wall_clock_utc_ns
        ):
            msg = "historical macro surprise availability precedes publication"
            raise ValueError(msg)
        _int64(self.headline_surprise_ppm, "historical headline_surprise_ppm")
        _bounded_text(self.source_id, "historical source_id", 64)


@dataclass(frozen=True, slots=True)
class MacroInputBundle:
    """Point-in-time calendar, frozen consensus, release, and market context."""

    session_id: SessionId
    target_instrument_id: InstrumentId
    configuration_version: ConfigurationVersion
    calendar: MacroCalendarEvent
    consensus_snapshot: MacroConsensusSnapshot
    sources: tuple[MacroSourceEvidence, ...]
    historical_surprises: tuple[HistoricalMacroSurprise, ...]
    as_of_wall_clock_utc_ns: int
    production_process_monotonic_time_ns: int
    release: MacroActualRelease | None = None
    market_snapshot: MacroMarketSnapshot | None = None
    schema_version: str = MACRO_SCHEMA_VERSION

    def __post_init__(self) -> None:
        """Validate bounds, evidence, freeze, release lineage, and point in time."""
        if self.schema_version != MACRO_SCHEMA_VERSION:
            msg = "unsupported normalized macro schema version"
            raise ValueError(msg)
        _positive_timestamp(self.as_of_wall_clock_utc_ns, "as_of_wall_clock_utc_ns")
        _positive_timestamp(
            self.production_process_monotonic_time_ns,
            "production_process_monotonic_time_ns",
        )
        if (
            len(self.sources) > MAX_SOURCES
            or len(self.historical_surprises) > MAX_HISTORY
        ):
            msg = "macro source or history collection exceeds its bound"
            raise ValueError(msg)
        if self.consensus_snapshot.calendar_event_id != self.calendar.calendar_event_id:
            msg = "macro consensus references a different calendar event"
            raise ValueError(msg)
        if self.consensus_snapshot.frozen_wall_clock_utc_ns >= (
            self.calendar.scheduled_release_wall_clock_utc_ns
        ):
            msg = "macro consensus must freeze before the scheduled release"
            raise ValueError(msg)
        source_index = self._validate_sources()
        self._validate_evidence_graph(source_index)
        self._validate_calendar_and_consensus(source_index)
        self._validate_point_in_time(source_index)

    def _validate_sources(self) -> dict[str, MacroSourceEvidence]:
        source_index = {item.source_id: item for item in self.sources}
        if len(source_index) != len(self.sources):
            msg = "macro source identifiers must be unique"
            raise ValueError(msg)
        if (
            sum(item.kind is MacroSourceKind.OFFICIAL_CALENDAR for item in self.sources)
            != 1
        ):
            msg = "macro input requires exactly one official calendar source"
            raise ValueError(msg)
        if not any(
            item.kind is MacroSourceKind.CONSENSUS_SNAPSHOT for item in self.sources
        ):
            msg = "macro input requires a consensus snapshot source"
            raise ValueError(msg)
        official_count = sum(
            item.kind is MacroSourceKind.OFFICIAL_RELEASE for item in self.sources
        )
        if official_count != (1 if self.release is not None else 0):
            msg = "macro official release source does not match release presence"
            raise ValueError(msg)
        return source_index

    def _all_references(self) -> tuple[MacroEvidenceReference, ...]:
        values: list[MacroEvidenceReference] = []
        for field in self.calendar.expected_fields:
            values.extend(field.evidence)
        for estimate in self.consensus_snapshot.estimates:
            values.extend(estimate.evidence)
        if self.release is not None:
            for observation in self.release.observations:
                values.extend(observation.evidence)
            for prior in self.release.prior_values:
                values.extend(prior.evidence)
        return tuple(values)

    def _validate_evidence_graph(
        self, source_index: dict[str, MacroSourceEvidence]
    ) -> None:
        excerpt_index = {
            (source.source_id, excerpt.excerpt_id)
            for source in self.sources
            for excerpt in source.excerpts
        }
        if any(
            reference.source_id not in source_index
            or (reference.source_id, reference.excerpt_id) not in excerpt_index
            for reference in self._all_references()
        ):
            msg = "macro evidence reference does not resolve to an exact excerpt"
            raise ValueError(msg)
        if any(
            item.source_id not in source_index for item in self.historical_surprises
        ):
            msg = "historical macro surprise source does not resolve"
            raise ValueError(msg)
        if self.market_snapshot is not None and (
            self.market_snapshot.source_id not in source_index
            or source_index[self.market_snapshot.source_id].kind
            is not MacroSourceKind.MARKET_FEATURES
        ):
            msg = "macro market snapshot source does not resolve to market features"
            raise ValueError(msg)

    def _validate_calendar_and_consensus(
        self, source_index: dict[str, MacroSourceEvidence]
    ) -> None:
        if any(
            estimate.identity.event_type is not self.calendar.event_type
            for estimate in self.consensus_snapshot.estimates
        ):
            msg = "macro consensus event type differs from calendar"
            raise ValueError(msg)
        if any(
            source_index[reference.source_id].kind
            is not MacroSourceKind.OFFICIAL_CALENDAR
            for item in self.calendar.expected_fields
            for reference in item.evidence
        ):
            msg = "macro expected fields require official calendar evidence"
            raise ValueError(msg)
        if any(
            source_index[reference.source_id].received_wall_clock_utc_ns
            > self.consensus_snapshot.frozen_wall_clock_utc_ns
            for estimate in self.consensus_snapshot.estimates
            for reference in estimate.evidence
        ):
            msg = "macro consensus evidence arrived after the frozen snapshot"
            raise ValueError(msg)

    def _validate_point_in_time(
        self, source_index: dict[str, MacroSourceEvidence]
    ) -> None:
        if any(
            source.received_wall_clock_utc_ns > self.as_of_wall_clock_utc_ns
            for source in self.sources
        ):
            msg = "macro source was unavailable at the as-of cutoff"
            raise ValueError(msg)
        scheduled = self.calendar.scheduled_release_wall_clock_utc_ns
        if any(
            source_index[reference.source_id].received_wall_clock_utc_ns >= scheduled
            for field in self.calendar.expected_fields
            for reference in field.evidence
        ):
            msg = "macro calendar evidence was not known before scheduled release"
            raise ValueError(msg)
        if any(
            item.event_type is not self.calendar.event_type
            or item.official_publication_wall_clock_utc_ns >= scheduled
            or item.available_wall_clock_utc_ns
            > self.consensus_snapshot.frozen_wall_clock_utc_ns
            for item in self.historical_surprises
        ):
            msg = "historical macro surprise leaks the current or a future event"
            raise ValueError(msg)
        if self.release is None:
            if self.market_snapshot is not None:
                msg = "pre-release macro input cannot contain response features"
                raise ValueError(msg)
            return
        self._validate_release_time(source_index, scheduled)
        self._validate_market_time()

    def _validate_release_time(
        self,
        source_index: dict[str, MacroSourceEvidence],
        scheduled: int,
    ) -> None:
        if self.release is None:
            msg = "release-time validation requires a release"
            raise ValueError(msg)
        release = self.release
        if release.official_publication_wall_clock_utc_ns < scheduled:
            msg = "macro official publication precedes its scheduled time"
            raise ValueError(msg)
        if release.receipt_wall_clock_utc_ns > self.as_of_wall_clock_utc_ns:
            msg = "macro release receipt is after the as-of cutoff"
            raise ValueError(msg)
        official_source = next(
            item
            for item in self.sources
            if item.kind is MacroSourceKind.OFFICIAL_RELEASE
        )
        if (
            official_source.publication_wall_clock_utc_ns
            != release.official_publication_wall_clock_utc_ns
            or official_source.received_wall_clock_utc_ns
            != release.receipt_wall_clock_utc_ns
        ):
            msg = "macro release timestamps differ from official-source provenance"
            raise ValueError(msg)
        if any(
            observation.source_id not in source_index
            or source_index[observation.source_id].kind
            not in {MacroSourceKind.OFFICIAL_RELEASE, MacroSourceKind.DATA_PROVIDER}
            or source_index[observation.source_id].received_wall_clock_utc_ns
            < release.official_publication_wall_clock_utc_ns
            or observation.identity.event_type is not self.calendar.event_type
            for observation in release.observations
        ):
            msg = "macro release observation source, time, or event type is invalid"
            raise ValueError(msg)
        if any(
            prior.known_available_wall_clock_utc_ns
            >= release.official_publication_wall_clock_utc_ns
            or prior.identity.event_type is not self.calendar.event_type
            for prior in release.prior_values
        ):
            msg = "macro prior value was not known before the current release"
            raise ValueError(msg)

    def _validate_market_time(self) -> None:
        if self.market_snapshot is None:
            return
        if self.release is None:
            msg = "market-time validation requires a release"
            raise ValueError(msg)
        snapshot = self.market_snapshot
        receipt = self.release.receipt_wall_clock_utc_ns
        if (
            snapshot.available_wall_clock_utc_ns < receipt
            or snapshot.available_wall_clock_utc_ns > self.as_of_wall_clock_utc_ns
            or any(
                item.window_start_wall_clock_utc_ns >= receipt
                or item.window_end_wall_clock_utc_ns < receipt
                or item.window_end_wall_clock_utc_ns
                > snapshot.available_wall_clock_utc_ns
                for item in snapshot.observations
            )
        ):
            msg = "macro cross-asset response window or availability is invalid"
            raise ValueError(msg)

    def canonical_bytes(self) -> bytes:
        """Return deterministic canonical JSON for persistence and replay."""
        return _canonical_json_bytes(self)

    @property
    def sha256(self) -> bytes:
        """Hash the complete normalized macro input contract."""
        return hashlib.sha256(self.canonical_bytes()).digest()


@dataclass(frozen=True, slots=True)
class MacroFieldSurprise:
    """One actual-to-frozen-consensus comparison or explicit failure reason."""

    identity: MacroMetricIdentity
    actual_value: int | None
    consensus_value: int | None
    dispersion: int | None
    surprise_ppm: int | None
    reason: MacroReasonCode | None


@dataclass(frozen=True, slots=True)
class MacroRevisionSurprise:
    """A revision kept separate from the value known before release."""

    identity: MacroMetricIdentity
    prior_value: int
    revised_value: int
    delta_value: int
    revision_surprise_ppm: int


@dataclass(frozen=True, slots=True)
class CrossAssetResponseFeature:
    """Normalized response magnitude retaining raw value and explicit unit."""

    instrument_id: InstrumentId
    kind: CrossAssetKind
    group: CrossAssetGroup
    unit: CrossAssetUnit
    response_value: int
    normalized_magnitude_ppm: int

    def __post_init__(self) -> None:
        """Validate bounded normalized magnitude."""
        if not 0 <= self.normalized_magnitude_ppm <= MAX_ABSOLUTE_RESPONSE_PPM:
            msg = "normalized cross-asset response magnitude exceeds its PPM bound"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class MacroDirectionDistribution:
    """Fixed-point expected target direction distribution."""

    down_ppm: int
    flat_ppm: int
    up_ppm: int

    def __post_init__(self) -> None:
        """Require bounded probabilities summing exactly to one million."""
        values = (self.down_ppm, self.flat_ppm, self.up_ppm)
        if any(not 0 <= item <= PPM for item in values) or sum(values) != PPM:
            msg = "macro direction probabilities must sum to 1,000,000 PPM"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class MacroSpecialistResult:
    """Detailed publish or abstention result with optional common forecast."""

    macro_event_id: GlobalEventId
    model_id: ModelId
    model_version: ModelVersion
    input_sha256: bytes
    consensus_snapshot_sha256: bytes
    release_sha256: bytes | None
    revision: int
    phase: MacroEventPhase
    decision: MacroDecision
    scheduled_release_wall_clock_utc_ns: int
    official_publication_wall_clock_utc_ns: int | None
    receipt_wall_clock_utc_ns: int | None
    release_delay_ns: int | None
    headline_surprise: MacroFieldSurprise
    core_surprise: MacroFieldSurprise | None
    subcomponent_surprises: tuple[MacroFieldSurprise, ...]
    revision_surprises: tuple[MacroRevisionSurprise, ...]
    surprise_percentile_ppm: int | None
    cross_asset_response_features: tuple[CrossAssetResponseFeature, ...]
    source_quality_score_ppm: int
    reason_codes: tuple[MacroReasonCode, ...]
    forecast_id: ForecastId | None
    expected_return_ppm: int | None
    expected_volatility_ppm: int | None
    direction: MacroDirectionDistribution | None
    production_process_monotonic_time_ns: int
    valid_until_process_monotonic_time_ns: int | None
    forecast_contract_bytes: bytes | None
    schema_version: str = MACRO_SCHEMA_VERSION

    def __post_init__(self) -> None:
        """Validate state, bounds, lineage, and forecast/abstention exclusivity."""
        _valid_digest(self.input_sha256, "macro result input_sha256")
        _valid_digest(
            self.consensus_snapshot_sha256,
            "macro result consensus_snapshot_sha256",
        )
        if self.release_sha256 is not None:
            _valid_digest(self.release_sha256, "macro result release_sha256")
        if self.revision < 0 or self.schema_version != MACRO_SCHEMA_VERSION:
            msg = "macro result revision or schema version is invalid"
            raise ValueError(msg)
        _positive_timestamp(
            self.scheduled_release_wall_clock_utc_ns,
            "result scheduled_release_wall_clock_utc_ns",
        )
        if not 0 <= self.source_quality_score_ppm <= PPM:
            msg = "macro result source quality is outside [0, 1,000,000] PPM"
            raise ValueError(msg)
        if len(set(self.reason_codes)) != len(self.reason_codes):
            msg = "macro result reason codes must be unique"
            raise ValueError(msg)
        if self.surprise_percentile_ppm is not None and not (
            0 <= self.surprise_percentile_ppm <= PPM
        ):
            msg = "macro surprise percentile is outside [0, 1,000,000] PPM"
            raise ValueError(msg)
        _positive_timestamp(
            self.production_process_monotonic_time_ns,
            "result production_process_monotonic_time_ns",
        )
        self._validate_release_state()
        self._validate_decision_state()

    def _validate_release_state(self) -> None:
        has_release = self.revision > 0
        times = (
            self.official_publication_wall_clock_utc_ns,
            self.receipt_wall_clock_utc_ns,
            self.release_delay_ns,
            self.release_sha256,
        )
        if has_release != all(item is not None for item in times):
            msg = "macro result release revision and release fields disagree"
            raise ValueError(msg)
        publication = cast("int", self.official_publication_wall_clock_utc_ns)
        receipt = cast("int", self.receipt_wall_clock_utc_ns)
        delay = cast("int", self.release_delay_ns)
        if has_release and (receipt < publication or delay < 0):
            msg = "macro result release timing is invalid"
            raise ValueError(msg)
        if not has_release and self.phase not in {
            MacroEventPhase.SCHEDULED,
            MacroEventPhase.DELAYED,
        }:
            msg = "macro result without release has an invalid phase"
            raise ValueError(msg)

    def _validate_decision_state(self) -> None:
        forecast_values = (
            self.forecast_id,
            self.expected_return_ppm,
            self.expected_volatility_ppm,
            self.direction,
            self.valid_until_process_monotonic_time_ns,
            self.forecast_contract_bytes,
        )
        if self.decision is MacroDecision.ABSTAIN:
            if any(item is not None for item in forecast_values):
                msg = "macro abstention cannot carry forecast values"
                raise ValueError(msg)
            return
        if any(item is None for item in forecast_values):
            msg = "published macro result requires all forecast values"
            raise ValueError(msg)
        if self.phase not in {
            MacroEventPhase.RELEASE_COMPLETE,
            MacroEventPhase.CORRECTED,
        }:
            msg = "published macro result requires a complete release phase"
            raise ValueError(msg)
        expected_return = cast("int", self.expected_return_ppm)
        expected_volatility = cast("int", self.expected_volatility_ppm)
        valid_until = cast("int", self.valid_until_process_monotonic_time_ns)
        forecast_bytes = cast("bytes", self.forecast_contract_bytes)
        if (
            abs(expected_return) > MAX_ABSOLUTE_RESPONSE_PPM
            or not 0 <= expected_volatility <= MAX_ABSOLUTE_RESPONSE_PPM
        ):
            msg = "published macro return or volatility is outside its PPM bound"
            raise ValueError(msg)
        if valid_until <= self.production_process_monotonic_time_ns:
            msg = "published macro forecast lifetime is invalid"
            raise ValueError(msg)
        if forecast_bytes[4:8] != b"AMCR":
            msg = "published macro result lacks a canonical ModelForecast"
            raise ValueError(msg)

    def canonical_bytes(self) -> bytes:
        """Return deterministic detailed-result JSON for replay comparison."""
        return _canonical_json_bytes(self)

    @property
    def sha256(self) -> bytes:
        """Hash the complete result, including optional forecast bytes."""
        return hashlib.sha256(self.canonical_bytes()).digest()


def _canonical_value(value: object) -> object:
    if isinstance(value, IntEnum):
        return value.name
    if isinstance(value, Identifier128):
        return value.hex()
    if isinstance(value, bytes):
        return value.hex()
    if is_dataclass(value) and not isinstance(value, type):
        return {
            item.name: _canonical_value(getattr(value, item.name))
            for item in fields(value)
        }
    if isinstance(value, tuple):
        return [_canonical_value(item) for item in value]
    if value is None or isinstance(value, (bool, int, str)):
        return value
    msg = f"unsupported canonical macro value: {type(value).__name__}"
    raise TypeError(msg)


def _canonical_json_bytes(value: object) -> bytes:
    return json.dumps(
        _canonical_value(value),
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")
