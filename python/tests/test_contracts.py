"""Golden and fail-closed tests for the Python contract boundary."""

from __future__ import annotations

from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace
from typing import TYPE_CHECKING, Any, cast

import aegis_mx_intelligence.contracts as contracts_module
import pytest
from aegis.mx.contracts.v1.AuditEnvelope import AuditEnvelope
from aegis.mx.contracts.v1.ContractPayload import ContractPayload
from aegis.mx.contracts.v1.ContractRecord import ContractRecord
from aegis.mx.contracts.v1.DataQualityState import DataQualityState
from aegis.mx.contracts.v1.RecordType import RecordType
from aegis_mx_intelligence.contracts import (
    AuditMetadata,
    ChannelId,
    ConfigurationVersion,
    ContractValidationError,
    DataQualityCode,
    DataQualityContractInput,
    FeatureSnapshotId,
    ForecastId,
    ForecastTargetCode,
    ForecastUnitCode,
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
    decode_data_quality_envelope,
)

if TYPE_CHECKING:
    from collections.abc import Callable

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
GOLDEN_PATH = REPOSITORY_ROOT / "schemas/golden/data_quality_v1.amae"
MODEL_FORECAST_GOLDEN_PATH = REPOSITORY_ROOT / "schemas/golden/model_forecast_v1_3.amae"


def _identifier(high: int, low: int) -> Identifier128:
    return Identifier128(high=high, low=low)


def _contract_input() -> DataQualityContractInput:
    return DataQualityContractInput(
        record_id=GlobalEventId(_identifier(0x101, 0x102)),
        venue_id=VenueId(_identifier(0x201, 0x202)),
        channel_id=ChannelId(_identifier(0x301, 0x302)),
        state=DataQualityCode.VALID,
        observed_process_monotonic_time_ns=5_000_000,
        last_good_exchange_event_time_ns=1_800_000_000_000_000_000,
        last_good_nic_receive_time_ns=1_800_000_000_000_000_500,
        missing_sequence_count=0,
        malformed_record_count=0,
        stale_after_ns=250_000_000,
    )


def _audit_metadata() -> AuditMetadata:
    return AuditMetadata(
        envelope_id=GlobalEventId(_identifier(0x401, 0x402)),
        session_id=SessionId(_identifier(0x501, 0x502)),
        configuration_version=ConfigurationVersion(_identifier(0x601, 0x602)),
        created_process_monotonic_time_ns=5_000_100,
        recorded_wall_clock_utc_time_ns=1_800_000_000_100_000_000,
    )


def _canonical_envelope() -> bytes:
    contract = build_data_quality_contract(_contract_input())
    return build_size_prefixed_audit_envelope(
        contract,
        RecordType.DATA_QUALITY_STATE,
        _audit_metadata(),
    )


