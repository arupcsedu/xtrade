"""Versioned integer-only contracts for the deterministic earnings specialist."""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass, fields, is_dataclass
from enum import IntEnum
from typing import TYPE_CHECKING, Final

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

EARNINGS_SCHEMA_VERSION: Final = "1.0.0"
PPM: Final = 1_000_000
SHA256_BYTES: Final = 32
MAX_TEXT_BYTES: Final = 128
MAX_SOURCES: Final = 32
MAX_METRICS: Final = 128
MAX_ESTIMATES: Final = 128
MAX_GUIDANCE: Final = 64
MAX_ADJUSTMENTS: Final = 32
MAX_HISTORY: Final = 64
MAX_EVIDENCE_REFERENCES: Final = 16
MAX_ABSOLUTE_RETURN_PPM: Final = 10_000_000
MAX_VOLATILITY_PPM: Final = 10_000_000
MAX_INT64: Final = (1 << 63) - 1
MIN_INT64: Final = -(1 << 63)


class AccountingBasis(IntEnum):
    """Accounting basis is part of metric identity and never inferred."""

    GAAP = 1
    NON_GAAP = 2
    NOT_APPLICABLE = 3


class EarningsMetricKind(IntEnum):
    """Normalized earnings metrics supported by the initial specialist."""

    EPS = 1
    REVENUE = 2
    GROSS_MARGIN = 3
    OPERATING_MARGIN = 4
    NET_MARGIN = 5
    NET_INCOME = 6
    FREE_CASH_FLOW = 7
    CAPEX = 8
    SEGMENT_REVENUE = 9
    SEGMENT_OPERATING_INCOME = 10


class EarningsMetricUnit(IntEnum):
    """Explicit fixed-point units; binary floating point is not accepted."""

    CURRENCY_NANOS = 1
    CURRENCY_NANOS_PER_SHARE = 2
    PPM = 3
    QUANTITY_UNITS = 4


class EarningsSourceKind(IntEnum):
    """Point-in-time input source categories."""

    OFFICIAL_EARNINGS_RELEASE = 1
    REGULATORY_FILING = 2
    CONSENSUS_ESTIMATES = 3
    PRIOR_REPORTED_VALUES = 4
    COMPANY_GUIDANCE = 5
    SEGMENT_RESULTS = 6
    OPTION_IMPLIED_MOVE = 7
    HISTORICAL_REACTION = 8
    TRANSCRIPT = 9
    POST_RELEASE_MARKET_FEATURES = 10


class EarningsPhase(IntEnum):
    """Deterministic lifecycle phases exposed to integration hooks."""

    PRE_EARNINGS = 1
    RELEASE_PROCESSING = 2
    PRICE_DISCOVERY = 3
    RECOVERY = 4


class EarningsReasonCode(IntEnum):
    """Machine-readable uncertainty and degradation reasons."""

    MISSING_CONSENSUS = 1
    INCOMPATIBLE_METRIC = 2
    MISSING_PRIOR_GUIDANCE = 3
    MISSING_FILING = 4
    MISSING_TRANSCRIPT = 5
    MISSING_OPTION_IMPLIED_MOVE = 6
    INSUFFICIENT_HISTORY = 7
    MISSING_POST_RELEASE_FEATURES = 8
    CORRECTED_RELEASE = 9
    GAAP_NON_GAAP_DIVERGENCE = 10
    SEGMENT_TOTAL_MISMATCH = 11
    MATERIAL_ONE_TIME_ADJUSTMENTS = 12
    MISSING_FREE_CASH_FLOW = 13
    NO_COMPARABLE_METRICS = 14


