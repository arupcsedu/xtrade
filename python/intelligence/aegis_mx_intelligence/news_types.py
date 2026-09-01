"""Strict immutable types for untrusted news and filing intelligence."""

from __future__ import annotations

import hashlib
from dataclasses import dataclass, field
from enum import IntEnum
from typing import Final

from aegis_mx_intelligence.contracts import (
    ChannelId,
    ConfigurationVersion,
    GlobalEventId,
    Identifier128,
    InstrumentId,
    ModelId,
    ModelVersion,
    SessionId,
)

MAX_DOCUMENT_BYTES: Final = 1 << 20
MAX_TEXT_CHARACTERS: Final = 250_000
MAX_IDENTIFIER_TEXT: Final = 128
MAX_EXCERPT_CHARACTERS: Final = 512
MAX_FACTS: Final = 32
MAX_ENTITIES: Final = 32
MAX_EVIDENCE: Final = 64
SHA256_BYTES: Final = 32
MAX_ALIASES: Final = 32
MAX_TICKERS: Final = 16
MAX_FACT_EVIDENCE: Final = 8
PPM: Final = 1_000_000


class SourceAuthentication(IntEnum):
    """Authentication outcome supplied by a provider boundary."""

    INVALID = 0
    MOCK_VERIFIED = 1
    REPLAY_HASH_VERIFIED = 2
    PUBLIC_TLS_VERIFIED = 3
    SIGNATURE_VERIFIED = 4


class DocumentType(IntEnum):
    """Supported normalized source-document types."""

    NEWS_ARTICLE = 1
    FILING = 2
    PRESS_RELEASE = 3
    REGULATORY_NOTICE = 4
    MACRO_RELEASE = 5
    CORRECTION = 6


class EventType(IntEnum):
    """Supported structured intelligence event types."""

    EARNINGS_RELEASE = 1
    GUIDANCE_UPDATE = 2
    MERGER_ACQUISITION = 3
    PRODUCT_RECALL = 4
    REGULATORY_ACTION = 5
    EXECUTIVE_CHANGE = 6
    FINANCING = 7
    BANKRUPTCY = 8
    LITIGATION = 9
    CYBERSECURITY_INCIDENT = 10
    ANALYST_ACTION = 11
    SUPPLY_CHAIN_DISRUPTION = 12
    TRADING_HALT = 13
    MACRO_RELEASE = 14
    RUMOR = 15
    CORRECTION = 16


class IntelligenceStage(IntEnum):
    """Immutable publication stage."""

    FAST = 1
    DEEP = 2


class Adjudication(IntEnum):
    """How a publication relates to the current evidence."""

    PRELIMINARY = 1
    CONFIRMED = 2
    REFINED = 3
    DISPUTED = 4
    CORRECTED = 5


class ProviderHealth(IntEnum):
    """Provider lifecycle state."""

    STARTING = 1
    HEALTHY = 2
    DEGRADED = 3
    INVALID = 4
    STOPPED = 5


class IngestStatus(IntEnum):
    """Bounded pipeline result for one source document."""

    PUBLISHED = 1
    DUPLICATE = 2
    REJECTED = 3
    UNRESOLVED = 4
    QUEUE_FULL = 5
    STOPPED = 6


def _bounded_text(value: str, name: str, maximum: int = MAX_IDENTIFIER_TEXT) -> None:
    if not value or len(value.encode("utf-8")) > maximum or "\x00" in value:
        msg = f"{name} is empty, contains NUL, or exceeds {maximum} UTF-8 bytes"
        raise ValueError(msg)


def _score(value: int, name: str) -> None:
    if not 0 <= value <= PPM:
        msg = f"{name} must be within [0, 1_000_000] PPM"
        raise ValueError(msg)


def sha256_bytes(content: bytes | str) -> bytes:
    """Return SHA-256 for bytes or UTF-8 text."""
    encoded = content.encode("utf-8") if isinstance(content, str) else content
    return hashlib.sha256(encoded).digest()


