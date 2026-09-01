"""Canonical FlatBuffers publication for structured intelligence alerts."""

from __future__ import annotations

from typing import Any, cast

import aegis.mx.contracts.v1.ChannelId as channel_id_wire
import aegis.mx.contracts.v1.ConfigurationVersion as configuration_version_wire
import aegis.mx.contracts.v1.ContractRecord as contract_record_wire
import aegis.mx.contracts.v1.EventIntelligenceRecord as intelligence_wire
import aegis.mx.contracts.v1.ExchangeEventTimeNs as exchange_time_wire
import aegis.mx.contracts.v1.GlobalEventId as global_event_id_wire
import aegis.mx.contracts.v1.InstrumentId as instrument_id_wire
import aegis.mx.contracts.v1.IntelligenceEvidenceExcerpt as evidence_wire
import aegis.mx.contracts.v1.IntelligenceFact as fact_wire
import aegis.mx.contracts.v1.IntelligenceResolvedEntity as entity_wire
import aegis.mx.contracts.v1.ModelId as model_id_wire
import aegis.mx.contracts.v1.ModelVersion as model_version_wire
import aegis.mx.contracts.v1.ProcessMonotonicTimeNs as monotonic_time_wire
import aegis.mx.contracts.v1.SchemaVersion as schema_version_wire
import aegis.mx.contracts.v1.SessionId as session_id_wire
import aegis.mx.contracts.v1.Sha256Digest as digest_wire
import aegis.mx.contracts.v1.WallClockUtcTimeNs as wall_time_wire
import flatbuffers  # type: ignore[import-untyped]
from aegis.mx.contracts.v1.ContractPayload import ContractPayload
from aegis.mx.contracts.v1.DataQualityCode import DataQualityCode
from aegis.mx.contracts.v1.IntelligenceTrustCode import IntelligenceTrustCode
from aegis.mx.contracts.v1.RecordType import RecordType

from aegis_mx_intelligence.contracts import SCHEMA_MAJOR, SCHEMA_MINOR, SCHEMA_PATCH
from aegis_mx_intelligence.news_types import IntelligenceAlert


def _version(builder: flatbuffers.Builder) -> int:
    return cast(
        "int",
        schema_version_wire.CreateSchemaVersion(
            builder, SCHEMA_MAJOR, SCHEMA_MINOR, SCHEMA_PATCH
        ),
    )


def _evidence_offsets(
    builder: flatbuffers.Builder, alert: IntelligenceAlert
) -> list[int]:
    offsets = []
    for item in alert.classification.evidence:
        exact_text = builder.CreateString(item.exact_text)
        evidence_wire.Start(builder)
        digest = digest_wire.CreateSha256Digest(builder, cast("Any", item.sha256))
        evidence_wire.IntelligenceEvidenceExcerptAddExactTextSha256(builder, digest)
        evidence_wire.IntelligenceEvidenceExcerptAddExactText(builder, exact_text)
        evidence_wire.IntelligenceEvidenceExcerptAddSourceEndUtf8Offset(
            builder, item.end
        )
        evidence_wire.IntelligenceEvidenceExcerptAddSourceStartUtf8Offset(
            builder, item.start
        )
        evidence_wire.IntelligenceEvidenceExcerptAddExcerptId(builder, item.excerpt_id)
        offsets.append(cast("int", evidence_wire.End(builder)))
    return offsets


def _fact_offsets(builder: flatbuffers.Builder, alert: IntelligenceAlert) -> list[int]:
    offsets = []
    for item in alert.classification.facts:
        name = builder.CreateString(item.name)
        value = builder.CreateString(item.value)
        unit = builder.CreateString(item.unit)
        fact_wire.StartEvidenceExcerptIdsVector(builder, len(item.evidence_excerpt_ids))
        for excerpt_id in reversed(item.evidence_excerpt_ids):
            builder.PrependUint32(excerpt_id)
        evidence_ids = builder.EndVector()
        fact_wire.Start(builder)
        fact_wire.IntelligenceFactAddEvidenceExcerptIds(builder, evidence_ids)
        fact_wire.IntelligenceFactAddUncertaintyPpm(builder, item.uncertainty_ppm)
        fact_wire.IntelligenceFactAddUnit(builder, unit)
        fact_wire.IntelligenceFactAddValue(builder, value)
        fact_wire.IntelligenceFactAddName(builder, name)
        offsets.append(cast("int", fact_wire.End(builder)))
    return offsets