class AccountingQualityFlag(IntEnum):
    """Deterministic accounting-quality diagnostics, not allegations."""

    GAAP_NON_GAAP_DIVERGENCE = 1
    MATERIAL_ONE_TIME_ADJUSTMENTS = 2
    SEGMENT_TOTAL_MISMATCH = 3
    MISSING_FREE_CASH_FLOW = 4
    CORRECTED_RELEASE = 5


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
class EarningsMetricIdentity:
    """Exact comparability key for actual, estimate, prior, and guidance values."""

    kind: EarningsMetricKind
    basis: AccountingBasis
    unit: EarningsMetricUnit
    fiscal_period: str
    segment_name: str = ""

    def __post_init__(self) -> None:
        """Validate the bounded period and optional segment identity."""
        _bounded_text(self.fiscal_period, "fiscal_period", 32)
        if self.segment_name:
            _bounded_text(self.segment_name, "segment_name", 64)
        is_segment = self.kind in {
            EarningsMetricKind.SEGMENT_REVENUE,
            EarningsMetricKind.SEGMENT_OPERATING_INCOME,
        }
        if is_segment != bool(self.segment_name):
            msg = (
                "segment metrics require a segment and consolidated metrics forbid one"
            )
            raise ValueError(msg)


@dataclass(frozen=True, slots=True, order=True)
class EarningsEvidenceReference:
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
class EarningsSourceEvidence:
    """Authenticated/sanitized source provenance retained by content hash."""

    source_id: str
    source_global_event_id: GlobalEventId
    kind: EarningsSourceKind
    provider_id: str
    document_id: str
    source_content_sha256: bytes
    sanitized_content_sha256: bytes
    publication_wall_clock_utc_ns: int
    received_wall_clock_utc_ns: int
    excerpts: tuple[EvidenceExcerpt, ...]

    def __post_init__(self) -> None:
        """Validate source timestamps, digests, bounds, and excerpt identity."""
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
            msg = "source receipt precedes publication"
            raise ValueError(msg)
        if not self.excerpts or len(self.excerpts) > MAX_EVIDENCE_REFERENCES:
            msg = "source evidence requires one to sixteen exact excerpts"
            raise ValueError(msg)
        identifiers = {item.excerpt_id for item in self.excerpts}
        if len(identifiers) != len(self.excerpts):
            msg = "source excerpt identifiers must be unique"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class NormalizedEarningsMetric:
    """One integer-valued actual or prior reported metric."""

    identity: EarningsMetricIdentity
    value: int
    evidence: tuple[EarningsEvidenceReference, ...]

    def __post_init__(self) -> None:
        """Validate wire-safe value and bounded evidence references."""
        _int64(self.value, "metric value")
        _validate_reference_count(self.evidence, "metric")


@dataclass(frozen=True, slots=True)
class EstimateDistribution:
    """Point-in-time analyst distribution for one exact metric identity."""

    identity: EarningsMetricIdentity
    consensus_value: int
    dispersion: int
    minimum_value: int
    maximum_value: int
    analyst_count: int
    available_wall_clock_utc_ns: int
    evidence: tuple[EarningsEvidenceReference, ...]

    def __post_init__(self) -> None:
        """Validate range, timestamp, analyst count, and exact evidence."""
        for name, value in (
            ("consensus_value", self.consensus_value),
            ("minimum_value", self.minimum_value),
            ("maximum_value", self.maximum_value),
        ):
            _int64(value, name)
        if (
            self.dispersion < 0
            or self.analyst_count <= 0
            or not self.minimum_value <= self.consensus_value <= self.maximum_value
        ):
            msg = "estimate dispersion, analyst count, or value range is invalid"
            raise ValueError(msg)
        _positive_timestamp(
            self.available_wall_clock_utc_ns,
            "estimate available_wall_clock_utc_ns",
        )
        _validate_reference_count(self.evidence, "estimate")


@dataclass(frozen=True, slots=True)
class GuidanceRange:
    """Evidence-backed company guidance range for one exact metric identity."""

    identity: EarningsMetricIdentity
    lower_value: int
    upper_value: int
    evidence: tuple[EarningsEvidenceReference, ...]

    def __post_init__(self) -> None:
        """Validate ordered wire-safe endpoints and evidence references."""
        _int64(self.lower_value, "guidance lower_value")
        _int64(self.upper_value, "guidance upper_value")
        if self.lower_value > self.upper_value:
            msg = "guidance lower_value exceeds upper_value"
            raise ValueError(msg)
        _validate_reference_count(self.evidence, "guidance")