def derived_identifier(namespace: str, *parts: bytes | str | int) -> Identifier128:
    """Derive a stable nonzero identifier with length-delimited framing."""
    digest = hashlib.sha256(namespace.encode("ascii"))
    for part in parts:
        if isinstance(part, int):
            if part < 0:
                msg = "identifier integer parts must be nonnegative"
                raise ValueError(msg)
            encoded = part.to_bytes(16, "big")
        elif isinstance(part, str):
            encoded = part.encode("utf-8")
        else:
            encoded = part
        digest.update(len(encoded).to_bytes(4, "big"))
        digest.update(encoded)
    raw = digest.digest()[:16]
    high = int.from_bytes(raw[:8], "big")
    low = int.from_bytes(raw[8:], "big")
    return Identifier128(high or 1, low)


@dataclass(frozen=True, slots=True)
class AuthenticationEvidence:
    """Provider authentication evidence without credentials or raw signatures."""

    status: SourceAuthentication
    method: str
    evidence_sha256: bytes

    def __post_init__(self) -> None:
        """Validate bounded authentication evidence."""
        _bounded_text(self.method, "authentication method", 64)
        if len(self.evidence_sha256) != SHA256_BYTES:
            msg = "authentication evidence must be a SHA-256 digest"
            raise ValueError(msg)

    @property
    def verified(self) -> bool:
        """Return whether the provider boundary established source authenticity."""
        return self.status is not SourceAuthentication.INVALID and any(
            self.evidence_sha256
        )


@dataclass(frozen=True, slots=True)
class SourceDocument:
    """Bounded provider-neutral input; payload bytes are always untrusted."""

    provider_id: str
    document_id: str
    source_uri: str
    content_type: str
    payload: bytes
    provider_event_time_utc_ns: int
    received_wall_clock_utc_ns: int
    authentication: AuthenticationEvidence

    def __post_init__(self) -> None:
        """Validate the provider-neutral source envelope."""
        _bounded_text(self.provider_id, "provider_id", 64)
        _bounded_text(self.document_id, "document_id")
        _bounded_text(self.source_uri, "source_uri", 512)
        _bounded_text(self.content_type, "content_type", 128)
        if not self.payload or len(self.payload) > MAX_DOCUMENT_BYTES:
            msg = "document payload is empty or exceeds the byte limit"
            raise ValueError(msg)
        if self.provider_event_time_utc_ns <= 0 or self.received_wall_clock_utc_ns <= 0:
            msg = "provider and receipt UTC timestamps must be positive"
            raise ValueError(msg)
        if self.received_wall_clock_utc_ns < self.provider_event_time_utc_ns:
            msg = "receipt UTC timestamp precedes provider event time"
            raise ValueError(msg)

    @property
    def content_sha256(self) -> bytes:
        """Hash the exact untrusted payload bytes."""
        return sha256_bytes(self.payload)


@dataclass(frozen=True, slots=True)
class SanitizedDocument:
    """Plain-text analysis view separated from retained evidence text."""

    source: SourceDocument
    retained_text: str
    analysis_text: str
    sanitized_sha256: bytes
    prompt_injection_detected: bool
    language_code: str

    def __post_init__(self) -> None:
        """Validate sanitized and analysis views."""
        if not self.retained_text or len(self.retained_text) > MAX_TEXT_CHARACTERS:
            msg = "sanitized document text is empty or oversized"
            raise ValueError(msg)
        if not self.analysis_text or len(self.analysis_text) > MAX_TEXT_CHARACTERS:
            msg = "analysis text is empty or oversized"
            raise ValueError(msg)
        if len(self.sanitized_sha256) != SHA256_BYTES:
            msg = "sanitized content digest must contain 32 bytes"
            raise ValueError(msg)
        _bounded_text(self.language_code, "language_code", 16)


