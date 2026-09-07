"""Bounded fast/deep news and filing intelligence orchestration."""

from __future__ import annotations

import hashlib
import json
from collections import OrderedDict, deque
from dataclasses import dataclass, fields, replace
from typing import Protocol

from aegis_mx_intelligence.build_info import VERSION
from aegis_mx_intelligence.contracts import (
    ChannelId,
    ConfigurationVersion,
    GlobalEventId,
    ModelId,
    ModelVersion,
    SessionId,
)
from aegis_mx_intelligence.news_analysis import EntityRegistry, NoveltyIndex, RuleEngine
from aegis_mx_intelligence.news_contract import build_event_intelligence_contract
from aegis_mx_intelligence.news_providers import DocumentProvider, ProviderError
from aegis_mx_intelligence.news_sandbox import (
    DocumentSanitizer,
    ProcessDocumentSanitizer,
)
from aegis_mx_intelligence.news_security import (
    UnsafeDocumentError,
    detect_document_type,
)
from aegis_mx_intelligence.news_types import (
    Adjudication,
    Classification,
    DeepAdjudication,
    DocumentType,
    IngestStatus,
    IntelligenceAlert,
    IntelligenceStage,
    PipelineMetrics,
    PipelineServiceStatus,
    ProviderHealth,
    ResolvedEntity,
    SanitizedDocument,
    SourceDocument,
    derived_identifier,
)

MAX_POLICY_ID_BYTES = 128


class MonotonicClock(Protocol):
    """Injectable nanosecond monotonic clock."""

    def __call__(self) -> int:
        """Return a positive monotonic timestamp."""


@dataclass(frozen=True, slots=True)
class IntelligencePipelineConfig:
    """Immutable bounded pipeline configuration without credentials."""

    session_id: SessionId
    source_channel_id: ChannelId
    configuration_version: ConfigurationVersion
    fast_model_id: ModelId
    fast_model_version: ModelVersion
    deep_model_id: ModelId
    deep_model_version: ModelVersion
    publication_capacity: int = 1_024
    deduplication_capacity: int = 8_192
    deep_queue_capacity: int = 256
    deep_deadline_ns: int = 5_000_000_000
    provider_poll_batch: int = 32
    deep_policy_id: str = "aegis-intelligence-adjudication-v1"

    def __post_init__(self) -> None:
        """Validate every capacity, deadline, and policy identifier."""
        values = (
            self.publication_capacity,
            self.deduplication_capacity,
            self.deep_queue_capacity,
            self.deep_deadline_ns,
            self.provider_poll_batch,
        )
        if any(value <= 0 for value in values):
            msg = "pipeline capacities and deadlines must be positive"
            raise ValueError(msg)
        if (
            not self.deep_policy_id
            or len(self.deep_policy_id.encode("utf-8")) > MAX_POLICY_ID_BYTES
        ):
            msg = "deep policy identifier is invalid"
            raise ValueError(msg)

    def sha256(self) -> str:
        """Return a canonical secret-free configuration hash."""
        payload = {
            "configuration_version": self.configuration_version.hex(),
            "deduplication_capacity": self.deduplication_capacity,
            "deep_deadline_ns": self.deep_deadline_ns,
            "deep_model_id": self.deep_model_id.hex(),
            "deep_model_version": self.deep_model_version.hex(),
            "deep_policy_id": self.deep_policy_id,
            "deep_queue_capacity": self.deep_queue_capacity,
            "fast_model_id": self.fast_model_id.hex(),
            "fast_model_version": self.fast_model_version.hex(),
            "provider_poll_batch": self.provider_poll_batch,
            "publication_capacity": self.publication_capacity,
            "session_id": self.session_id.hex(),
            "source_channel_id": self.source_channel_id.hex(),
        }
        encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
        return hashlib.sha256(encoded).hexdigest()


class IntelligencePublisher(Protocol):
    """One-way structured publication boundary with no OMS methods."""

    def publish(self, alert: IntelligenceAlert) -> bool:
        """Publish immutably or reject due to bounded capacity."""


class BoundedInMemoryPublisher:
    """Deterministic publisher used by replay and integration tests."""

    def __init__(self, capacity: int) -> None:
        """Create a fixed-capacity immutable publication history."""
        if capacity <= 0:
            msg = "publisher capacity must be positive"
            raise ValueError(msg)
        self._capacity = capacity
        self._alerts: list[IntelligenceAlert] = []

    @property
    def alerts(self) -> tuple[IntelligenceAlert, ...]:
        """Return immutable publication history; records are never overwritten."""
        return tuple(self._alerts)

    def publish(self, alert: IntelligenceAlert) -> bool:
        """Append one immutable alert if capacity permits."""
        if len(self._alerts) >= self._capacity:
            return False
        self._alerts.append(alert)
        return True