@dataclass(frozen=True, slots=True)
class OneTimeAdjustment:
    """Explicit one-time adjustment amount with retained evidence."""

    label: str
    amount_currency_nanos: int
    evidence: tuple[EarningsEvidenceReference, ...]

    def __post_init__(self) -> None:
        """Validate label, amount, and evidence."""
        _bounded_text(self.label, "adjustment label", 64)
        _int64(self.amount_currency_nanos, "adjustment amount_currency_nanos")
        _validate_reference_count(self.evidence, "adjustment")


@dataclass(frozen=True, slots=True)
class OptionImpliedMove:
    """Point-in-time pre-release option-implied absolute move."""

    absolute_move_ppm: int
    available_wall_clock_utc_ns: int
    evidence: tuple[EarningsEvidenceReference, ...]

    def __post_init__(self) -> None:
        """Validate bounded move, timestamp, and evidence."""
        if not 0 < self.absolute_move_ppm <= MAX_ABSOLUTE_RETURN_PPM:
            msg = "option-implied move is outside its PPM bound"
            raise ValueError(msg)
        _positive_timestamp(
            self.available_wall_clock_utc_ns,
            "option move available_wall_clock_utc_ns",
        )
        _validate_reference_count(self.evidence, "option move")


@dataclass(frozen=True, slots=True)
class HistoricalEarningsReaction:
    """Point-in-time historical reaction known before the current release."""

    historical_event_id: str
    publication_wall_clock_utc_ns: int
    available_wall_clock_utc_ns: int
    gap_return_ppm: int
    realized_volatility_ppm: int
    price_discovery_duration_ns: int

    def __post_init__(self) -> None:
        """Validate point-in-time timestamps and bounded reaction values."""
        _bounded_text(self.historical_event_id, "historical_event_id", 64)
        _positive_timestamp(
            self.publication_wall_clock_utc_ns,
            "historical publication_wall_clock_utc_ns",
        )
        if self.available_wall_clock_utc_ns < self.publication_wall_clock_utc_ns:
            msg = "historical reaction availability precedes publication"
            raise ValueError(msg)
        if abs(self.gap_return_ppm) > MAX_ABSOLUTE_RETURN_PPM:
            msg = "historical gap return exceeds its PPM bound"
            raise ValueError(msg)
        if not 0 <= self.realized_volatility_ppm <= MAX_VOLATILITY_PPM:
            msg = "historical realized volatility exceeds its PPM bound"
            raise ValueError(msg)
        _positive_timestamp(
            self.price_discovery_duration_ns,
            "historical price_discovery_duration_ns",
        )


@dataclass(frozen=True, slots=True)
class EarningsMarketFeatures:
    """Immutable feature provenance used by the common forecast contract."""

    feature_snapshot_id: FeatureSnapshotId
    as_of_exchange_event_time_ns: int
    available_wall_clock_utc_ns: int
    return_since_release_ppm: int
    realized_volatility_ppm: int
    is_post_release: bool
    valid: bool = True

    def __post_init__(self) -> None:
        """Validate timestamps and fixed-point feature ranges."""
        _positive_timestamp(
            self.as_of_exchange_event_time_ns,
            "feature as_of_exchange_event_time_ns",
        )
        _positive_timestamp(
            self.available_wall_clock_utc_ns,
            "feature available_wall_clock_utc_ns",
        )
        if abs(self.return_since_release_ppm) > MAX_ABSOLUTE_RETURN_PPM:
            msg = "feature return exceeds its PPM bound"
            raise ValueError(msg)
        if not 0 <= self.realized_volatility_ppm <= MAX_VOLATILITY_PPM:
            msg = "feature realized volatility exceeds its PPM bound"
            raise ValueError(msg)