@dataclass(frozen=True, slots=True)
class EntityDefinition:
    """Point-in-time entity master entry and bounded relationship edges."""

    canonical_name: str
    aliases: tuple[str, ...]
    tickers: tuple[str, ...]
    instrument_id: InstrumentId
    relationships: tuple[tuple[str, str], ...] = ()

    def __post_init__(self) -> None:
        """Validate registry aliases, tickers, and relationship bounds."""
        _bounded_text(self.canonical_name, "canonical_name")
        if (
            not self.aliases
            or len(self.aliases) > MAX_ALIASES
            or len(self.tickers) > MAX_TICKERS
        ):
            msg = "entity aliases/tickers are empty or exceed configured bounds"
            raise ValueError(msg)
        for item in (*self.aliases, *self.tickers):
            _bounded_text(item, "entity alias/ticker", 64)
        for relation, target in self.relationships:
            _bounded_text(relation, "relationship", 64)
            _bounded_text(target, "relationship target")

    @property
    def entity_sha256(self) -> bytes:
        """Return a stable entity identity independent of a ticker spelling."""
        return sha256_bytes(self.canonical_name.casefold())


@dataclass(frozen=True, slots=True)
class ResolvedEntity:
    """One registry-backed entity resolution or relationship expansion."""

    entity_sha256: bytes
    canonical_name: str
    relationship: str
    instrument_id: InstrumentId
    confidence_ppm: int

    def __post_init__(self) -> None:
        """Validate a resolved registry-backed entity."""
        if len(self.entity_sha256) != SHA256_BYTES or not any(self.entity_sha256):
            msg = "resolved entity digest must be nonzero SHA-256"
            raise ValueError(msg)
        _bounded_text(self.canonical_name, "resolved canonical name")
        _bounded_text(self.relationship, "resolved relationship", 64)
        _score(self.confidence_ppm, "entity confidence")


@dataclass(frozen=True, slots=True)
class EvidenceExcerpt:
    """Exact excerpt from sanitized retained text with stable offsets and hash."""

    excerpt_id: int
    start: int
    end: int
    exact_text: str

    def __post_init__(self) -> None:
        """Validate evidence identity, offsets, and size."""
        if self.excerpt_id <= 0 or self.start < 0 or self.end <= self.start:
            msg = "evidence identifier and offsets are invalid"
            raise ValueError(msg)
        if len(self.exact_text) > MAX_EXCERPT_CHARACTERS:
            msg = "evidence excerpt exceeds its character bound"
            raise ValueError(msg)

    @property
    def sha256(self) -> bytes:
        """Hash the exact retained excerpt."""
        return sha256_bytes(self.exact_text)


