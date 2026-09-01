"""Generate or verify deterministic cross-language schema golden files."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from aegis.mx.contracts.v1.RecordType import RecordType
from aegis_mx_intelligence.contracts import (
    AuditMetadata,
    ChannelId,
    ConfigurationVersion,
    DataQualityCode,
    DataQualityContractInput,
    FeatureSnapshotId,
    ForecastId,
    GlobalEventId,
    Identifier128,
    InstrumentId,
    ModelForecastContractInput,
    ModelId,
    ModelVersion,
    SessionId,
    VenueId,
    build_data_quality_contract,
    build_model_forecast_contract,
    build_size_prefixed_audit_envelope,
)
from aegis_mx_intelligence.news_analysis import EntityRegistry, NoveltyIndex, RuleEngine
from aegis_mx_intelligence.news_contract import build_event_intelligence_contract
from aegis_mx_intelligence.news_security import detect_document_type, sanitize_document
from aegis_mx_intelligence.news_types import (
    Adjudication,
    AuthenticationEvidence,
    EntityDefinition,
    IntelligenceAlert,
    IntelligenceStage,
    SourceAuthentication,
    SourceDocument,
)

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
GOLDEN_DIRECTORY = REPOSITORY_ROOT / "schemas/golden"
DATA_QUALITY_BINARY_PATH = GOLDEN_DIRECTORY / "data_quality_v1.amae"
DATA_QUALITY_MANIFEST_PATH = GOLDEN_DIRECTORY / "data_quality_v1.json"
MODEL_FORECAST_BINARY_PATH = GOLDEN_DIRECTORY / "model_forecast_v1_3.amae"
MODEL_FORECAST_MANIFEST_PATH = GOLDEN_DIRECTORY / "model_forecast_v1_3.json"
EVENT_INTELLIGENCE_BINARY_PATH = GOLDEN_DIRECTORY / "event_intelligence_v1_4.amae"
EVENT_INTELLIGENCE_MANIFEST_PATH = GOLDEN_DIRECTORY / "event_intelligence_v1_4.json"


def canonical_data_quality_bytes() -> bytes:
    """Build the canonical fixture shared by C++ and Python tests."""
    contract = build_data_quality_contract(
        DataQualityContractInput(
            record_id=GlobalEventId(Identifier128(0x101, 0x102)),
            venue_id=VenueId(Identifier128(0x201, 0x202)),
            channel_id=ChannelId(Identifier128(0x301, 0x302)),
            state=DataQualityCode.VALID,
            observed_process_monotonic_time_ns=5_000_000,
            last_good_exchange_event_time_ns=1_800_000_000_000_000_000,
            last_good_nic_receive_time_ns=1_800_000_000_000_000_500,
            missing_sequence_count=0,
            malformed_record_count=0,
            stale_after_ns=250_000_000,
        )
    )
    return build_size_prefixed_audit_envelope(
        contract,
        RecordType.DATA_QUALITY_STATE,
        AuditMetadata(
            envelope_id=GlobalEventId(Identifier128(0x401, 0x402)),
            session_id=SessionId(Identifier128(0x501, 0x502)),
            configuration_version=ConfigurationVersion(Identifier128(0x601, 0x602)),
            created_process_monotonic_time_ns=5_000_100,
            recorded_wall_clock_utc_time_ns=1_800_000_000_100_000_000,
        ),
    )


def canonical_model_forecast_bytes() -> bytes:
    """Build the v1.3 forecast fixture shared by C++ and Python tests."""
    contract = build_model_forecast_contract(
        ModelForecastContractInput(
            record_id=GlobalEventId(Identifier128(0x701, 0x702)),
            forecast_id=ForecastId(Identifier128(0x711, 0x712)),
            session_id=SessionId(Identifier128(0x721, 0x722)),
            model_id=ModelId(Identifier128(0x731, 0x732)),
            model_version=ModelVersion(Identifier128(0x741, 0x742)),
            instrument_id=InstrumentId(Identifier128(0x751, 0x752)),
            feature_snapshot_id=FeatureSnapshotId(Identifier128(0x761, 0x762)),
            configuration_version=ConfigurationVersion(Identifier128(0x771, 0x772)),
            expected_return_ppm=1_250,
            return_p10_ppm=-2_000,
            return_p50_ppm=1_000,
            return_p90_ppm=4_500,
            probability_down_ppm=250_000,
            probability_flat_ppm=150_000,
            probability_up_ppm=600_000,
            volatility_ppm=3_250,
            confidence_ppm=800_000,
            calibration_score_ppm=900_000,
            data_quality_score_ppm=1_000_000,
            ood_score_ppm=25_000,
            horizon_ns=1_000_000_000,
            as_of_exchange_event_time_ns=1_800_000_001_000_000_000,
            production_process_monotonic_time_ns=6_000_000,
            expiration_process_monotonic_time_ns=6_250_000,
            has_transaction_cost_estimate=True,
            estimated_spread_cost_ppm=100,
            estimated_slippage_cost_ppm=125,
            estimated_market_impact_ppm=250,
            estimated_adverse_selection_cost_ppm=75,
            estimated_fee_cost_ppm=50,
        )
    )
    return build_size_prefixed_audit_envelope(
        contract,
        RecordType.MODEL_FORECAST,
        AuditMetadata(
            envelope_id=GlobalEventId(Identifier128(0x781, 0x782)),
            session_id=SessionId(Identifier128(0x721, 0x722)),
            configuration_version=ConfigurationVersion(Identifier128(0x771, 0x772)),
            created_process_monotonic_time_ns=6_000_100,
            recorded_wall_clock_utc_time_ns=1_800_000_001_100_000_000,
        ),
    )


def canonical_event_intelligence_bytes() -> bytes:
    """Build the v1.4 intelligence fixture shared by C++ and Python tests."""
    source = SourceDocument(
        provider_id="synthetic-public-provider",
        document_id="fixture-earnings-001",
        source_uri="https://public.example.invalid/fixture-earnings-001",
        content_type="text/plain; charset=utf-8",
        payload=(
            b"ACME Corp press release: the company said ACME reported earnings "
            b"revenue $5 million."
        ),
        provider_event_time_utc_ns=1_800_000_002_000_000_000,
        received_wall_clock_utc_ns=1_800_000_002_000_001_000,
        authentication=AuthenticationEvidence(
            SourceAuthentication.MOCK_VERIFIED,
            "synthetic-golden",
            hashlib.sha256(b"synthetic-golden-authentication").digest(),
        ),
    )
    document = sanitize_document(source)
    instrument = InstrumentId(Identifier128(0x801, 0x802))
    registry = EntityRegistry(
        (EntityDefinition("ACME Corporation", ("ACME Corp",), ("ACME",), instrument),)
    )
    entities = registry.resolve(document.analysis_text)
    novelty = NoveltyIndex().score(document.analysis_text)
    classification = RuleEngine().classify(document, novelty)
    if classification is None:
        msg = "canonical intelligence fixture did not classify"
        raise RuntimeError(msg)
    alert = IntelligenceAlert(
        global_event_id=GlobalEventId(Identifier128(0x811, 0x812)),
        session_id=SessionId(Identifier128(0x821, 0x822)),
        source_channel_id=ChannelId(Identifier128(0x831, 0x832)),
        configuration_version=ConfigurationVersion(Identifier128(0x841, 0x842)),
        model_id=ModelId(Identifier128(0x851, 0x852)),
        model_version=ModelVersion(Identifier128(0x861, 0x862)),
        document=document,
        document_type=detect_document_type(document),
        stage=IntelligenceStage.FAST,
        adjudication=Adjudication.PRELIMINARY,
        revision=1,
        entities=entities,
        classification=classification,
        processed_monotonic_time_ns=7_000_000,
    )
    contract = build_event_intelligence_contract(alert)
    return build_size_prefixed_audit_envelope(
        contract,
        RecordType.EVENT_INTELLIGENCE_RECORD,
        AuditMetadata(
            envelope_id=GlobalEventId(Identifier128(0x871, 0x872)),
            session_id=alert.session_id,
            configuration_version=alert.configuration_version,
            created_process_monotonic_time_ns=7_000_100,
            recorded_wall_clock_utc_time_ns=1_800_000_002_000_002_000,
        ),
    )


def manifest(encoded: bytes, file_name: str) -> bytes:
    """Return canonical UTF-8 manifest bytes."""
    content = {
        "file": file_name,
        "flatbuffers_version": "25.12.19",
        "schema_version": "1.8.0",
        "sha256": hashlib.sha256(encoded).hexdigest(),
        "size_bytes": len(encoded),
        "test_seed": 20_260_828,
    }
    return (json.dumps(content, indent=2, sort_keys=True) + "\n").encode()


def main() -> int:
    """Write fixtures or fail when checked-in bytes drift."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    data_quality = canonical_data_quality_bytes()
    model_forecast = canonical_model_forecast_bytes()
    event_intelligence = canonical_event_intelligence_bytes()
    expected = {
        DATA_QUALITY_BINARY_PATH: data_quality,
        DATA_QUALITY_MANIFEST_PATH: manifest(
            data_quality, DATA_QUALITY_BINARY_PATH.name
        ),
        MODEL_FORECAST_BINARY_PATH: model_forecast,
        MODEL_FORECAST_MANIFEST_PATH: manifest(
            model_forecast, MODEL_FORECAST_BINARY_PATH.name
        ),
        EVENT_INTELLIGENCE_BINARY_PATH: event_intelligence,
        EVENT_INTELLIGENCE_MANIFEST_PATH: manifest(
            event_intelligence, EVENT_INTELLIGENCE_BINARY_PATH.name
        ),
    }

    if args.check:
        drifted = [
            str(path.relative_to(REPOSITORY_ROOT))
            for path, content in expected.items()
            if not path.exists() or path.read_bytes() != content
        ]
        if drifted:
            parser.error("golden file drift: " + ", ".join(drifted))
        print(f"Schema golden files are current: {len(expected)}")
        return 0

    GOLDEN_DIRECTORY.mkdir(parents=True, exist_ok=True)
    for path, content in expected.items():
        path.write_bytes(content)
    print(f"Wrote schema golden files: {len(expected)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
