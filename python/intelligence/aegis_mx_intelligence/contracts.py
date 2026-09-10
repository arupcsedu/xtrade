"""Canonical off-hot-path builders for the Aegis-MX v1 event contracts."""

from __future__ import annotations

import hashlib
import hmac
import struct
from dataclasses import dataclass
from enum import IntEnum
from typing import Any, NewType, cast

import aegis.mx.contracts.v1.AuditEnvelope as audit_envelope_wire
import aegis.mx.contracts.v1.ChannelId as channel_id_wire
import aegis.mx.contracts.v1.ConfigurationVersion as configuration_version_wire
import aegis.mx.contracts.v1.ContractRecord as contract_record_wire
import aegis.mx.contracts.v1.DataQualityState as data_quality_state_wire
import aegis.mx.contracts.v1.ExchangeEventTimeNs as exchange_time_wire
import aegis.mx.contracts.v1.FeatureSnapshotId as feature_snapshot_id_wire
import aegis.mx.contracts.v1.ForecastId as forecast_id_wire
import aegis.mx.contracts.v1.GlobalEventId as global_event_id_wire
import aegis.mx.contracts.v1.HorizonSpec as horizon_spec_wire
import aegis.mx.contracts.v1.InstrumentId as instrument_id_wire
import aegis.mx.contracts.v1.ModelForecast as model_forecast_wire
import aegis.mx.contracts.v1.ModelId as model_id_wire
import aegis.mx.contracts.v1.ModelVersion as model_version_wire
import aegis.mx.contracts.v1.NicReceiveTimeNs as nic_time_wire
import aegis.mx.contracts.v1.ProcessMonotonicTimeNs as monotonic_time_wire
import aegis.mx.contracts.v1.SchemaVersion as schema_version_wire
import aegis.mx.contracts.v1.SessionId as session_id_wire
import aegis.mx.contracts.v1.Sha256Digest as sha256_digest_wire
import aegis.mx.contracts.v1.VenueId as venue_id_wire
import aegis.mx.contracts.v1.WallClockUtcTimeNs as wall_time_wire
import flatbuffers  # type: ignore[import-untyped]
from aegis.mx.contracts.v1.AuditEnvelope import AuditEnvelope
from aegis.mx.contracts.v1.ContractPayload import ContractPayload
from aegis.mx.contracts.v1.ContractRecord import ContractRecord
from aegis.mx.contracts.v1.DataQualityCode import DataQualityCode as WireDataQualityCode
from aegis.mx.contracts.v1.DataQualityState import DataQualityState
from aegis.mx.contracts.v1.ForecastHorizonUnit import (
    ForecastHorizonUnit as WireForecastHorizonUnit,
)
from aegis.mx.contracts.v1.ForecastTarget import ForecastTarget
from aegis.mx.contracts.v1.ForecastUnit import ForecastUnit
from aegis.mx.contracts.v1.HorizonHaltPolicy import (
    HorizonHaltPolicy as WireHorizonHaltPolicy,
)
from aegis.mx.contracts.v1.HorizonSessionEndpoint import (
    HorizonSessionEndpoint as WireHorizonSessionEndpoint,
)
from aegis.mx.contracts.v1.RecordType import RecordType

SCHEMA_MAJOR = 1
SCHEMA_MINOR = 9
SCHEMA_PATCH = 0
MAXIMUM_CONTRACT_BYTES = 1 << 20
MAXIMUM_ENVELOPE_BYTES = 1 << 22
MINIMUM_ENVELOPE_BYTES = 12


@dataclass(frozen=True, slots=True, order=True)
class Identifier128:
    """Opaque unsigned 128-bit value; all-zero is invalid."""

    high: int
    low: int

    def __post_init__(self) -> None:
        """Reject values that cannot be represented by the wire struct."""
        maximum = (1 << 64) - 1
        if not 0 <= self.high <= maximum or not 0 <= self.low <= maximum:
            msg = "identifier words must be unsigned 64-bit integers"
            raise ValueError(msg)
        if self.high == 0 and self.low == 0:
            msg = "the all-zero identifier is reserved and invalid"
            raise ValueError(msg)

    def hex(self) -> str:
        """Return the canonical fixed-width lowercase representation."""
        return f"{self.high:016x}{self.low:016x}"


VenueId = NewType("VenueId", Identifier128)
InstrumentId = NewType("InstrumentId", Identifier128)
ChannelId = NewType("ChannelId", Identifier128)
StrategyId = NewType("StrategyId", Identifier128)
ModelId = NewType("ModelId", Identifier128)
ModelVersion = NewType("ModelVersion", Identifier128)
OrderId = NewType("OrderId", Identifier128)
IntentId = NewType("IntentId", Identifier128)
ForecastId = NewType("ForecastId", Identifier128)
FeatureSnapshotId = NewType("FeatureSnapshotId", Identifier128)
RiskSnapshotId = NewType("RiskSnapshotId", Identifier128)
ConfigurationVersion = NewType("ConfigurationVersion", Identifier128)
GlobalEventId = NewType("GlobalEventId", Identifier128)
SessionId = NewType("SessionId", Identifier128)