@dataclass(frozen=True, slots=True)
class StructuredFact:
    """Strict evidence-backed fact; values are inert text, never instructions."""

    name: str
    value: str
    unit: str
    uncertainty_ppm: int
    evidence_excerpt_ids: tuple[int, ...]

    def __post_init__(self) -> None:
        """Validate inert fact fields and evidence links."""
        _bounded_text(self.name, "fact name", 64)
        _bounded_text(self.value, "fact value", 256)
        _bounded_text(self.unit, "fact unit", 32)
        _score(self.uncertainty_ppm, "fact uncertainty")
        if (
            not self.evidence_excerpt_ids
            or len(self.evidence_excerpt_ids) > MAX_FACT_EVIDENCE
        ):
            msg = "facts require one to eight evidence excerpts"
            raise ValueError(msg)
        if any(item <= 0 for item in self.evidence_excerpt_ids):
            msg = "fact evidence identifiers must be positive"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class Classification:
    """Validated deterministic or deep-stage structured output."""

    event_type: EventType
    relevance_ppm: int
    novelty_ppm: int
    materiality_ppm: int
    source_trust_ppm: int
    uncertainty_ppm: int
    facts: tuple[StructuredFact, ...] = ()
    evidence: tuple[EvidenceExcerpt, ...] = ()

    def __post_init__(self) -> None:
        """Validate scores, bounds, and fact-to-evidence relationships."""
        for name in (
            "relevance_ppm",
            "novelty_ppm",
            "materiality_ppm",
            "source_trust_ppm",
            "uncertainty_ppm",
        ):
            _score(getattr(self, name), name)
        if len(self.facts) > MAX_FACTS or len(self.evidence) > MAX_EVIDENCE:
            msg = "classification facts or evidence exceed configured bounds"
            raise ValueError(msg)
        evidence_ids = {item.excerpt_id for item in self.evidence}
        if len(evidence_ids) != len(self.evidence):
            msg = "evidence excerpt identifiers must be unique"
            raise ValueError(msg)
        if any(
            excerpt_id not in evidence_ids
            for fact in self.facts
            for excerpt_id in fact.evidence_excerpt_ids
        ):
            msg = "fact references absent evidence"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class DeepAdjudication:
    """Strict output returned by a tool-less deep adjudicator."""

    parent_fast_event_id: GlobalEventId
    source_content_sha256: bytes
    classification: Classification
    adjudication: Adjudication

    def __post_init__(self) -> None:
        """Validate immutable deep-stage provenance and disposition."""
        if len(self.source_content_sha256) != SHA256_BYTES:
            msg = "deep adjudication source digest must contain 32 bytes"
            raise ValueError(msg)
        if self.adjudication is Adjudication.PRELIMINARY:
            msg = "deep adjudication cannot remain preliminary"
            raise ValueError(msg)


@dataclass(frozen=True, slots=True)
class IntelligenceAlert:
    """Immutable normalized publication and its canonical contract bytes."""

    global_event_id: GlobalEventId
    session_id: SessionId
    source_channel_id: ChannelId
    configuration_version: ConfigurationVersion
    model_id: ModelId
    model_version: ModelVersion
    document: SanitizedDocument
    document_type: DocumentType
    stage: IntelligenceStage
    adjudication: Adjudication
    revision: int
    entities: tuple[ResolvedEntity, ...]
    classification: Classification
    processed_monotonic_time_ns: int
    parent_fast_event_id: GlobalEventId | None = None
    correction_of_event_id: GlobalEventId | None = None
    contradicts_event_ids: tuple[GlobalEventId, ...] = ()
    contract_bytes: bytes = field(default=b"", compare=False)

    def __post_init__(self) -> None:
        """Validate alert lineage, revision, and entity resolution."""
        if self.revision <= 0 or self.processed_monotonic_time_ns <= 0:
            msg = "alert revision and processed monotonic time must be positive"
            raise ValueError(msg)
        if not self.entities or len(self.entities) > MAX_ENTITIES:
            msg = "alert requires bounded entity resolution"
            raise ValueError(msg)
        if (
            self.stage is IntelligenceStage.FAST
            and self.parent_fast_event_id is not None
        ):
            msg = "fast alert cannot have a fast parent"
            raise ValueError(msg)
        if self.stage is IntelligenceStage.DEEP and self.parent_fast_event_id is None:
            msg = "deep alert requires its immutable fast parent"
            raise ValueError(msg)


@dataclass(slots=True)
class PipelineMetrics:
    """Fixed-cardinality counters; raw source identifiers are never labels."""

    received_total: int = 0
    published_fast_total: int = 0
    published_deep_total: int = 0
    duplicates_total: int = 0
    rejected_total: int = 0
    unresolved_total: int = 0
    injection_detected_total: int = 0
    contradictions_total: int = 0
    corrections_total: int = 0
    queue_rejections_total: int = 0
    deep_failures_total: int = 0
    deep_deadline_misses_total: int = 0


@dataclass(frozen=True, slots=True)
class PipelineServiceStatus:
    """Common service health/readiness/build/configuration surface."""

    healthy: bool
    ready: bool
    version: str
    configuration_sha256: str
    provider_health: ProviderHealth
    stopped: bool