@dataclass(frozen=True, slots=True)
class DeepAdjudicationRequest:
    """Tool-less deep-stage request with source text explicitly marked untrusted."""

    policy_id: str
    untrusted_document_text: str
    fast_alert: IntelligenceAlert
    deadline_monotonic_time_ns: int


class DeepAdjudicator(Protocol):
    """Replaceable deep-language-model boundary; it receives no tool capability."""

    def adjudicate(self, request: DeepAdjudicationRequest) -> DeepAdjudication:
        """Return schema-constrained output without side effects."""


class DeterministicDeepAdjudicator:
    """Infrastructure-only adjudicator that confirms validated fast output."""

    def adjudicate(self, request: DeepAdjudicationRequest) -> DeepAdjudication:
        """Confirm the fast classification without interpreting instructions."""
        return DeepAdjudication(
            parent_fast_event_id=request.fast_alert.global_event_id,
            source_content_sha256=request.fast_alert.document.source.content_sha256,
            classification=request.fast_alert.classification,
            adjudication=Adjudication.CONFIRMED,
        )


@dataclass(frozen=True, slots=True)
class AuditLogEntry:
    """Bounded structured audit entry that never contains document text."""

    code: str
    content_sha256_hex: str
    event_id_hex: str
    processed_monotonic_time_ns: int