def _validate_reference_count(
    references: tuple[EarningsEvidenceReference, ...], owner: str
) -> None:
    if not references or len(references) > MAX_EVIDENCE_REFERENCES:
        msg = f"{owner} requires one to sixteen evidence references"
        raise ValueError(msg)
    if len(set(references)) != len(references):
        msg = f"{owner} evidence references must be unique"
        raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class EarningsInputBundle:
    """Immutable normalized earnings input with point-in-time provenance."""

    earnings_event_id: GlobalEventId
    session_id: SessionId
    instrument_id: InstrumentId
    configuration_version: ConfigurationVersion
    revision: int
    as_of_wall_clock_utc_ns: int
    production_process_monotonic_time_ns: int
    sources: tuple[EarningsSourceEvidence, ...]
    actuals: tuple[NormalizedEarningsMetric, ...]
    consensus: tuple[EstimateDistribution, ...]
    prior_reported_values: tuple[NormalizedEarningsMetric, ...]
    current_guidance: tuple[GuidanceRange, ...]
    prior_guidance: tuple[GuidanceRange, ...]
    one_time_adjustments: tuple[OneTimeAdjustment, ...]
    historical_reactions: tuple[HistoricalEarningsReaction, ...]
    market_features: EarningsMarketFeatures
    option_implied_move: OptionImpliedMove | None = None
    correction_of_input_sha256: bytes | None = None
    schema_version: str = EARNINGS_SCHEMA_VERSION

    def __post_init__(self) -> None:
        """Validate bounds, evidence graph, corrections, and point-in-time data."""
        if self.schema_version != EARNINGS_SCHEMA_VERSION:
            msg = "unsupported normalized earnings schema version"
            raise ValueError(msg)
        if self.revision <= 0:
            msg = "earnings revision must be positive"
            raise ValueError(msg)
        if (self.revision == 1) != (self.correction_of_input_sha256 is None):
            msg = "only corrected revisions require a parent input digest"
            raise ValueError(msg)
        if self.correction_of_input_sha256 is not None:
            _valid_digest(
                self.correction_of_input_sha256,
                "correction_of_input_sha256",
            )
        _positive_timestamp(self.as_of_wall_clock_utc_ns, "as_of_wall_clock_utc_ns")
        _positive_timestamp(
            self.production_process_monotonic_time_ns,
            "production_process_monotonic_time_ns",
        )
        self._validate_collection_bounds()
        source_index = self._validate_sources()
        self._validate_evidence_graph(source_index)
        self._validate_unique_metric_identities()
        self._validate_point_in_time(source_index)

    def _validate_collection_bounds(self) -> None:
        collections = (
            (self.sources, MAX_SOURCES, "sources"),
            (self.actuals, MAX_METRICS, "actuals"),
            (self.consensus, MAX_ESTIMATES, "consensus"),
            (self.prior_reported_values, MAX_METRICS, "prior values"),
            (self.current_guidance, MAX_GUIDANCE, "current guidance"),
            (self.prior_guidance, MAX_GUIDANCE, "prior guidance"),
            (self.one_time_adjustments, MAX_ADJUSTMENTS, "adjustments"),
            (self.historical_reactions, MAX_HISTORY, "history"),
        )
        if not self.actuals:
            msg = "earnings input requires at least one reported actual"
            raise ValueError(msg)
        for values, maximum, name in collections:
            if len(values) > maximum:
                msg = f"earnings {name} exceeds its configured bound"
                raise ValueError(msg)

    def _validate_sources(self) -> dict[str, EarningsSourceEvidence]:
        source_index = {item.source_id: item for item in self.sources}
        if len(source_index) != len(self.sources):
            msg = "earnings source identifiers must be unique"
            raise ValueError(msg)
        official = [
            item
            for item in self.sources
            if item.kind is EarningsSourceKind.OFFICIAL_EARNINGS_RELEASE
        ]
        if len(official) != 1:
            msg = "earnings input requires exactly one official release source"
            raise ValueError(msg)
        return source_index

    def _validate_evidence_graph(
        self, source_index: dict[str, EarningsSourceEvidence]
    ) -> None:
        excerpt_index = {
            (source.source_id, excerpt.excerpt_id)
            for source in self.sources
            for excerpt in source.excerpts
        }
        references = (
            reference
            for collection in (
                self.actuals,
                self.consensus,
                self.prior_reported_values,
                self.current_guidance,
                self.prior_guidance,
                self.one_time_adjustments,
            )
            for item in collection
            for reference in item.evidence
        )
        if any(
            reference.source_id not in source_index
            or (reference.source_id, reference.excerpt_id) not in excerpt_index
            for reference in references
        ):
            msg = "earnings evidence reference does not resolve to an exact excerpt"
            raise ValueError(msg)
        if self.option_implied_move is not None and any(
            reference.source_id not in source_index
            or (reference.source_id, reference.excerpt_id) not in excerpt_index
            for reference in self.option_implied_move.evidence
        ):
            msg = "option-implied evidence reference does not resolve"
            raise ValueError(msg)

    def _validate_unique_metric_identities(self) -> None:
        collections = (
            (self.actuals, "actual"),
            (self.consensus, "consensus"),
            (self.prior_reported_values, "prior reported"),
            (self.current_guidance, "current guidance"),
            (self.prior_guidance, "prior guidance"),
        )
        for values, name in collections:
            identities = {item.identity for item in values}
            if len(identities) != len(values):
                msg = f"duplicate {name} metric identity"
                raise ValueError(msg)

    def _validate_point_in_time(
        self, source_index: dict[str, EarningsSourceEvidence]
    ) -> None:
        if any(
            source.received_wall_clock_utc_ns > self.as_of_wall_clock_utc_ns
            for source in self.sources
        ) or self.market_features.available_wall_clock_utc_ns > (
            self.as_of_wall_clock_utc_ns
        ):
            msg = "earnings source or feature was unavailable at the as-of cutoff"
            raise ValueError(msg)
        official = next(
            item
            for item in source_index.values()
            if item.kind is EarningsSourceKind.OFFICIAL_EARNINGS_RELEASE
        )
        release_time = official.publication_wall_clock_utc_ns
        if any(
            estimate.available_wall_clock_utc_ns > release_time
            for estimate in self.consensus
        ):
            msg = "consensus became available after the official release"
            raise ValueError(msg)
        if any(
            source_index[reference.source_id].received_wall_clock_utc_ns > release_time
            for estimate in self.consensus
            for reference in estimate.evidence
        ):
            msg = "consensus evidence was received after the official release"
            raise ValueError(msg)
        if (
            self.option_implied_move is not None
            and self.option_implied_move.available_wall_clock_utc_ns > release_time
        ):
            msg = "option-implied move became available after the official release"
            raise ValueError(msg)
        if self.option_implied_move is not None and any(
            source_index[reference.source_id].received_wall_clock_utc_ns > release_time
            for reference in self.option_implied_move.evidence
        ):
            msg = "option-implied evidence was received after the official release"
            raise ValueError(msg)
        if any(
            reaction.publication_wall_clock_utc_ns >= release_time
            or reaction.available_wall_clock_utc_ns > release_time
            for reaction in self.historical_reactions
        ):
            msg = "historical reaction leaks the current or a future event"
            raise ValueError(msg)
        feature_time = self.market_features.available_wall_clock_utc_ns
        if self.market_features.is_post_release and feature_time < release_time:
            msg = "post-release features precede the release"
            raise ValueError(msg)

    @property
    def official_release(self) -> EarningsSourceEvidence:
        """Return the uniquely validated official release source."""
        return next(
            item
            for item in self.sources
            if item.kind is EarningsSourceKind.OFFICIAL_EARNINGS_RELEASE
        )

    def canonical_bytes(self) -> bytes:
        """Return deterministic canonical JSON for persistence and replay."""
        return _canonical_json_bytes(self)

    @property
    def sha256(self) -> bytes:
        """Hash the complete normalized input contract."""
        return hashlib.sha256(self.canonical_bytes()).digest()