def _entity_offsets(
    builder: flatbuffers.Builder, alert: IntelligenceAlert
) -> list[int]:
    offsets = []
    for item in alert.entities:
        canonical_name = builder.CreateString(item.canonical_name)
        relationship = builder.CreateString(item.relationship)
        entity_wire.Start(builder)
        entity_wire.IntelligenceResolvedEntityAddConfidencePpm(
            builder, item.confidence_ppm
        )
        instrument = instrument_id_wire.CreateInstrumentId(
            builder, item.instrument_id.high, item.instrument_id.low
        )
        entity_wire.IntelligenceResolvedEntityAddInstrumentId(builder, instrument)
        entity_wire.IntelligenceResolvedEntityAddRelationship(builder, relationship)
        entity_wire.IntelligenceResolvedEntityAddCanonicalName(builder, canonical_name)
        digest = digest_wire.CreateSha256Digest(
            builder, cast("Any", item.entity_sha256)
        )
        entity_wire.IntelligenceResolvedEntityAddEntitySha256(builder, digest)
        offsets.append(cast("int", entity_wire.End(builder)))
    return offsets


def _table_vector(
    builder: flatbuffers.Builder,
    offsets: list[int],
    start: Any,
) -> int:
    start(builder, len(offsets))
    for offset in reversed(offsets):
        builder.PrependUOffsetTRelative(offset)
    return cast("int", builder.EndVector())