class IntelligencePipeline:
    """Near-real-time deterministic fast stage plus bounded asynchronous deep stage."""

    def __init__(
        self,
        config: IntelligencePipelineConfig,
        provider: DocumentProvider,
        registry: EntityRegistry,
        publisher: IntelligencePublisher,
        deep_adjudicator: DeepAdjudicator,
        clock: MonotonicClock,
        rule_engine: RuleEngine | None = None,
        sanitizer: DocumentSanitizer | None = None,
    ) -> None:
        """Bind immutable configuration and least-privilege dependencies."""
        self._config = config
        self._provider = provider
        self._registry = registry
        self._publisher = publisher
        self._deep = deep_adjudicator
        self._clock = clock
        self._rules = rule_engine or RuleEngine()
        self._sanitizer = sanitizer or ProcessDocumentSanitizer()
        self._novelty = NoveltyIndex(config.deduplication_capacity)
        self._seen_hashes: OrderedDict[bytes, None] = OrderedDict()
        self._document_revisions: OrderedDict[tuple[str, str], IntelligenceAlert] = (
            OrderedDict()
        )
        self._fact_index: OrderedDict[tuple[bytes, str], tuple[str, GlobalEventId]] = (
            OrderedDict()
        )
        self._deep_queue: deque[tuple[IntelligenceAlert, int]] = deque()
        self._audit_log: deque[AuditLogEntry] = deque(
            maxlen=config.publication_capacity
        )
        self._metrics = PipelineMetrics()
        self._stopped = False

    @property
    def metrics(self) -> PipelineMetrics:
        """Return fixed-cardinality counters."""
        return self._metrics

    @property
    def audit_log(self) -> tuple[AuditLogEntry, ...]:
        """Return structured entries without source text."""
        return tuple(self._audit_log)

    def _audit(
        self, code: str, document: SourceDocument, event_id: GlobalEventId | None
    ) -> None:
        self._audit_log.append(
            AuditLogEntry(
                code,
                document.content_sha256.hex(),
                "" if event_id is None else event_id.hex(),
                self._clock(),
            )
        )

    def _remember_hash(self, digest: bytes) -> None:
        self._seen_hashes.pop(digest, None)
        while len(self._seen_hashes) >= self._config.deduplication_capacity:
            self._seen_hashes.popitem(last=False)
        self._seen_hashes[digest] = None

    def _contradictions(
        self, document: SanitizedDocument, classification: Classification
    ) -> tuple[GlobalEventId, ...]:
        primary = self._registry.resolve(document.analysis_text)[0]
        identifiers = []
        for fact in classification.facts:
            prior = self._fact_index.get((primary.entity_sha256, fact.name))
            if prior is not None and prior[0] != fact.value:
                identifiers.append(prior[1])
        return tuple(dict.fromkeys(identifiers))

    def _record_facts(self, alert: IntelligenceAlert) -> None:
        primary = alert.entities[0]
        for fact in alert.classification.facts:
            key = (primary.entity_sha256, fact.name)
            self._fact_index.pop(key, None)
            while len(self._fact_index) >= self._config.deduplication_capacity:
                self._fact_index.popitem(last=False)
            self._fact_index[key] = (fact.value, alert.global_event_id)

    def _create_fast_alert(
        self,
        document: SanitizedDocument,
        document_type: DocumentType,
        classification: Classification,
        entities: tuple[ResolvedEntity, ...],
    ) -> IntelligenceAlert:
        key = (document.source.provider_id, document.source.document_id)
        prior = self._document_revisions.get(key)
        revision = 1 if prior is None else prior.revision + 1
        event_id = GlobalEventId(
            derived_identifier(
                "news-fast-alert",
                document.source.provider_id,
                document.source.document_id,
                document.source.content_sha256,
                revision,
            )
        )
        correction = (
            prior.global_event_id
            if prior is not None and document_type is DocumentType.CORRECTION
            else None
        )
        contradictions = self._contradictions(document, classification)
        alert = IntelligenceAlert(
            global_event_id=event_id,
            session_id=self._config.session_id,
            source_channel_id=self._config.source_channel_id,
            configuration_version=self._config.configuration_version,
            model_id=self._config.fast_model_id,
            model_version=self._config.fast_model_version,
            document=document,
            document_type=document_type,
            stage=IntelligenceStage.FAST,
            adjudication=(
                Adjudication.CORRECTED
                if document_type is DocumentType.CORRECTION
                else Adjudication.PRELIMINARY
            ),
            revision=revision,
            entities=entities,
            classification=classification,
            processed_monotonic_time_ns=self._clock(),
            correction_of_event_id=correction,
            contradicts_event_ids=contradictions,
        )
        return replace(alert, contract_bytes=build_event_intelligence_contract(alert))

    def ingest(self, source: SourceDocument) -> IngestStatus:
        """Run all fast stages and publish before enqueuing optional deep work."""
        if self._stopped:
            return IngestStatus.STOPPED
        self._metrics.received_total += 1
        if not source.authentication.verified:
            self._metrics.rejected_total += 1
            self._audit("authentication_rejected", source, None)
            return IngestStatus.REJECTED
        try:
            document = self._sanitizer.sanitize(source)
        except UnsafeDocumentError:
            self._metrics.rejected_total += 1
            self._audit("document_rejected", source, None)
            return IngestStatus.REJECTED
        if source.content_sha256 in self._seen_hashes:
            self._metrics.duplicates_total += 1
            self._audit("duplicate", source, None)
            return IngestStatus.DUPLICATE
        if document.prompt_injection_detected:
            self._metrics.injection_detected_total += 1

        entities = self._registry.resolve(document.analysis_text)
        if not entities:
            self._remember_hash(source.content_sha256)
            self._metrics.unresolved_total += 1
            self._audit("unresolved", source, None)
            return IngestStatus.UNRESOLVED
        novelty = self._novelty.score(document.analysis_text)
        classification = self._rules.classify(document, novelty)
        if classification is None:
            self._remember_hash(source.content_sha256)
            self._metrics.rejected_total += 1
            self._audit("unclassified", source, None)
            return IngestStatus.REJECTED

        document_type = detect_document_type(document)
        alert = self._create_fast_alert(
            document, document_type, classification, entities
        )
        if not self._publisher.publish(alert):
            self._metrics.queue_rejections_total += 1
            self._audit("publication_full", source, alert.global_event_id)
            return IngestStatus.QUEUE_FULL

        self._remember_hash(source.content_sha256)
        self._novelty.record(source.content_sha256, document.analysis_text)
        key = (source.provider_id, source.document_id)
        self._document_revisions.pop(key, None)
        while len(self._document_revisions) >= self._config.deduplication_capacity:
            self._document_revisions.popitem(last=False)
        self._document_revisions[key] = alert
        self._record_facts(alert)
        self._metrics.published_fast_total += 1
        self._metrics.contradictions_total += len(alert.contradicts_event_ids)
        if alert.correction_of_event_id is not None:
            self._metrics.corrections_total += 1
        self._audit("fast_published", source, alert.global_event_id)
        if len(self._deep_queue) >= self._config.deep_queue_capacity:
            self._metrics.queue_rejections_total += 1
        else:
            deadline = self._clock() + self._config.deep_deadline_ns
            self._deep_queue.append((alert, deadline))
        return IngestStatus.PUBLISHED

    def poll_provider_once(self) -> tuple[IngestStatus, ...]:
        """Poll a bounded provider batch; provider errors fail readiness closed."""
        if self._stopped:
            return ()
        try:
            documents = self._provider.poll(self._config.provider_poll_batch)
        except ProviderError:
            return (IngestStatus.REJECTED,)
        return tuple(self.ingest(document) for document in documents)

    @staticmethod
    def _validate_deep_output(
        fast: IntelligenceAlert, output: DeepAdjudication
    ) -> None:
        if output.parent_fast_event_id != fast.global_event_id:
            msg = "deep output parent does not match fast alert"
            raise ValueError(msg)
        if output.source_content_sha256 != fast.document.source.content_sha256:
            msg = "deep output source provenance does not match"
            raise ValueError(msg)
        retained = fast.document.retained_text
        if any(
            item.end > len(retained)
            or retained[item.start : item.end] != item.exact_text
            for item in output.classification.evidence
        ):
            msg = "deep output evidence is not an exact source excerpt"
            raise ValueError(msg)

    def _create_deep_alert(
        self, fast: IntelligenceAlert, output: DeepAdjudication
    ) -> IntelligenceAlert:
        changed = (
            output.classification.event_type is not fast.classification.event_type
            or output.adjudication is Adjudication.DISPUTED
        )
        contradictions = (
            *fast.contradicts_event_ids,
            *((fast.global_event_id,) if changed else ()),
        )
        event_id = GlobalEventId(
            derived_identifier(
                "news-deep-alert",
                fast.global_event_id.hex(),
                self._config.deep_model_version.hex(),
                int(output.adjudication),
            )
        )
        alert = IntelligenceAlert(
            global_event_id=event_id,
            session_id=fast.session_id,
            source_channel_id=fast.source_channel_id,
            configuration_version=fast.configuration_version,
            model_id=self._config.deep_model_id,
            model_version=self._config.deep_model_version,
            document=fast.document,
            document_type=fast.document_type,
            stage=IntelligenceStage.DEEP,
            adjudication=output.adjudication,
            revision=fast.revision,
            entities=fast.entities,
            classification=output.classification,
            processed_monotonic_time_ns=self._clock(),
            parent_fast_event_id=fast.global_event_id,
            correction_of_event_id=fast.correction_of_event_id,
            contradicts_event_ids=tuple(dict.fromkeys(contradictions)),
        )
        return replace(alert, contract_bytes=build_event_intelligence_contract(alert))

    def run_deep_once(self, maximum_documents: int = 1) -> int:
        """Process a bounded deep batch; failures never erase fast publications."""
        if maximum_documents <= 0:
            msg = "maximum_documents must be positive"
            raise ValueError(msg)
        processed = 0
        for _ in range(min(maximum_documents, len(self._deep_queue))):
            fast, deadline = self._deep_queue.popleft()
            processed += 1
            if self._clock() > deadline:
                self._metrics.deep_deadline_misses_total += 1
                self._audit(
                    "deep_deadline_missed", fast.document.source, fast.global_event_id
                )
                continue
            request = DeepAdjudicationRequest(
                policy_id=self._config.deep_policy_id,
                untrusted_document_text=fast.document.analysis_text,
                fast_alert=fast,
                deadline_monotonic_time_ns=deadline,
            )
            try:
                output = self._deep.adjudicate(request)
                self._validate_deep_output(fast, output)
                if self._clock() > deadline:
                    self._metrics.deep_deadline_misses_total += 1
                    self._audit(
                        "deep_late_discarded",
                        fast.document.source,
                        fast.global_event_id,
                    )
                    continue
                deep_alert = self._create_deep_alert(fast, output)
            except (TypeError, ValueError, RuntimeError):
                self._metrics.deep_failures_total += 1
                self._audit("deep_rejected", fast.document.source, fast.global_event_id)
                continue
            if not self._publisher.publish(deep_alert):
                self._metrics.queue_rejections_total += 1
                self._audit(
                    "publication_full", fast.document.source, deep_alert.global_event_id
                )
                continue
            self._metrics.published_deep_total += 1
            self._metrics.contradictions_total += max(
                0,
                len(deep_alert.contradicts_event_ids) - len(fast.contradicts_event_ids),
            )
            self._audit(
                "deep_published", fast.document.source, deep_alert.global_event_id
            )
        return processed

    def status(self) -> PipelineServiceStatus:
        """Return health/readiness/version/configuration information."""
        health = self._provider.health
        healthy = health not in {ProviderHealth.INVALID, ProviderHealth.STOPPED}
        return PipelineServiceStatus(
            healthy=healthy,
            ready=healthy and not self._stopped,
            version=VERSION,
            configuration_sha256=self._config.sha256(),
            provider_health=health,
            stopped=self._stopped,
        )

    def prometheus_metrics(self) -> str:
        """Render fixed-cardinality metrics without provider/document labels."""
        return "".join(
            f"intelligence_{name} {value}\n"
            for item in fields(self._metrics)
            for name, value in ((item.name, getattr(self._metrics, item.name)),)
        )

    def shutdown(self, deadline_monotonic_time_ns: int) -> bool:
        """Stop admission and drain deep work only within the supplied deadline."""
        self._stopped = True
        while self._deep_queue and self._clock() <= deadline_monotonic_time_ns:
            self.run_deep_once()
        drained = not self._deep_queue
        self._deep_queue.clear()
        self._provider.close()
        return drained