class DataQualityCode(IntEnum):
    """Known v1 data-quality states; zero is deliberately not exposed."""

    VALID = WireDataQualityCode.VALID
    DEGRADED = WireDataQualityCode.DEGRADED
    STALE = WireDataQualityCode.STALE
    INVALID = WireDataQualityCode.INVALID


class ForecastTargetCode(IntEnum):
    """Known v1.3 time-series forecast targets."""

    RETURN = ForecastTarget.RETURN
    REALIZED_VOLATILITY = ForecastTarget.REALIZED_VOLATILITY
    VOLUME = ForecastTarget.VOLUME
    SPREAD = ForecastTarget.SPREAD
    MARKET_FACTOR = ForecastTarget.MARKET_FACTOR
    SECTOR_FACTOR = ForecastTarget.SECTOR_FACTOR


class ForecastUnitCode(IntEnum):
    """Explicit v1.3 units accepted by the rich forecast builder."""

    RETURN_PPM = ForecastUnit.RETURN_PPM
    VOLATILITY_PPM = ForecastUnit.VOLATILITY_PPM
    VOLUME_UNITS = ForecastUnit.VOLUME_UNITS
    SPREAD_TICKS = ForecastUnit.SPREAD_TICKS
    FACTOR_PPM = ForecastUnit.FACTOR_PPM


class ForecastHorizonUnitCode(IntEnum):
    """Known v1.9 semantic forecast-horizon units."""

    ELAPSED_NANOSECONDS = WireForecastHorizonUnit.ELAPSED_NANOSECONDS
    TRADING_MINUTES = WireForecastHorizonUnit.TRADING_MINUTES
    TRADING_SESSIONS = WireForecastHorizonUnit.TRADING_SESSIONS


class HorizonHaltPolicyCode(IntEnum):
    """Known v1.9 handling for halt intervals."""

    NOT_APPLICABLE = WireHorizonHaltPolicy.NOT_APPLICABLE
    REJECT = WireHorizonHaltPolicy.REJECT
    PAUSE = WireHorizonHaltPolicy.PAUSE
    COUNT_SCHEDULED = WireHorizonHaltPolicy.COUNT_SCHEDULED


class HorizonSessionEndpointCode(IntEnum):
    """Known v1.9 deterministic session endpoints."""

    NOT_APPLICABLE = WireHorizonSessionEndpoint.NOT_APPLICABLE
    REGULAR_SESSION_CLOSE = WireHorizonSessionEndpoint.REGULAR_SESSION_CLOSE


class ContractValidationError(ValueError):
    """Raised when a record fails closed during offline decoding."""


@dataclass(frozen=True, slots=True)
class DataQualityContractInput:
    """Input to the deterministic DataQualityState contract builder."""

    record_id: GlobalEventId
    venue_id: VenueId
    channel_id: ChannelId
    state: DataQualityCode
    observed_process_monotonic_time_ns: int
    last_good_exchange_event_time_ns: int
    last_good_nic_receive_time_ns: int
    missing_sequence_count: int
    malformed_record_count: int
    stale_after_ns: int


@dataclass(frozen=True, slots=True)
class ModelForecastContractInput:
    """Integer-only v1.9 forecast values for deterministic serialization."""

    record_id: GlobalEventId
    forecast_id: ForecastId
    session_id: SessionId
    model_id: ModelId
    model_version: ModelVersion
    instrument_id: InstrumentId
    feature_snapshot_id: FeatureSnapshotId
    configuration_version: ConfigurationVersion
    expected_return_ppm: int
    return_p10_ppm: int
    return_p50_ppm: int
    return_p90_ppm: int
    probability_down_ppm: int
    probability_flat_ppm: int
    probability_up_ppm: int
    volatility_ppm: int
    confidence_ppm: int
    calibration_score_ppm: int
    data_quality_score_ppm: int
    ood_score_ppm: int
    horizon_ns: int
    as_of_exchange_event_time_ns: int
    production_process_monotonic_time_ns: int
    expiration_process_monotonic_time_ns: int
    has_transaction_cost_estimate: bool = False
    estimated_spread_cost_ppm: int = 0
    estimated_slippage_cost_ppm: int = 0
    estimated_market_impact_ppm: int = 0
    estimated_adverse_selection_cost_ppm: int = 0
    estimated_fee_cost_ppm: int = 0
    forecast_target: ForecastTargetCode = ForecastTargetCode.RETURN
    forecast_unit: ForecastUnitCode = ForecastUnitCode.RETURN_PPM
    target_value: int = 0
    target_p10: int = 0
    target_p50: int = 0
    target_p90: int = 0
    horizon_unit: ForecastHorizonUnitCode = ForecastHorizonUnitCode.ELAPSED_NANOSECONDS
    horizon_value: int = 0
    horizon_halt_policy: HorizonHaltPolicyCode = HorizonHaltPolicyCode.NOT_APPLICABLE
    horizon_session_endpoint: HorizonSessionEndpointCode = (
        HorizonSessionEndpointCode.NOT_APPLICABLE
    )
    horizon_calendar_version: ConfigurationVersion | None = None
    target_exchange_event_time_ns: int = 0