def build_event_intelligence_contract(alert: IntelligenceAlert) -> bytes:
    """Serialize one v1.4 immutable, evidence-backed intelligence contract."""
    source = alert.document.source
    classification = alert.classification
    evidence_ids = {item.excerpt_id for item in classification.evidence}
    if any(
        item.end > len(alert.document.retained_text)
        or alert.document.retained_text[item.start : item.end] != item.exact_text
        for item in classification.evidence
    ):
        msg = "evidence offsets do not identify exact retained text"
        raise ValueError(msg)
    if any(
        excerpt_id not in evidence_ids
        for item in classification.facts
        for excerpt_id in item.evidence_excerpt_ids
    ):
        msg = "fact evidence linkage is invalid"
        raise ValueError(msg)

    builder = flatbuffers.Builder(4_096)
    language = builder.CreateString(alert.document.language_code)
    provider = builder.CreateString(source.provider_id)
    document_id = builder.CreateString(source.document_id)
    evidence_offsets = _evidence_offsets(builder, alert)
    fact_offsets = _fact_offsets(builder, alert)
    entity_offsets = _entity_offsets(builder, alert)
    evidence_vector = _table_vector(
        builder, evidence_offsets, intelligence_wire.StartEvidenceVector
    )
    fact_vector = _table_vector(
        builder, fact_offsets, intelligence_wire.StartFactsVector
    )
    entity_vector = _table_vector(
        builder, entity_offsets, intelligence_wire.StartEntitiesVector
    )
    intelligence_wire.StartContradictsGlobalEventIdsVector(
        builder, len(alert.contradicts_event_ids)
    )
    for identifier in reversed(alert.contradicts_event_ids):
        global_event_id_wire.CreateGlobalEventId(
            builder, identifier.high, identifier.low
        )
    contradictions = builder.EndVector()

    intelligence_wire.Start(builder)
    intelligence_wire.EventIntelligenceRecordAddContradictsGlobalEventIds(
        builder, contradictions
    )
    intelligence_wire.EventIntelligenceRecordAddEvidence(builder, evidence_vector)
    intelligence_wire.EventIntelligenceRecordAddFacts(builder, fact_vector)
    intelligence_wire.EventIntelligenceRecordAddEntities(builder, entity_vector)
    intelligence_wire.EventIntelligenceRecordAddPromptInjectionDetected(
        builder, alert.document.prompt_injection_detected
    )
    intelligence_wire.EventIntelligenceRecordAddUncertaintyPpm(
        builder, classification.uncertainty_ppm
    )
    intelligence_wire.EventIntelligenceRecordAddSourceTrustPpm(
        builder, classification.source_trust_ppm
    )
    intelligence_wire.EventIntelligenceRecordAddMaterialityPpm(
        builder, classification.materiality_ppm
    )
    intelligence_wire.EventIntelligenceRecordAddNoveltyPpm(
        builder, classification.novelty_ppm
    )
    authentication_digest = digest_wire.CreateSha256Digest(
        builder, cast("Any", source.authentication.evidence_sha256)
    )
    intelligence_wire.EventIntelligenceRecordAddSourceAuthenticationSha256(
        builder, authentication_digest
    )
    intelligence_wire.EventIntelligenceRecordAddSourceDocumentId(builder, document_id)
    intelligence_wire.EventIntelligenceRecordAddSourceProviderId(builder, provider)
    intelligence_wire.EventIntelligenceRecordAddLanguageCode(builder, language)
    if alert.correction_of_event_id is not None:
        correction = global_event_id_wire.CreateGlobalEventId(
            builder,
            alert.correction_of_event_id.high,
            alert.correction_of_event_id.low,
        )
        intelligence_wire.EventIntelligenceRecordAddCorrectionOfGlobalEventId(
            builder, correction
        )
    if alert.parent_fast_event_id is not None:
        parent = global_event_id_wire.CreateGlobalEventId(
            builder, alert.parent_fast_event_id.high, alert.parent_fast_event_id.low
        )
        intelligence_wire.EventIntelligenceRecordAddParentFastGlobalEventId(
            builder, parent
        )
    intelligence_wire.EventIntelligenceRecordAddRevision(builder, alert.revision)
    intelligence_wire.EventIntelligenceRecordAddAdjudication(
        builder, cast("Any", int(alert.adjudication))
    )
    intelligence_wire.EventIntelligenceRecordAddStage(
        builder, cast("Any", int(alert.stage))
    )
    intelligence_wire.EventIntelligenceRecordAddEventType(
        builder, cast("Any", int(classification.event_type))
    )
    intelligence_wire.EventIntelligenceRecordAddDocumentType(
        builder, cast("Any", int(alert.document_type))
    )
    sanitized_digest = digest_wire.CreateSha256Digest(
        builder, cast("Any", alert.document.sanitized_sha256)
    )
    intelligence_wire.EventIntelligenceRecordAddSanitizedContentSha256(
        builder, sanitized_digest
    )
    source_digest = digest_wire.CreateSha256Digest(
        builder, cast("Any", source.content_sha256)
    )
    intelligence_wire.EventIntelligenceRecordAddUntrustedSourceContentSha256(
        builder, source_digest
    )
    intelligence_wire.EventIntelligenceRecordAddClassificationCode(
        builder, int(classification.event_type)
    )
    intelligence_wire.EventIntelligenceRecordAddRelevancePpm(
        builder, classification.relevance_ppm
    )
    processed = monotonic_time_wire.CreateProcessMonotonicTimeNs(
        builder, alert.processed_monotonic_time_ns
    )
    intelligence_wire.EventIntelligenceRecordAddProcessedMonotonicTime(
        builder, processed
    )
    received = wall_time_wire.CreateWallClockUtcTimeNs(
        builder, source.received_wall_clock_utc_ns
    )
    intelligence_wire.EventIntelligenceRecordAddReceivedWallClockUtcTime(
        builder, received
    )
    provider_time = exchange_time_wire.CreateExchangeEventTimeNs(
        builder, source.provider_event_time_utc_ns
    )
    intelligence_wire.EventIntelligenceRecordAddProviderEventTime(
        builder, provider_time
    )
    intelligence_wire.EventIntelligenceRecordAddTextTrust(
        builder, IntelligenceTrustCode.SANITIZED
    )
    intelligence_wire.EventIntelligenceRecordAddTrust(
        builder,
        DataQualityCode.DEGRADED
        if alert.document.prompt_injection_detected
        else DataQualityCode.VALID,
    )
    configuration = configuration_version_wire.CreateConfigurationVersion(
        builder,
        alert.configuration_version.high,
        alert.configuration_version.low,
    )
    intelligence_wire.EventIntelligenceRecordAddConfigurationVersion(
        builder, configuration
    )
    model_version = model_version_wire.CreateModelVersion(
        builder, alert.model_version.high, alert.model_version.low
    )
    intelligence_wire.EventIntelligenceRecordAddModelVersion(builder, model_version)
    model_id = model_id_wire.CreateModelId(
        builder, alert.model_id.high, alert.model_id.low
    )
    intelligence_wire.EventIntelligenceRecordAddModelId(builder, model_id)
    primary = alert.entities[0].instrument_id
    instrument = instrument_id_wire.CreateInstrumentId(
        builder, primary.high, primary.low
    )
    intelligence_wire.EventIntelligenceRecordAddInstrumentId(builder, instrument)
    channel = channel_id_wire.CreateChannelId(
        builder, alert.source_channel_id.high, alert.source_channel_id.low
    )
    intelligence_wire.EventIntelligenceRecordAddSourceChannelId(builder, channel)
    session = session_id_wire.CreateSessionId(
        builder, alert.session_id.high, alert.session_id.low
    )
    intelligence_wire.EventIntelligenceRecordAddSessionId(builder, session)
    event_id = global_event_id_wire.CreateGlobalEventId(
        builder, alert.global_event_id.high, alert.global_event_id.low
    )
    intelligence_wire.EventIntelligenceRecordAddGlobalEventId(builder, event_id)
    intelligence_wire.EventIntelligenceRecordAddSchemaVersion(
        builder, _version(builder)
    )
    record = intelligence_wire.End(builder)

    contract_record_wire.Start(builder)
    contract_record_wire.ContractRecordAddPayload(builder, record)
    record_id = global_event_id_wire.CreateGlobalEventId(
        builder, alert.global_event_id.high, alert.global_event_id.low
    )
    contract_record_wire.ContractRecordAddRecordId(builder, record_id)
    contract_record_wire.ContractRecordAddSchemaVersion(builder, _version(builder))
    contract_record_wire.ContractRecordAddPayloadType(
        builder, ContractPayload.EventIntelligenceRecord
    )
    contract_record_wire.ContractRecordAddRecordType(
        builder, RecordType.EVENT_INTELLIGENCE_RECORD
    )
    contract = contract_record_wire.End(builder)
    builder.Finish(contract, file_identifier=b"AMCR")
    return bytes(builder.Output())