@dataclass(frozen=True, slots=True)
class MetricSurprise:
    """One actual-to-consensus assessment without incompatible comparison."""

    identity: EarningsMetricIdentity
    actual_value: int
    consensus_value: int | None
    dispersion: int | None
    surprise_ppm: int | None
    reason: EarningsReasonCode | None


@dataclass(frozen=True, slots=True)
class GuidanceChange:
    """One midpoint guidance change relative to compatible prior guidance."""

    identity: EarningsMetricIdentity
    current_midpoint: int
    prior_midpoint: int | None
    delta_value: int | None
    change_ppm: int | None
    reason: EarningsReasonCode | None


@dataclass(frozen=True, slots=True)
class DirectionDistribution:
    """Fixed-point expected direction distribution."""

    down_ppm: int
    flat_ppm: int
    up_ppm: int

    def __post_init__(self) -> None:
        """Require three bounded probabilities summing exactly to one million."""
        values = (self.down_ppm, self.flat_ppm, self.up_ppm)
        if any(not 0 <= item <= PPM for item in values) or sum(values) != PPM:
            msg = "direction probabilities must be bounded and sum to 1,000,000 PPM"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class ExpectedGapRange:
    """Expected opening/reaction gap range in signed return PPM."""

    lower_return_ppm: int
    center_return_ppm: int
    upper_return_ppm: int

    def __post_init__(self) -> None:
        """Validate an ordered bounded return range."""
        if (
            not -MAX_ABSOLUTE_RETURN_PPM
            <= self.lower_return_ppm
            <= self.center_return_ppm
            <= self.upper_return_ppm
            <= MAX_ABSOLUTE_RETURN_PPM
        ):
            msg = "expected gap range is unordered or outside its PPM bound"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class EarningsSpecialistResult:
    """Detailed deterministic result plus canonical common forecast bytes."""

    earnings_event_id: GlobalEventId
    forecast_id: ForecastId
    model_id: ModelId
    model_version: ModelVersion
    input_sha256: bytes
    revision: int
    phase: EarningsPhase
    surprises: tuple[MetricSurprise, ...]
    guidance_changes: tuple[GuidanceChange, ...]
    accounting_quality_flags: tuple[AccountingQualityFlag, ...]
    materiality_ppm: int
    direction: DirectionDistribution
    expected_return_ppm: int
    expected_volatility_ppm: int
    expected_gap: ExpectedGapRange
    estimated_price_discovery_duration_ns: int
    uncertainty_ppm: int
    reason_codes: tuple[EarningsReasonCode, ...]
    production_process_monotonic_time_ns: int
    valid_until_process_monotonic_time_ns: int
    forecast_contract_bytes: bytes
    schema_version: str = EARNINGS_SCHEMA_VERSION

    def __post_init__(self) -> None:
        """Validate bounds, timing, provenance, and canonical forecast presence."""
        _valid_digest(self.input_sha256, "earnings result input_sha256")
        if self.revision <= 0 or self.schema_version != EARNINGS_SCHEMA_VERSION:
            msg = "earnings result revision or schema version is invalid"
            raise ValueError(msg)
        for value, name in (
            (self.materiality_ppm, "materiality_ppm"),
            (self.uncertainty_ppm, "uncertainty_ppm"),
        ):
            if not 0 <= value <= PPM:
                msg = f"{name} is outside [0, 1,000,000] PPM"
                raise ValueError(msg)
        if abs(self.expected_return_ppm) > MAX_ABSOLUTE_RETURN_PPM:
            msg = "expected return exceeds its PPM bound"
            raise ValueError(msg)
        if not 0 <= self.expected_volatility_ppm <= MAX_VOLATILITY_PPM:
            msg = "expected volatility exceeds its PPM bound"
            raise ValueError(msg)
        if self.estimated_price_discovery_duration_ns <= 0:
            msg = "price-discovery duration must be positive"
            raise ValueError(msg)
        if (
            self.production_process_monotonic_time_ns <= 0
            or self.valid_until_process_monotonic_time_ns
            <= self.production_process_monotonic_time_ns
        ):
            msg = "earnings result monotonic lifetime is invalid"
            raise ValueError(msg)
        if self.forecast_contract_bytes[4:8] != b"AMCR":
            msg = "earnings result lacks a canonical ModelForecast contract"
            raise ValueError(msg)
        if len(set(self.reason_codes)) != len(self.reason_codes) or len(
            set(self.accounting_quality_flags)
        ) != len(self.accounting_quality_flags):
            msg = "earnings result reasons and quality flags must be unique"
            raise ValueError(msg)

    def canonical_bytes(self) -> bytes:
        """Return deterministic detailed-result JSON for replay comparison."""
        return _canonical_json_bytes(self)

    @property
    def sha256(self) -> bytes:
        """Hash the complete specialist result including forecast bytes."""
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
    msg = f"unsupported canonical earnings value: {type(value).__name__}"
    raise TypeError(msg)


def _canonical_json_bytes(value: object) -> bytes:
    return json.dumps(
        _canonical_value(value),
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")