@dataclass(frozen=True, slots=True)
class AuditMetadata:
    """Unambiguous timestamps and identities for an AuditEnvelope."""

    envelope_id: GlobalEventId
    session_id: SessionId
    configuration_version: ConfigurationVersion
    created_process_monotonic_time_ns: int
    recorded_wall_clock_utc_time_ns: int
    previous_envelope_sha256: bytes = bytes(32)


@dataclass(frozen=True, slots=True)
class DecodedDataQualityEnvelope:
    """Validated values copied from an offline DataQualityState envelope."""

    envelope_id: Identifier128
    record_id: Identifier128
    session_id: Identifier128
    configuration_version: Identifier128
    venue_id: Identifier128
    channel_id: Identifier128
    state: DataQualityCode
    observed_process_monotonic_time_ns: int
    last_good_exchange_event_time_ns: int
    last_good_nic_receive_time_ns: int
    missing_sequence_count: int
    malformed_record_count: int
    stale_after_ns: int
    payload_sha256: bytes


def _check_uint64(value: int, name: str, *, positive: bool = False) -> None:
    maximum = (1 << 64) - 1
    minimum = 1 if positive else 0
    if not minimum <= value <= maximum:
        msg = f"{name} is outside its unsigned 64-bit wire range"
        raise ValueError(msg)


def _check_int64(value: int, name: str, *, positive: bool = False) -> None:
    minimum = 1 if positive else -(1 << 63)
    maximum = (1 << 63) - 1
    if not minimum <= value <= maximum:
        msg = f"{name} is outside its signed 64-bit wire range"
        raise ValueError(msg)


def _create_version(builder: flatbuffers.Builder) -> int:
    return cast(
        "int",
        schema_version_wire.CreateSchemaVersion(
            builder,
            SCHEMA_MAJOR,
            SCHEMA_MINOR,
            SCHEMA_PATCH,
        ),
    )


def build_data_quality_contract(data: DataQualityContractInput) -> bytes:
    """Serialize one canonical, non-size-prefixed AMCR contract."""
    _check_uint64(
        data.observed_process_monotonic_time_ns,
        "observed_process_monotonic_time_ns",
        positive=True,
    )
    _check_int64(
        data.last_good_exchange_event_time_ns,
        "last_good_exchange_event_time_ns",
        positive=True,
    )
    _check_int64(
        data.last_good_nic_receive_time_ns,
        "last_good_nic_receive_time_ns",
        positive=True,
    )
    _check_uint64(data.missing_sequence_count, "missing_sequence_count")
    _check_uint64(data.malformed_record_count, "malformed_record_count")
    _check_uint64(data.stale_after_ns, "stale_after_ns", positive=True)

    builder = flatbuffers.Builder(512)
    data_quality_state_wire.Start(builder)
    data_quality_state_wire.DataQualityStateAddStaleAfterNs(
        builder, data.stale_after_ns
    )
    data_quality_state_wire.DataQualityStateAddMalformedRecordCount(
        builder,
        data.malformed_record_count,
    )
    data_quality_state_wire.DataQualityStateAddMissingSequenceCount(
        builder,
        data.missing_sequence_count,
    )
    nic_time = nic_time_wire.CreateNicReceiveTimeNs(
        builder,
        data.last_good_nic_receive_time_ns,
    )
    data_quality_state_wire.DataQualityStateAddLastGoodNicReceiveTime(builder, nic_time)
    exchange_time = exchange_time_wire.CreateExchangeEventTimeNs(
        builder,
        data.last_good_exchange_event_time_ns,
    )
    data_quality_state_wire.DataQualityStateAddLastGoodExchangeEventTime(
        builder, exchange_time
    )
    monotonic_time = monotonic_time_wire.CreateProcessMonotonicTimeNs(
        builder,
        data.observed_process_monotonic_time_ns,
    )
    data_quality_state_wire.DataQualityStateAddObservedProcessMonotonicTime(
        builder,
        monotonic_time,
    )
    channel_id = channel_id_wire.CreateChannelId(
        builder,
        data.channel_id.high,
        data.channel_id.low,
    )
    data_quality_state_wire.DataQualityStateAddChannelId(builder, channel_id)
    venue_id = venue_id_wire.CreateVenueId(
        builder,
        data.venue_id.high,
        data.venue_id.low,
    )
    data_quality_state_wire.DataQualityStateAddVenueId(builder, venue_id)
    data_quality_state_wire.DataQualityStateAddSchemaVersion(
        builder, _create_version(builder)
    )
    data_quality_state_wire.DataQualityStateAddState(
        builder, cast("Any", int(data.state))
    )
    data_quality = data_quality_state_wire.End(builder)

    contract_record_wire.Start(builder)
    contract_record_wire.ContractRecordAddPayload(builder, data_quality)
    record_id = global_event_id_wire.CreateGlobalEventId(
        builder,
        data.record_id.high,
        data.record_id.low,
    )
    contract_record_wire.ContractRecordAddRecordId(builder, record_id)
    contract_record_wire.ContractRecordAddSchemaVersion(
        builder, _create_version(builder)
    )
    contract_record_wire.ContractRecordAddPayloadType(
        builder, ContractPayload.DataQualityState
    )
    contract_record_wire.ContractRecordAddRecordType(
        builder, RecordType.DATA_QUALITY_STATE
    )
    contract = contract_record_wire.End(builder)
    builder.Finish(contract, file_identifier=b"AMCR")
    return bytes(builder.Output())