def _forecast_input() -> ModelForecastContractInput:
    return ModelForecastContractInput(
        record_id=GlobalEventId(_identifier(0x701, 0x702)),
        forecast_id=ForecastId(_identifier(0x711, 0x712)),
        session_id=SessionId(_identifier(0x721, 0x722)),
        model_id=ModelId(_identifier(0x731, 0x732)),
        model_version=ModelVersion(_identifier(0x741, 0x742)),
        instrument_id=InstrumentId(_identifier(0x751, 0x752)),
        feature_snapshot_id=FeatureSnapshotId(_identifier(0x761, 0x762)),
        configuration_version=ConfigurationVersion(_identifier(0x771, 0x772)),
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


def _canonical_forecast_envelope() -> bytes:
    contract = build_model_forecast_contract(_forecast_input())
    return build_size_prefixed_audit_envelope(
        contract,
        RecordType.MODEL_FORECAST,
        AuditMetadata(
            envelope_id=GlobalEventId(_identifier(0x781, 0x782)),
            session_id=SessionId(_identifier(0x721, 0x722)),
            configuration_version=ConfigurationVersion(_identifier(0x771, 0x772)),
            created_process_monotonic_time_ns=6_000_100,
            recorded_wall_clock_utc_time_ns=1_800_000_001_100_000_000,
        ),
    )


@pytest.mark.parametrize(
    ("changes", "message"),
    [
        (
            {"forecast_unit": ForecastUnitCode.VOLUME_UNITS},
            "target and unit",
        ),
        (
            {
                "forecast_target": ForecastTargetCode.VOLUME,
                "forecast_unit": ForecastUnitCode.VOLUME_UNITS,
                "target_value": 100,
                "target_p10": 200,
                "target_p50": 100,
                "target_p90": 300,
            },
            "quantiles",
        ),
        (
            {
                "forecast_target": ForecastTargetCode.VOLUME,
                "forecast_unit": ForecastUnitCode.VOLUME_UNITS,
                "target_value": 1_000_000_000_001,
                "target_p10": 0,
                "target_p50": 1,
                "target_p90": 2,
                "expected_return_ppm": 0,
                "return_p10_ppm": 0,
                "return_p50_ppm": 0,
                "return_p90_ppm": 0,
                "probability_down_ppm": 0,
                "probability_flat_ppm": 1_000_000,
                "probability_up_ppm": 0,
            },
            "semantic range",
        ),
        (
            {
                "forecast_target": ForecastTargetCode.VOLUME,
                "forecast_unit": ForecastUnitCode.VOLUME_UNITS,
                "target_value": 100,
                "target_p10": 90,
                "target_p50": 100,
                "target_p90": 110,
            },
            "neutral return",
        ),
    ],
)
def test_v1_3_forecast_target_semantics_fail_closed(
    changes: dict[str, object], message: str
) -> None:
    with pytest.raises(ValueError, match=message):
        build_model_forecast_contract(
            replace(_forecast_input(), **cast("Any", changes))
        )


def test_identifier_is_fixed_width_and_rejects_invalid_words() -> None:
    identifier = _identifier(0x0123456789ABCDEF, 0xFEDCBA9876543210)
    assert identifier.hex() == "0123456789abcdeffedcba9876543210"

    with pytest.raises(ValueError, match="unsigned 64-bit"):
        _identifier(-1, 1)
    with pytest.raises(ValueError, match="unsigned 64-bit"):
        _identifier(1 << 64, 1)
    with pytest.raises(ValueError, match="all-zero"):
        _identifier(0, 0)


def test_python_encoding_matches_cross_language_golden() -> None:
    assert _canonical_envelope() == GOLDEN_PATH.read_bytes()


def test_python_forecast_encoding_matches_cross_language_golden() -> None:
    assert _canonical_forecast_envelope() == MODEL_FORECAST_GOLDEN_PATH.read_bytes()


@pytest.mark.parametrize(
    ("field", "value", "message"),
    [
        ("probability_up_ppm", 599_999, "sum"),
        ("return_p10_ppm", 2_000, "monotonic"),
        ("expected_return_ppm", 10_000_001, "semantic range"),
        ("volatility_ppm", 10_000_001, "semantic range"),
        ("confidence_ppm", 1_000_001, "outside"),
        ("estimated_fee_cost_ppm", 1_000_001, "outside"),
        ("estimated_slippage_cost_ppm", 1_000_001, "outside"),
        ("estimated_adverse_selection_cost_ppm", 1_000_001, "outside"),
        ("expiration_process_monotonic_time_ns", 6_000_000, "expiration"),
        ("has_transaction_cost_estimate", False, "presence"),
    ],
)
def test_forecast_builder_fails_closed(
    field: str,
    value: object,
    message: str,
) -> None:
    changes = cast("Any", {field: value})
    with pytest.raises(ValueError, match=message):
        build_model_forecast_contract(replace(_forecast_input(), **changes))


def test_data_quality_envelope_round_trip() -> None:
    decoded = decode_data_quality_envelope(_canonical_envelope())

    assert decoded.envelope_id == _identifier(0x401, 0x402)
    assert decoded.record_id == _identifier(0x101, 0x102)
    assert decoded.session_id == _identifier(0x501, 0x502)
    assert decoded.configuration_version == _identifier(0x601, 0x602)
    assert decoded.venue_id == _identifier(0x201, 0x202)
    assert decoded.channel_id == _identifier(0x301, 0x302)
    assert decoded.state is DataQualityCode.VALID
    assert decoded.observed_process_monotonic_time_ns == 5_000_000
    assert decoded.last_good_exchange_event_time_ns == 1_800_000_000_000_000_000
    assert decoded.last_good_nic_receive_time_ns == 1_800_000_000_000_000_500
    assert decoded.missing_sequence_count == 0
    assert decoded.malformed_record_count == 0
    assert decoded.stale_after_ns == 250_000_000
    assert len(decoded.payload_sha256) == 32


@pytest.mark.parametrize(
    ("field", "value", "message"),
    [
        ("observed_process_monotonic_time_ns", 0, "unsigned 64-bit"),
        ("last_good_exchange_event_time_ns", 0, "signed 64-bit"),
        ("last_good_nic_receive_time_ns", 1 << 63, "signed 64-bit"),
        ("missing_sequence_count", -1, "unsigned 64-bit"),
        ("malformed_record_count", 1 << 64, "unsigned 64-bit"),
        ("stale_after_ns", 0, "unsigned 64-bit"),
    ],
)
def test_contract_builder_rejects_out_of_range_values(
    field: str,
    value: int,
    message: str,
) -> None:
    changes = cast("Any", {field: value})
    with pytest.raises(ValueError, match=message):
        build_data_quality_contract(replace(_contract_input(), **changes))


def test_envelope_builder_rejects_invalid_inputs() -> None:
    metadata = _audit_metadata()
    contract = build_data_quality_contract(_contract_input())

    with pytest.raises(ValueError, match="payload length"):
        build_size_prefixed_audit_envelope(b"", 17, metadata)
    with pytest.raises(ValueError, match="payload length"):
        build_size_prefixed_audit_envelope(bytes((1 << 20) + 1), 17, metadata)
    with pytest.raises(ValueError, match="record_type"):
        build_size_prefixed_audit_envelope(contract, 0, metadata)
    with pytest.raises(ValueError, match="unsigned 64-bit"):
        build_size_prefixed_audit_envelope(
            contract,
            17,
            replace(metadata, created_process_monotonic_time_ns=0),
        )
    with pytest.raises(ValueError, match="signed 64-bit"):
        build_size_prefixed_audit_envelope(
            contract,
            17,
            replace(metadata, recorded_wall_clock_utc_time_ns=0),
        )
    with pytest.raises(ValueError, match="exactly 32"):
        build_size_prefixed_audit_envelope(
            contract,
            17,
            replace(metadata, previous_envelope_sha256=b"short"),
        )


def test_decoder_rejects_framing_and_hash_failures() -> None:
    encoded = bytearray(_canonical_envelope())
    with pytest.raises(ContractValidationError, match="length"):
        decode_data_quality_envelope(b"")

    wrong_size = bytearray(encoded)
    wrong_size[0] ^= 1
    with pytest.raises(ContractValidationError, match="size prefix"):
        decode_data_quality_envelope(bytes(wrong_size))

    wrong_identifier = bytearray(encoded)
    wrong_identifier[8:12] = b"NOPE"
    with pytest.raises(ContractValidationError, match="AMAE"):
        decode_data_quality_envelope(bytes(wrong_identifier))

    # The final byte is inside the opaque AMCR vector and leaves the outer
    # FlatBuffer structurally readable, so integrity rejects it first.
    encoded[-1] ^= 1
    with pytest.raises(ContractValidationError, match="SHA-256"):
        decode_data_quality_envelope(bytes(encoded))


def test_decoder_rejects_unknown_data_quality_enum() -> None:
    invalid = replace(_contract_input(), state=cast("DataQualityCode", 255))
    contract = build_data_quality_contract(invalid)
    envelope = build_size_prefixed_audit_envelope(
        contract,
        RecordType.DATA_QUALITY_STATE,
        _audit_metadata(),
    )

    with pytest.raises(ContractValidationError, match="enum"):
        decode_data_quality_envelope(envelope)


_ZERO_IDENTIFIER = SimpleNamespace(High=lambda: 0, Low=lambda: 0)
_UNSUPPORTED_VERSION = SimpleNamespace(Major=lambda: 2)


@pytest.mark.parametrize(
    ("owner", "method", "replacement", "message"),
    [
        (AuditEnvelope, "SchemaVersion", lambda _: None, "schema major"),
        (
            AuditEnvelope,
            "SchemaVersion",
            lambda _: _UNSUPPORTED_VERSION,
            "schema major",
        ),
        (AuditEnvelope, "EnvelopeId", lambda _: None, "identifier"),
        (AuditEnvelope, "EnvelopeId", lambda _: _ZERO_IDENTIFIER, "all-zero"),
        (
            AuditEnvelope,
            "CreatedProcessMonotonicTime",
            lambda _: None,
            "monotonic",
        ),
        (AuditEnvelope, "RecordedWallClockUtcTime", lambda _: None, "wall-clock"),
        (
            AuditEnvelope,
            "RecordType",
            lambda _: RecordType.CLOCK_QUALITY_STATE,
            "record type",
        ),
        (AuditEnvelope, "PayloadLength", lambda _: 0, "payload length"),
        (AuditEnvelope, "PayloadSha256", lambda _: None, "SHA-256 is absent"),
        (
            ContractRecord,
            "RecordType",
            lambda _: RecordType.CLOCK_QUALITY_STATE,
            "discriminator",
        ),
        (
            ContractRecord,
            "PayloadType",
            lambda _: ContractPayload.ClockQualityState,
            "discriminator",
        ),
        (ContractRecord, "Payload", lambda _: None, "payload is absent"),
        (
            DataQualityState,
            "ObservedProcessMonotonicTime",
            lambda _: None,
            "observed monotonic",
        ),
        (
            DataQualityState,
            "LastGoodExchangeEventTime",
            lambda _: None,
            "exchange event",
        ),
        (
            DataQualityState,
            "LastGoodNicReceiveTime",
            lambda _: None,
            "NIC receive",
        ),
        (DataQualityState, "StaleAfterNs", lambda _: 0, "staleness"),
    ],
)
def test_decoder_rejects_missing_or_inconsistent_known_fields(
    monkeypatch: pytest.MonkeyPatch,
    owner: type[object],
    method: str,
    replacement: Callable[..., object],
    message: str,
) -> None:
    monkeypatch.setattr(owner, method, replacement)
    with pytest.raises(ContractValidationError, match=message):
        decode_data_quality_envelope(_canonical_envelope())


def test_decoder_rejects_missing_contract_identifier(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(
        ContractRecord,
        "ContractRecordBufferHasIdentifier",
        staticmethod(lambda *_: False),
    )
    with pytest.raises(ContractValidationError, match="AMCR"):
        decode_data_quality_envelope(_canonical_envelope())


def test_decoder_normalizes_low_level_parse_failures(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    def fail_parse(*_: object) -> None:
        raise IndexError

    monkeypatch.setattr(AuditEnvelope, "GetRootAs", staticmethod(fail_parse))
    with pytest.raises(ContractValidationError, match="malformed"):
        decode_data_quality_envelope(_canonical_envelope())


def test_envelope_builder_enforces_final_size_bound(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(contracts_module, "MAXIMUM_ENVELOPE_BYTES", 100)
    with pytest.raises(ValueError, match="exceeds"):
        build_size_prefixed_audit_envelope(
            build_data_quality_contract(_contract_input()),
            RecordType.DATA_QUALITY_STATE,
            _audit_metadata(),
        )