def build_model_forecast_contract(data: ModelForecastContractInput) -> bytes:
    """Serialize one canonical, non-size-prefixed v1.9 ModelForecast."""
    maximum_score = 1_000_000
    maximum_return = 10_000_000
    returns = (
        data.expected_return_ppm,
        data.return_p10_ppm,
        data.return_p50_ppm,
        data.return_p90_ppm,
    )
    for index, value in enumerate(returns):
        _check_int64(value, f"return_value_{index}")
        if not -maximum_return <= value <= maximum_return:
            msg = "forecast return is outside the v1.3 semantic range"
            raise ValueError(msg)
    if not data.return_p10_ppm <= data.return_p50_ppm <= data.return_p90_ppm:
        msg = "forecast quantiles must be monotonic"
        raise ValueError(msg)
    scores = (
        data.probability_down_ppm,
        data.probability_flat_ppm,
        data.probability_up_ppm,
        data.confidence_ppm,
        data.calibration_score_ppm,
        data.data_quality_score_ppm,
        data.ood_score_ppm,
    )
    if any(not 0 <= score <= maximum_score for score in scores):
        msg = "forecast score is outside [0, 1_000_000] PPM"
        raise ValueError(msg)
    if sum(scores[:3]) != maximum_score:
        msg = "directional probabilities must sum to 1_000_000 PPM"
        raise ValueError(msg)
    _check_uint64(data.volatility_ppm, "volatility_ppm")
    if data.volatility_ppm > maximum_return:
        msg = "volatility_ppm exceeds the v1.3 semantic range"
        raise ValueError(msg)
    _check_uint64(data.horizon_ns, "horizon_ns", positive=True)
    _check_int64(
        data.as_of_exchange_event_time_ns,
        "as_of_exchange_event_time_ns",
        positive=True,
    )
    known_horizon_units = {
        ForecastHorizonUnitCode.ELAPSED_NANOSECONDS,
        ForecastHorizonUnitCode.TRADING_MINUTES,
        ForecastHorizonUnitCode.TRADING_SESSIONS,
    }
    known_halt_policies = {
        HorizonHaltPolicyCode.NOT_APPLICABLE,
        HorizonHaltPolicyCode.REJECT,
        HorizonHaltPolicyCode.PAUSE,
        HorizonHaltPolicyCode.COUNT_SCHEDULED,
    }
    known_session_endpoints = {
        HorizonSessionEndpointCode.NOT_APPLICABLE,
        HorizonSessionEndpointCode.REGULAR_SESSION_CLOSE,
    }
    if (
        not isinstance(data.horizon_unit, ForecastHorizonUnitCode)
        or data.horizon_unit not in known_horizon_units
        or not isinstance(data.horizon_halt_policy, HorizonHaltPolicyCode)
        or data.horizon_halt_policy not in known_halt_policies
        or not isinstance(data.horizon_session_endpoint, HorizonSessionEndpointCode)
        or data.horizon_session_endpoint not in known_session_endpoints
    ):
        msg = "forecast horizon contains an invalid enum value"
        raise ValueError(msg)

    horizon_value = data.horizon_value
    target_exchange_event_time_ns = data.target_exchange_event_time_ns
    if data.horizon_unit is ForecastHorizonUnitCode.ELAPSED_NANOSECONDS:
        if (
            data.horizon_halt_policy is not HorizonHaltPolicyCode.NOT_APPLICABLE
            or data.horizon_session_endpoint
            is not HorizonSessionEndpointCode.NOT_APPLICABLE
            or data.horizon_calendar_version is not None
        ):
            msg = "elapsed horizon cannot carry calendar semantics"
            raise ValueError(msg)
        horizon_value = horizon_value or data.horizon_ns
        target_exchange_event_time_ns = (
            target_exchange_event_time_ns
            or data.as_of_exchange_event_time_ns + data.horizon_ns
        )
    else:
        if horizon_value == 0 or target_exchange_event_time_ns == 0:
            msg = "trading horizon requires explicit value and target timestamp"
            raise ValueError(msg)
        if data.horizon_calendar_version is None:
            msg = "trading horizon requires a calendar version"
            raise ValueError(msg)
        if data.horizon_halt_policy is HorizonHaltPolicyCode.NOT_APPLICABLE:
            msg = "trading horizon requires an explicit halt policy"
            raise ValueError(msg)
        expected_endpoint = (
            HorizonSessionEndpointCode.NOT_APPLICABLE
            if data.horizon_unit is ForecastHorizonUnitCode.TRADING_MINUTES
            else HorizonSessionEndpointCode.REGULAR_SESSION_CLOSE
        )
        if data.horizon_session_endpoint is not expected_endpoint:
            msg = "forecast horizon unit and session endpoint disagree"
            raise ValueError(msg)
    _check_uint64(horizon_value, "horizon_value", positive=True)
    _check_int64(
        target_exchange_event_time_ns,
        "target_exchange_event_time_ns",
        positive=True,
    )
    if (
        target_exchange_event_time_ns <= data.as_of_exchange_event_time_ns
        or target_exchange_event_time_ns - data.as_of_exchange_event_time_ns
        != data.horizon_ns
    ):
        msg = "target timestamp and elapsed horizon disagree"
        raise ValueError(msg)
    _check_uint64(
        data.production_process_monotonic_time_ns,
        "production_process_monotonic_time_ns",
        positive=True,
    )
    _check_uint64(
        data.expiration_process_monotonic_time_ns,
        "expiration_process_monotonic_time_ns",
        positive=True,
    )
    if (
        data.expiration_process_monotonic_time_ns
        <= data.production_process_monotonic_time_ns
    ):
        msg = "forecast expiration must follow production"
        raise ValueError(msg)
    costs = (
        data.estimated_spread_cost_ppm,
        data.estimated_slippage_cost_ppm,
        data.estimated_market_impact_ppm,
        data.estimated_adverse_selection_cost_ppm,
        data.estimated_fee_cost_ppm,
    )
    if any(not 0 <= cost <= maximum_score for cost in costs):
        msg = "transaction cost is outside [0, 1_000_000] PPM"
        raise ValueError(msg)
    if not data.has_transaction_cost_estimate and any(costs):
        msg = "transaction cost fields require the presence flag"
        raise ValueError(msg)

    unit_by_target = {
        ForecastTargetCode.RETURN: ForecastUnitCode.RETURN_PPM,
        ForecastTargetCode.REALIZED_VOLATILITY: ForecastUnitCode.VOLATILITY_PPM,
        ForecastTargetCode.VOLUME: ForecastUnitCode.VOLUME_UNITS,
        ForecastTargetCode.SPREAD: ForecastUnitCode.SPREAD_TICKS,
        ForecastTargetCode.MARKET_FACTOR: ForecastUnitCode.FACTOR_PPM,
        ForecastTargetCode.SECTOR_FACTOR: ForecastUnitCode.FACTOR_PPM,
    }
    if unit_by_target.get(data.forecast_target) is not data.forecast_unit:
        msg = "forecast target and unit do not agree"
        raise ValueError(msg)
    target_values = (
        (
            data.expected_return_ppm,
            data.return_p10_ppm,
            data.return_p50_ppm,
            data.return_p90_ppm,
        )
        if data.forecast_target is ForecastTargetCode.RETURN
        else (data.target_value, data.target_p10, data.target_p50, data.target_p90)
    )
    if not target_values[1] <= target_values[2] <= target_values[3]:
        msg = "target quantiles must be monotonic"
        raise ValueError(msg)
    target_bounds = {
        ForecastTargetCode.RETURN: (-maximum_return, maximum_return),
        ForecastTargetCode.REALIZED_VOLATILITY: (0, maximum_return),
        ForecastTargetCode.VOLUME: (0, 1_000_000_000_000),
        ForecastTargetCode.SPREAD: (0, 1_000_000_000),
        ForecastTargetCode.MARKET_FACTOR: (-maximum_return, maximum_return),
        ForecastTargetCode.SECTOR_FACTOR: (-maximum_return, maximum_return),
    }
    minimum_target, maximum_target = target_bounds[data.forecast_target]
    if any(not minimum_target <= value <= maximum_target for value in target_values):
        msg = "forecast target is outside its semantic range"
        raise ValueError(msg)
    if data.forecast_target is not ForecastTargetCode.RETURN and (
        any(returns)
        or data.probability_down_ppm != 0
        or data.probability_flat_ppm != maximum_score
        or data.probability_up_ppm != 0
    ):
        msg = "non-return forecasts require neutral return fields"
        raise ValueError(msg)

    builder = flatbuffers.Builder(1024)
    horizon_spec_wire.Start(builder)
    horizon_spec_wire.HorizonSpecAddValue(builder, horizon_value)
    if data.horizon_calendar_version is not None:
        calendar_version = configuration_version_wire.CreateConfigurationVersion(
            builder,
            data.horizon_calendar_version.high,
            data.horizon_calendar_version.low,
        )
        horizon_spec_wire.HorizonSpecAddCalendarVersion(builder, calendar_version)
    horizon_spec_wire.HorizonSpecAddSessionEndpoint(
        builder,
        cast("Any", data.horizon_session_endpoint),
    )
    horizon_spec_wire.HorizonSpecAddHaltPolicy(
        builder,
        cast("Any", data.horizon_halt_policy),
    )
    horizon_spec_wire.HorizonSpecAddUnit(builder, cast("Any", data.horizon_unit))
    horizon_spec = horizon_spec_wire.End(builder)

    model_forecast_wire.Start(builder)
    model_forecast_wire.ModelForecastAddTargetP90(builder, target_values[3])
    model_forecast_wire.ModelForecastAddTargetP50(builder, target_values[2])
    model_forecast_wire.ModelForecastAddTargetP10(builder, target_values[1])
    model_forecast_wire.ModelForecastAddTargetValue(builder, target_values[0])
    model_forecast_wire.ModelForecastAddEstimatedAdverseSelectionCostPpm(
        builder, data.estimated_adverse_selection_cost_ppm
    )
    model_forecast_wire.ModelForecastAddEstimatedSlippageCostPpm(
        builder, data.estimated_slippage_cost_ppm
    )
    model_forecast_wire.ModelForecastAddEstimatedFeeCostPpm(
        builder, data.estimated_fee_cost_ppm
    )
    model_forecast_wire.ModelForecastAddEstimatedMarketImpactPpm(
        builder, data.estimated_market_impact_ppm
    )
    model_forecast_wire.ModelForecastAddEstimatedSpreadCostPpm(
        builder, data.estimated_spread_cost_ppm
    )
    model_forecast_wire.ModelForecastAddVolatilityPpm(builder, data.volatility_ppm)
    model_forecast_wire.ModelForecastAddReturnP90Ppm(builder, data.return_p90_ppm)
    model_forecast_wire.ModelForecastAddReturnP50Ppm(builder, data.return_p50_ppm)
    model_forecast_wire.ModelForecastAddReturnP10Ppm(builder, data.return_p10_ppm)
    model_forecast_wire.ModelForecastAddExpectedReturnPpm(
        builder, data.expected_return_ppm
    )
    model_forecast_wire.ModelForecastAddHorizonNs(builder, data.horizon_ns)
    model_forecast_wire.ModelForecastAddForecastValue(builder, target_values[0])
    target_exchange_time = exchange_time_wire.CreateExchangeEventTimeNs(
        builder,
        target_exchange_event_time_ns,
    )
    model_forecast_wire.ModelForecastAddTargetExchangeEventTime(
        builder,
        target_exchange_time,
    )
    model_forecast_wire.ModelForecastAddHorizonSpec(builder, horizon_spec)
    model_forecast_wire.ModelForecastAddOodScorePpm(builder, data.ood_score_ppm)
    model_forecast_wire.ModelForecastAddDataQualityScorePpm(
        builder, data.data_quality_score_ppm
    )
    model_forecast_wire.ModelForecastAddCalibrationScorePpm(
        builder, data.calibration_score_ppm
    )
    model_forecast_wire.ModelForecastAddProbabilityUpPpm(
        builder, data.probability_up_ppm
    )
    model_forecast_wire.ModelForecastAddProbabilityFlatPpm(
        builder, data.probability_flat_ppm
    )
    model_forecast_wire.ModelForecastAddProbabilityDownPpm(
        builder, data.probability_down_ppm
    )
    as_of_time = exchange_time_wire.CreateExchangeEventTimeNs(
        builder, data.as_of_exchange_event_time_ns
    )
    model_forecast_wire.ModelForecastAddAsOfExchangeEventTime(builder, as_of_time)
    expiration_time = monotonic_time_wire.CreateProcessMonotonicTimeNs(
        builder, data.expiration_process_monotonic_time_ns
    )
    model_forecast_wire.ModelForecastAddValidUntilProcessMonotonicTime(
        builder, expiration_time
    )
    production_time = monotonic_time_wire.CreateProcessMonotonicTimeNs(
        builder, data.production_process_monotonic_time_ns
    )
    model_forecast_wire.ModelForecastAddCreatedProcessMonotonicTime(
        builder, production_time
    )
    model_forecast_wire.ModelForecastAddConfidencePpm(builder, data.confidence_ppm)
    configuration_version = configuration_version_wire.CreateConfigurationVersion(
        builder, data.configuration_version.high, data.configuration_version.low
    )
    model_forecast_wire.ModelForecastAddConfigurationVersion(
        builder, configuration_version
    )
    feature_snapshot_id = feature_snapshot_id_wire.CreateFeatureSnapshotId(
        builder, data.feature_snapshot_id.high, data.feature_snapshot_id.low
    )
    model_forecast_wire.ModelForecastAddFeatureSnapshotId(builder, feature_snapshot_id)
    instrument_id = instrument_id_wire.CreateInstrumentId(
        builder, data.instrument_id.high, data.instrument_id.low
    )
    model_forecast_wire.ModelForecastAddInstrumentId(builder, instrument_id)
    model_version = model_version_wire.CreateModelVersion(
        builder, data.model_version.high, data.model_version.low
    )
    model_forecast_wire.ModelForecastAddModelVersion(builder, model_version)
    model_id = model_id_wire.CreateModelId(
        builder, data.model_id.high, data.model_id.low
    )
    model_forecast_wire.ModelForecastAddModelId(builder, model_id)
    session_id = session_id_wire.CreateSessionId(
        builder, data.session_id.high, data.session_id.low
    )
    model_forecast_wire.ModelForecastAddSessionId(builder, session_id)
    forecast_id = forecast_id_wire.CreateForecastId(
        builder, data.forecast_id.high, data.forecast_id.low
    )
    model_forecast_wire.ModelForecastAddForecastId(builder, forecast_id)
    model_forecast_wire.ModelForecastAddSchemaVersion(builder, _create_version(builder))
    # Match flatc's generated C++ CreateModelForecast insertion order exactly;
    # FlatBuffers tables are semantic-order independent, but golden bytes are not.
    model_forecast_wire.ModelForecastAddForecastTarget(
        builder, cast("Any", data.forecast_target)
    )
    model_forecast_wire.ModelForecastAddHasTransactionCostEstimate(
        builder, data.has_transaction_cost_estimate
    )
    model_forecast_wire.ModelForecastAddForecastUnit(
        builder, cast("Any", data.forecast_unit)
    )
    forecast = model_forecast_wire.End(builder)

    contract_record_wire.Start(builder)
    contract_record_wire.ContractRecordAddPayload(builder, forecast)
    record_id = global_event_id_wire.CreateGlobalEventId(
        builder, data.record_id.high, data.record_id.low
    )
    contract_record_wire.ContractRecordAddRecordId(builder, record_id)
    contract_record_wire.ContractRecordAddSchemaVersion(
        builder, _create_version(builder)
    )
    contract_record_wire.ContractRecordAddPayloadType(
        builder, ContractPayload.ModelForecast
    )
    contract_record_wire.ContractRecordAddRecordType(builder, RecordType.MODEL_FORECAST)
    contract = contract_record_wire.End(builder)
    builder.Finish(contract, file_identifier=b"AMCR")
    return bytes(builder.Output())


def build_size_prefixed_audit_envelope(
    contract_payload: bytes,
    record_type: int,
    metadata: AuditMetadata,
) -> bytes:
    """Wrap exact AMCR bytes in a canonical size-prefixed AMAE envelope."""
    if not contract_payload or len(contract_payload) > MAXIMUM_CONTRACT_BYTES:
        msg = "contract payload length is invalid"
        raise ValueError(msg)
    if not 1 <= record_type <= RecordType.KILL_SWITCH_EVENT:
        msg = "record_type is unknown or outside the v1 range"
        raise ValueError(msg)
    _check_uint64(
        metadata.created_process_monotonic_time_ns,
        "created_process_monotonic_time_ns",
        positive=True,
    )
    _check_int64(
        metadata.recorded_wall_clock_utc_time_ns,
        "recorded_wall_clock_utc_time_ns",
        positive=True,
    )
    if len(metadata.previous_envelope_sha256) != hashlib.sha256().digest_size:
        msg = "previous_envelope_sha256 must contain exactly 32 bytes"
        raise ValueError(msg)

    builder = flatbuffers.Builder(1024)
    payload = builder.CreateByteVector(contract_payload)
    payload_digest_bytes = hashlib.sha256(contract_payload).digest()

    audit_envelope_wire.Start(builder)
    previous_digest = sha256_digest_wire.CreateSha256Digest(
        builder,
        cast("Any", metadata.previous_envelope_sha256),
    )
    audit_envelope_wire.AuditEnvelopeAddPreviousEnvelopeSha256(builder, previous_digest)
    payload_digest = sha256_digest_wire.CreateSha256Digest(
        builder,
        cast("Any", payload_digest_bytes),
    )
    audit_envelope_wire.AuditEnvelopeAddPayloadSha256(builder, payload_digest)
    audit_envelope_wire.AuditEnvelopeAddPayload(builder, payload)
    wall_time = wall_time_wire.CreateWallClockUtcTimeNs(
        builder,
        metadata.recorded_wall_clock_utc_time_ns,
    )
    audit_envelope_wire.AuditEnvelopeAddRecordedWallClockUtcTime(builder, wall_time)
    monotonic_time = monotonic_time_wire.CreateProcessMonotonicTimeNs(
        builder,
        metadata.created_process_monotonic_time_ns,
    )
    audit_envelope_wire.AuditEnvelopeAddCreatedProcessMonotonicTime(
        builder, monotonic_time
    )
    configuration_version = configuration_version_wire.CreateConfigurationVersion(
        builder,
        metadata.configuration_version.high,
        metadata.configuration_version.low,
    )
    audit_envelope_wire.AuditEnvelopeAddConfigurationVersion(
        builder, configuration_version
    )
    session_id = session_id_wire.CreateSessionId(
        builder,
        metadata.session_id.high,
        metadata.session_id.low,
    )
    audit_envelope_wire.AuditEnvelopeAddSessionId(builder, session_id)
    envelope_id = global_event_id_wire.CreateGlobalEventId(
        builder,
        metadata.envelope_id.high,
        metadata.envelope_id.low,
    )
    audit_envelope_wire.AuditEnvelopeAddEnvelopeId(builder, envelope_id)
    audit_envelope_wire.AuditEnvelopeAddSchemaVersion(builder, _create_version(builder))
    audit_envelope_wire.AuditEnvelopeAddRecordType(builder, cast("Any", record_type))
    envelope = audit_envelope_wire.End(builder)
    builder.FinishSizePrefixed(envelope, file_identifier=b"AMAE")
    encoded = bytes(builder.Output())
    if len(encoded) > MAXIMUM_ENVELOPE_BYTES:
        msg = "audit envelope exceeds the v1 maximum"
        raise ValueError(msg)
    return encoded


def _identifier_from_wire(identifier: object | None) -> Identifier128:
    if identifier is None:
        raise ContractValidationError("required identifier is absent")
    high = int(identifier.High())  # type: ignore[attr-defined]
    low = int(identifier.Low())  # type: ignore[attr-defined]
    try:
        return Identifier128(high=high, low=low)
    except ValueError as error:
        raise ContractValidationError(str(error)) from error


def _require_current_version(version: object | None) -> None:
    if version is None or int(version.Major()) != SCHEMA_MAJOR:  # type: ignore[attr-defined]
        raise ContractValidationError("unsupported or missing schema major")


def decode_data_quality_envelope(encoded: bytes) -> DecodedDataQualityEnvelope:
    """Fail closed while decoding the canonical golden contract in Python.

    C++ verification remains the authoritative untrusted-wire ingress. This
    bounded offline decoder additionally checks framing, identifiers, hash,
    discriminators, enums, timestamps, and DataQualityState semantics.
    """
    try:
        if not MINIMUM_ENVELOPE_BYTES <= len(encoded) <= MAXIMUM_ENVELOPE_BYTES:
            raise ContractValidationError("audit envelope length is invalid")
        (declared_size,) = struct.unpack_from("<I", encoded)
        if declared_size != len(encoded) - 4:
            raise ContractValidationError("size prefix does not match buffer")
        if not AuditEnvelope.AuditEnvelopeBufferHasIdentifier(encoded, 0, True):
            raise ContractValidationError("AMAE file identifier is absent")

        envelope = AuditEnvelope.GetRootAs(encoded, 4)
        _require_current_version(envelope.SchemaVersion())
        envelope_id = _identifier_from_wire(envelope.EnvelopeId())
        session_id = _identifier_from_wire(envelope.SessionId())
        configuration_version = _identifier_from_wire(
            envelope.ConfigurationVersion(),
        )
        monotonic = envelope.CreatedProcessMonotonicTime()
        wall = envelope.RecordedWallClockUtcTime()
        if monotonic is None or int(monotonic.Value()) <= 0:
            raise ContractValidationError("process monotonic timestamp is invalid")
        if wall is None or int(wall.Value()) <= 0:
            raise ContractValidationError("wall-clock UTC timestamp is invalid")
        if envelope.RecordType() != RecordType.DATA_QUALITY_STATE:
            raise ContractValidationError("unexpected envelope record type")
        payload = bytes(
            cast("int", envelope.Payload(index))
            for index in range(envelope.PayloadLength())
        )
        if not payload or len(payload) > MAXIMUM_CONTRACT_BYTES:
            raise ContractValidationError("contract payload length is invalid")
        expected_digest = envelope.PayloadSha256()
        if expected_digest is None:
            raise ContractValidationError("payload SHA-256 is absent")
        payload_digest = bytes(
            cast("int", expected_digest.Bytes(index))
            for index in range(expected_digest.BytesLength())
        )
        if not hmac.compare_digest(payload_digest, hashlib.sha256(payload).digest()):
            raise ContractValidationError("payload SHA-256 does not match")
        if not ContractRecord.ContractRecordBufferHasIdentifier(payload, 0, False):
            raise ContractValidationError("AMCR file identifier is absent")

        contract = ContractRecord.GetRootAs(payload, 0)
        _require_current_version(contract.SchemaVersion())
        record_id = _identifier_from_wire(contract.RecordId())
        if contract.RecordType() != RecordType.DATA_QUALITY_STATE or (
            contract.PayloadType() != ContractPayload.DataQualityState
        ):
            raise ContractValidationError("record discriminator mismatch")
        union_table = contract.Payload()
        if union_table is None:
            raise ContractValidationError("DataQualityState payload is absent")
        data_quality = DataQualityState()
        data_quality.Init(union_table.Bytes, union_table.Pos)
        _require_current_version(data_quality.SchemaVersion())
        venue_id = _identifier_from_wire(data_quality.VenueId())
        channel_id = _identifier_from_wire(data_quality.ChannelId())
        try:
            state = DataQualityCode(data_quality.State())
        except ValueError as error:
            raise ContractValidationError("data-quality enum is invalid") from error
        observed = data_quality.ObservedProcessMonotonicTime()
        exchange_time = data_quality.LastGoodExchangeEventTime()
        nic_time = data_quality.LastGoodNicReceiveTime()
        if observed is None or int(observed.Value()) <= 0:
            raise ContractValidationError("observed monotonic timestamp is invalid")
        if exchange_time is None or int(exchange_time.Value()) <= 0:
            raise ContractValidationError("exchange event timestamp is invalid")
        if nic_time is None or int(nic_time.Value()) <= 0:
            raise ContractValidationError("NIC receive timestamp is invalid")
        if data_quality.StaleAfterNs() <= 0:
            raise ContractValidationError("staleness duration must be positive")
        return DecodedDataQualityEnvelope(
            envelope_id=envelope_id,
            record_id=record_id,
            session_id=session_id,
            configuration_version=configuration_version,
            venue_id=venue_id,
            channel_id=channel_id,
            state=state,
            observed_process_monotonic_time_ns=int(observed.Value()),
            last_good_exchange_event_time_ns=int(exchange_time.Value()),
            last_good_nic_receive_time_ns=int(nic_time.Value()),
            missing_sequence_count=int(data_quality.MissingSequenceCount()),
            malformed_record_count=int(data_quality.MalformedRecordCount()),
            stale_after_ns=int(data_quality.StaleAfterNs()),
            payload_sha256=payload_digest,
        )
    except ContractValidationError:
        raise
    except (IndexError, OverflowError, struct.error, TypeError, ValueError) as error:
        raise ContractValidationError("malformed FlatBuffer") from error
