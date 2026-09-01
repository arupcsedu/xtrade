"""Deterministic, point-in-time, and fail-closed macro specialist tests."""

from __future__ import annotations

import json
from dataclasses import replace
from pathlib import Path
from typing import TYPE_CHECKING, Any, cast

if TYPE_CHECKING:
    from collections.abc import Callable

import aegis_mx_intelligence.macro_specialist as specialist_module
import aegis_mx_intelligence.macro_types as types_module
import pytest
from aegis.mx.contracts.v1.ContractPayload import ContractPayload
from aegis.mx.contracts.v1.ContractRecord import ContractRecord
from aegis.mx.contracts.v1.ModelForecast import ModelForecast
from aegis.mx.contracts.v1.RecordType import RecordType
from aegis_mx_intelligence import (
    Annualization,
    ConfigurationVersion,
    CrossAssetGroup,
    CrossAssetKind,
    CrossAssetObservation,
    CrossAssetResponseFeature,
    CrossAssetUnit,
    EvidenceExcerpt,
    ExpectedReleaseField,
    FeatureSnapshotId,
    ForecastId,
    FrozenMacroEventProcessor,
    GlobalEventId,
    HistoricalMacroSurprise,
    Identifier128,
    InstrumentId,
    MacroActualRelease,
    MacroCalendarEvent,
    MacroConsensusEstimate,
    MacroConsensusSnapshot,
    MacroDecision,
    MacroDirectionDistribution,
    MacroeconomicReleaseSpecialist,
    MacroEvaluationError,
    MacroEventPhase,
    MacroEventType,
    MacroEvidenceReference,
    MacroInputBundle,
    MacroMarketSnapshot,
    MacroMetricIdentity,
    MacroMetricRole,
    MacroMetricUnit,
    MacroPriorValue,
    MacroReasonCode,
    MacroSourceEvidence,
    MacroSourceKind,
    MacroSourceQuality,
    MacroSpecialistConfig,
    MacroValueObservation,
    ModelId,
    ModelVersion,
    ReleaseCompletenessState,
    SeasonalAdjustment,
    SessionId,
    sha256_bytes,
)

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
FIXTURE_DIRECTORY = REPOSITORY_ROOT / "python/tests/fixtures/macro"
SCHEDULED = 1_800_000_000_000_000_000
FREEZE_TIME = SCHEDULED - 5_000_000
PRODUCTION_TIME = 9_000_000
TARGET_INSTRUMENT = InstrumentId(Identifier128(300, 301))


def _id(value: int) -> Identifier128:
    return Identifier128(value, value + 1)


def _identity(
    field_id: str,
    role: MacroMetricRole,
    unit: MacroMetricUnit = MacroMetricUnit.PERCENT_PPM,
    period: str = "2026-07",
    *,
    event_type: MacroEventType = MacroEventType.CPI,
) -> MacroMetricIdentity:
    return MacroMetricIdentity(
        event_type,
        field_id,
        role,
        unit,
        period,
        SeasonalAdjustment.SEASONALLY_ADJUSTED,
        Annualization.NOT_ANNUALIZED,
    )


def _reference(
    source_id: str, excerpt_id: int = 1
) -> tuple[MacroEvidenceReference, ...]:
    return (MacroEvidenceReference(source_id, excerpt_id),)


def _source(
    source_id: str,
    kind: MacroSourceKind,
    publication: int,
    receipt: int,
    *,
    quality: MacroSourceQuality = MacroSourceQuality.SYNTHETIC_REPLAY,
    authenticated: bool = True,
    excerpt_ids: tuple[int, ...] = (1,),
) -> MacroSourceEvidence:
    digest = sha256_bytes(source_id)
    return MacroSourceEvidence(
        source_id,
        GlobalEventId(
            Identifier128(
                int.from_bytes(digest[:8], "big") or 1,
                int.from_bytes(digest[8:16], "big"),
            )
        ),
        kind,
        quality,
        authenticated,
        f"{source_id}-provider",
        f"{source_id}-document",
        sha256_bytes(f"{source_id}-raw"),
        sha256_bytes(f"{source_id}-sanitized"),
        publication,
        receipt,
        tuple(
            EvidenceExcerpt(
                excerpt_id,
                (excerpt_id - 1) * 20,
                ((excerpt_id - 1) * 20) + len(f"{source_id} evidence"),
                f"{source_id} evidence",
            )
            for excerpt_id in excerpt_ids
        ),
    )


def _fields(
    *, core: bool = True, optional_subcomponent: bool = False
) -> tuple[ExpectedReleaseField, ...]:
    fields = [
        ExpectedReleaseField(
            _identity("cpi.all_items.mom", MacroMetricRole.HEADLINE),
            True,
            -1,
            500_000,
            _reference("calendar"),
        )
    ]
    if core:
        fields.append(
            ExpectedReleaseField(
                _identity("cpi.core.mom", MacroMetricRole.CORE),
                True,
                -1,
                300_000,
                _reference("calendar"),
            )
        )
    fields.append(
        ExpectedReleaseField(
            _identity("cpi.shelter.mom", MacroMetricRole.SUBCOMPONENT),
            not optional_subcomponent,
            -1,
            200_000,
            _reference("calendar"),
        )
    )
    return tuple(fields)


def _calendar(
    *, fields: tuple[ExpectedReleaseField, ...] | None = None
) -> MacroCalendarEvent:
    return MacroCalendarEvent(
        GlobalEventId(_id(10)),
        MacroEventType.CPI,
        "calendar-2026.08",
        SCHEDULED,
        _fields() if fields is None else fields,
    )


def _estimate(field: ExpectedReleaseField) -> MacroConsensusEstimate:
    values = {
        "cpi.all_items.mom": (30_000, 1_000),
        "cpi.core.mom": (29_000, 1_000),
        "cpi.shelter.mom": (35_000, 2_500),
    }
    consensus, dispersion = values[field.identity.field_id]
    return MacroConsensusEstimate(
        field.identity,
        consensus,
        dispersion,
        consensus - (dispersion * 2),
        consensus + (dispersion * 2),
        20,
        FREEZE_TIME - 1_000,
        _reference("consensus"),
    )


def _consensus(calendar: MacroCalendarEvent) -> MacroConsensusSnapshot:
    return MacroConsensusSnapshot(
        GlobalEventId(_id(20)),
        calendar.calendar_event_id,
        FREEZE_TIME,
        tuple(_estimate(field) for field in calendar.expected_fields),
    )


def _observed_value(field: ExpectedReleaseField) -> int:
    return {
        "cpi.all_items.mom": 32_000,
        "cpi.core.mom": 30_000,
        "cpi.shelter.mom": 40_000,
    }[field.identity.field_id]


def _market(receipt: int) -> MacroMarketSnapshot:
    observations = (
        CrossAssetObservation(
            TARGET_INSTRUMENT,
            CrossAssetKind.EQUITY_INDEX_FUTURE,
            CrossAssetUnit.RETURN_PPM,
            -20_000,
            receipt - 1_000,
            receipt + 5_000,
        ),
        CrossAssetObservation(
            InstrumentId(_id(310)),
            CrossAssetKind.TREASURY_YIELD,
            CrossAssetUnit.YIELD_CHANGE_BASIS_POINTS,
            2,
            receipt - 1_000,
            receipt + 5_000,
        ),
        CrossAssetObservation(
            InstrumentId(_id(320)),
            CrossAssetKind.FX_PROXY,
            CrossAssetUnit.RETURN_PPM,
            10_000,
            receipt - 1_000,
            receipt + 5_000,
        ),
        CrossAssetObservation(
            InstrumentId(_id(330)),
            CrossAssetKind.VOLATILITY_INSTRUMENT,
            CrossAssetUnit.RETURN_PPM,
            30_000,
            receipt - 1_000,
            receipt + 5_000,
        ),
        CrossAssetObservation(
            InstrumentId(_id(340)),
            CrossAssetKind.SECTOR_ETF,
            CrossAssetUnit.RETURN_PPM,
            -15_000,
            receipt - 1_000,
            receipt + 5_000,
        ),
    )
    return MacroMarketSnapshot(
        FeatureSnapshotId(_id(50)),
        "market",
        55_000,
        receipt + 10_000,
        observations,
    )


def _history() -> tuple[HistoricalMacroSurprise, ...]:
    values = (-2_000_000, -1_000_000, 0, 1_000_000, 1_500_000)
    return tuple(
        HistoricalMacroSurprise(
            GlobalEventId(_id(100 + index)),
            MacroEventType.CPI,
            SCHEDULED - 10_000_000 - (index * 1_000),
            SCHEDULED - 9_000_000 - (index * 1_000),
            value,
            "history",
        )
        for index, value in enumerate(values)
    )


def _bundle(
    *,
    release: bool = True,
    official_delay: int = 0,
    receipt_lag: int = 1_000_000,
    completeness: ReleaseCompletenessState = ReleaseCompletenessState.COMPLETE,
    revision: int = 1,
    correction_parent: bytes | None = None,
    omit_field: str | None = None,
    provider_conflict: bool = False,
    headline_actual: int = 32_000,
    calendar: MacroCalendarEvent | None = None,
    consensus: MacroConsensusSnapshot | None = None,
) -> MacroInputBundle:
    selected_calendar = _calendar() if calendar is None else calendar
    selected_consensus = (
        _consensus(selected_calendar) if consensus is None else consensus
    )
    sources = [
        _source(
            "calendar",
            MacroSourceKind.OFFICIAL_CALENDAR,
            SCHEDULED - 10_000_000,
            SCHEDULED - 9_000_000,
        ),
        _source(
            "consensus",
            MacroSourceKind.CONSENSUS_SNAPSHOT,
            SCHEDULED - 8_000_000,
            SCHEDULED - 7_000_000,
        ),
        _source(
            "prior",
            MacroSourceKind.PRIOR_RELEASE,
            SCHEDULED - 20_000_000,
            SCHEDULED - 19_000_000,
        ),
        _source(
            "history",
            MacroSourceKind.HISTORICAL_SURPRISE,
            SCHEDULED - 30_000_000,
            SCHEDULED - 29_000_000,
        ),
    ]
    actual_release = None
    market = None
    as_of = SCHEDULED - 1
    if release:
        publication = SCHEDULED + official_delay
        receipt = publication + receipt_lag
        sources.extend(
            [
                _source(
                    "release",
                    MacroSourceKind.OFFICIAL_RELEASE,
                    publication,
                    receipt,
                    quality=MacroSourceQuality.OFFICIAL_PRIMARY,
                ),
                _source(
                    "provider",
                    MacroSourceKind.DATA_PROVIDER,
                    publication,
                    receipt + 1,
                    quality=MacroSourceQuality.LICENSED_AGGREGATOR,
                ),
                _source(
                    "market",
                    MacroSourceKind.MARKET_FEATURES,
                    receipt,
                    receipt + 5_000,
                ),
            ]
        )
        observations = []
        for field in selected_calendar.expected_fields:
            if field.identity.field_id == omit_field:
                continue
            value = (
                headline_actual
                if field.identity.role is MacroMetricRole.HEADLINE
                else _observed_value(field)
            )
            observations.extend(
                [
                    MacroValueObservation(
                        field.identity,
                        value,
                        "release",
                        _reference("release"),
                    ),
                    MacroValueObservation(
                        field.identity,
                        value
                        + (
                            1
                            if provider_conflict
                            and field.identity.role is MacroMetricRole.HEADLINE
                            else 0
                        ),
                        "provider",
                        _reference("provider"),
                    ),
                ]
            )
        previous_period = _identity(
            "cpi.all_items.mom",
            MacroMetricRole.HEADLINE,
            period="2026-06",
        )
        actual_release = MacroActualRelease(
            revision,
            completeness,
            publication,
            receipt,
            tuple(observations),
            (
                MacroPriorValue(
                    previous_period,
                    28_000,
                    SCHEDULED - 20_000_000,
                    28_500,
                    (
                        MacroEvidenceReference("prior", 1),
                        MacroEvidenceReference("release", 1),
                    ),
                ),
            ),
            correction_parent,
        )
        market = _market(receipt)
        as_of = market.available_wall_clock_utc_ns + 1
    return MacroInputBundle(
        SessionId(_id(60)),
        TARGET_INSTRUMENT,
        ConfigurationVersion(_id(70)),
        selected_calendar,
        selected_consensus,
        tuple(sources),
        _history(),
        as_of,
        PRODUCTION_TIME,
        actual_release,
        market,
    )


def _config(**changes: object) -> MacroSpecialistConfig:
    config = MacroSpecialistConfig(ModelId(_id(80)), ModelVersion(_id(90)))
    return replace(config, **cast("Any", changes))


def _specialist(**changes: object) -> MacroeconomicReleaseSpecialist:
    return MacroeconomicReleaseSpecialist(_config(**changes))


def _load_fixture(name: str) -> dict[str, Any]:
    return cast(
        "dict[str, Any]",
        json.loads((FIXTURE_DIRECTORY / name).read_text(encoding="utf-8")),
    )


def _assert_invalid(*factories: Callable[[], object]) -> None:
    for factory in factories:
        with pytest.raises(ValueError, match=r".+"):
            factory()


def _fixture_bundle(data: dict[str, Any]) -> MacroInputBundle:
    revision = int(data["revision"])
    parent = None
    if revision > 1:
        first = _bundle()
        if first.release is None:
            raise AssertionError
        parent = first.release.sha256
    return _bundle(
        official_delay=int(data["official_delay_ns"]),
        receipt_lag=int(data["receipt_lag_ns"]),
        completeness=ReleaseCompletenessState[str(data["completeness"])],
        revision=revision,
        correction_parent=parent,
        omit_field=cast("str | None", data["omit_field"]),
        provider_conflict=bool(data["provider_conflict"]),
        headline_actual=int(data.get("headline_actual_override", 32_000)),
    )


def test_normal_release_calculates_all_outputs_and_common_forecast() -> None:
    result = _specialist().evaluate(_bundle())

    assert result.phase is MacroEventPhase.RELEASE_COMPLETE
    assert result.decision is MacroDecision.PUBLISH
    assert result.headline_surprise.surprise_ppm == 2_000_000
    assert result.core_surprise is not None
    assert result.core_surprise.surprise_ppm == 1_000_000
    assert result.subcomponent_surprises[0].surprise_ppm == 2_000_000
    assert result.revision_surprises[0].delta_value == 500
    assert result.revision_surprises[0].revision_surprise_ppm == 17_857
    assert result.surprise_percentile_ppm == 1_000_000
    assert {item.group for item in result.cross_asset_response_features} == set(
        CrossAssetGroup
    )
    assert result.cross_asset_response_features[1].normalized_magnitude_ppm == 2_000
    assert result.expected_return_ppm == -377_500
    assert result.expected_volatility_ppm == 15_000
    assert result.direction is not None
    assert (
        sum(
            (
                result.direction.down_ppm,
                result.direction.flat_ppm,
                result.direction.up_ppm,
            )
        )
        == 1_000_000
    )
    assert result.reason_codes == ()
    assert result.forecast_contract_bytes is not None
    record = ContractRecord.GetRootAs(result.forecast_contract_bytes, 0)
    assert record.RecordType() == RecordType.MODEL_FORECAST
    assert record.PayloadType() == ContractPayload.ModelForecast
    table = record.Payload()
    assert table is not None
    forecast = ModelForecast()
    forecast.Init(table.Bytes, table.Pos)
    assert forecast.ExpectedReturnPpm() == -377_500
    assert forecast.CalibrationScorePpm() == 0
    assert forecast.ProbabilityDownPpm() > forecast.ProbabilityUpPpm()


@pytest.mark.parametrize(
    "fixture_name",
    [
        "normal_release_v1.json",
        "delayed_release_v1.json",
        "partial_release_v1.json",
        "correction_v1.json",
        "conflicting_provider_values_v1.json",
    ],
)
def test_required_replay_fixtures_are_deterministic(fixture_name: str) -> None:
    data = _load_fixture(fixture_name)
    bundle = _fixture_bundle(data)
    specialist = _specialist()
    result = specialist.evaluate(bundle)

    assert result.phase.name == data["expected_phase"]
    assert result.decision.name == data["expected_decision"]
    assert set(data["expected_reason_codes"]).issubset(
        {item.name for item in result.reason_codes}
    )
    assert specialist.verify_replay(bundle, result.sha256) == result
    assert result.canonical_bytes() == specialist.evaluate(bundle).canonical_bytes()


def test_timestamp_disorder_fixture_fails_closed() -> None:
    data = _load_fixture("timestamp_disorder_v1.json")
    with pytest.raises(ValueError, match=str(data["expected_error"])):
        _fixture_bundle(data)


def test_scheduled_and_delayed_states_never_publish() -> None:
    scheduled = _specialist().evaluate(_bundle(release=False))
    delayed_bundle = replace(
        _bundle(release=False),
        as_of_wall_clock_utc_ns=SCHEDULED + 1,
    )
    delayed = _specialist().evaluate(delayed_bundle)

    assert scheduled.phase is MacroEventPhase.SCHEDULED
    assert scheduled.decision is MacroDecision.ABSTAIN
    assert scheduled.revision == 0
    assert scheduled.forecast_contract_bytes is None
    assert delayed.phase is MacroEventPhase.DELAYED
    assert set(delayed.reason_codes) == {
        MacroReasonCode.NO_RELEASE,
        MacroReasonCode.DELAYED_RELEASE,
    }


def test_optional_unreleased_subcomponent_is_retained_without_abstention() -> None:
    calendar = _calendar(fields=_fields(optional_subcomponent=True))
    result = _specialist().evaluate(
        _bundle(
            calendar=calendar,
            consensus=_consensus(calendar),
            omit_field="cpi.shelter.mom",
        )
    )

    assert result.decision is MacroDecision.PUBLISH
    assert result.subcomponent_surprises[0].reason is MacroReasonCode.FIELD_NOT_RELEASED


def test_calendar_without_core_is_supported() -> None:
    calendar = _calendar(fields=_fields(core=False))
    result = _specialist().evaluate(
        _bundle(calendar=calendar, consensus=_consensus(calendar))
    )
    assert result.core_surprise is None
    assert result.decision is MacroDecision.PUBLISH


def test_missing_and_incompatible_consensus_strictly_abstain() -> None:
    bundle = _bundle()
    missing = replace(
        bundle,
        consensus_snapshot=replace(
            bundle.consensus_snapshot,
            estimates=bundle.consensus_snapshot.estimates[1:],
        ),
    )
    result = _specialist().evaluate(missing)
    assert result.headline_surprise.reason is MacroReasonCode.MISSING_CONSENSUS
    assert result.decision is MacroDecision.ABSTAIN

    wrong_identity = replace(
        bundle.consensus_snapshot.estimates[0].identity,
        unit=MacroMetricUnit.RATIO_PPM,
    )
    incompatible = replace(
        bundle,
        consensus_snapshot=replace(
            bundle.consensus_snapshot,
            estimates=(
                replace(
                    bundle.consensus_snapshot.estimates[0], identity=wrong_identity
                ),
                *bundle.consensus_snapshot.estimates[1:],
            ),
        ),
    )
    result = _specialist().evaluate(incompatible)
    assert result.headline_surprise.reason is MacroReasonCode.INCOMPATIBLE_FIELD
    assert result.decision is MacroDecision.ABSTAIN


def test_incompatible_official_field_and_no_comparable_surprise_abstain() -> None:
    bundle = _bundle()
    if bundle.release is None:
        raise AssertionError
    observations = tuple(
        replace(
            item,
            identity=replace(item.identity, unit=MacroMetricUnit.RATIO_PPM),
        )
        for item in bundle.release.observations
    )
    result = _specialist().evaluate(
        replace(bundle, release=replace(bundle.release, observations=observations))
    )
    assert result.headline_surprise.reason is MacroReasonCode.INCOMPATIBLE_FIELD
    assert MacroReasonCode.NO_COMPARABLE_SURPRISE in result.reason_codes
    assert result.decision is MacroDecision.ABSTAIN


def test_quality_authentication_history_and_market_failures_abstain() -> None:
    bundle = _bundle()
    low_quality = replace(
        bundle,
        sources=(
            replace(bundle.sources[0], quality=MacroSourceQuality.UNVERIFIED),
            *bundle.sources[1:],
        ),
    )
    unauthenticated = replace(
        bundle,
        sources=(replace(bundle.sources[0], authenticated=False), *bundle.sources[1:]),
    )
    short_history = replace(
        bundle, historical_surprises=bundle.historical_surprises[:1]
    )
    missing_market = replace(bundle, market_snapshot=None)
    if bundle.market_snapshot is None:
        raise AssertionError
    invalid_market = replace(
        bundle,
        market_snapshot=replace(bundle.market_snapshot, valid=False),
    )
    missing_group = replace(
        bundle,
        market_snapshot=replace(
            bundle.market_snapshot,
            observations=bundle.market_snapshot.observations[:-1],
        ),
    )

    cases = (
        (low_quality, MacroReasonCode.LOW_SOURCE_QUALITY),
        (unauthenticated, MacroReasonCode.UNAUTHENTICATED_SOURCE),
        (short_history, MacroReasonCode.INSUFFICIENT_HISTORY),
        (missing_market, MacroReasonCode.MISSING_CROSS_ASSET_GROUP),
        (invalid_market, MacroReasonCode.INVALID_MARKET_FEATURES),
        (missing_group, MacroReasonCode.MISSING_CROSS_ASSET_GROUP),
    )
    for candidate, reason in cases:
        result = _specialist().evaluate(candidate)
        assert result.decision is MacroDecision.ABSTAIN
        assert reason in result.reason_codes
        assert result.forecast_contract_bytes is None


def test_zero_dispersion_revision_epsilon_and_response_bounds_are_deterministic() -> (
    None
):
    bundle = _bundle()
    if bundle.release is None or bundle.market_snapshot is None:
        raise AssertionError
    estimates = tuple(
        replace(item, dispersion=0) for item in bundle.consensus_snapshot.estimates
    )
    prior = replace(
        bundle.release.prior_values[0],
        value_known_before_release=0,
        revised_value_in_release=1,
    )
    observations = list(bundle.market_snapshot.observations)
    observations[0] = replace(observations[0], response_value=2_000_000)
    observations[1] = replace(observations[1], response_value=20_000)
    result = _specialist().evaluate(
        replace(
            bundle,
            consensus_snapshot=replace(bundle.consensus_snapshot, estimates=estimates),
            release=replace(bundle.release, prior_values=(prior,)),
            market_snapshot=replace(
                bundle.market_snapshot,
                observations=tuple(observations),
            ),
        )
    )

    assert result.headline_surprise.surprise_ppm == 2_000_000_000
    assert result.revision_surprises[0].revision_surprise_ppm == 1_000_000
    assert any(
        item.normalized_magnitude_ppm == 10_000_000
        for item in result.cross_asset_response_features
    )
    assert result.decision is MacroDecision.PUBLISH


def test_zero_market_responses_use_minimum_volatility_and_no_target_response() -> None:
    bundle = _bundle()
    if bundle.market_snapshot is None:
        raise AssertionError
    observations = tuple(
        replace(
            item,
            instrument_id=InstrumentId(_id(800 + index)),
            response_value=0,
        )
        for index, item in enumerate(bundle.market_snapshot.observations)
    )
    result = _specialist().evaluate(
        replace(
            bundle,
            market_snapshot=replace(bundle.market_snapshot, observations=observations),
        )
    )
    assert result.expected_volatility_ppm == 1
    assert result.expected_return_ppm == -375_000


def test_zero_weight_calendar_fails_before_publishing() -> None:
    calendar = _calendar(
        fields=tuple(replace(item, forecast_weight_ppm=0) for item in _fields())
    )
    with pytest.raises(MacroEvaluationError, match="weighted surprise"):
        _specialist().evaluate(
            _bundle(calendar=calendar, consensus=_consensus(calendar))
        )


def test_expiration_overflow_fails_closed() -> None:
    bundle = replace(
        _bundle(),
        production_process_monotonic_time_ns=(1 << 64) - 1,
    )
    with pytest.raises(MacroEvaluationError, match="expiration overflows"):
        _specialist().evaluate(bundle)


def test_replay_rejects_bad_digest_length_and_mismatch() -> None:
    specialist = _specialist()
    bundle = _bundle()
    with pytest.raises(MacroEvaluationError, match="32 bytes"):
        specialist.verify_replay(bundle, b"short")
    with pytest.raises(MacroEvaluationError, match="digest mismatch"):
        specialist.verify_replay(bundle, bytes(32))


def test_frozen_processor_enforces_event_consensus_revision_and_parent() -> None:
    scheduled = _bundle(release=False)
    specialist = _specialist()
    processor = FrozenMacroEventProcessor(specialist, scheduled)
    assert processor.consensus_snapshot_sha256 == scheduled.consensus_snapshot.sha256
    assert processor.process(scheduled).phase is MacroEventPhase.SCHEDULED

    first = _bundle()
    first_result = processor.process(first)
    assert first.release is not None
    correction = _bundle(
        revision=2,
        correction_parent=first.release.sha256,
        headline_actual=31_000,
    )
    assert processor.process(correction).phase is MacroEventPhase.CORRECTED

    with pytest.raises(MacroEvaluationError, match="stale"):
        processor.process(correction)
    different_event_id = GlobalEventId(_id(999))
    different_bundle = replace(
        scheduled,
        calendar=replace(scheduled.calendar, calendar_event_id=different_event_id),
        consensus_snapshot=replace(
            scheduled.consensus_snapshot,
            calendar_event_id=different_event_id,
        ),
    )
    with pytest.raises(MacroEvaluationError, match="different calendar"):
        processor.process(different_bundle)
    changed_estimate = replace(
        scheduled.consensus_snapshot.estimates[0],
        consensus_value=31_000,
        maximum_value=33_000,
    )
    with pytest.raises(MacroEvaluationError, match="consensus changed"):
        processor.process(
            replace(
                scheduled,
                consensus_snapshot=replace(
                    scheduled.consensus_snapshot,
                    estimates=(
                        changed_estimate,
                        *scheduled.consensus_snapshot.estimates[1:],
                    ),
                ),
            )
        )

    other = FrozenMacroEventProcessor(specialist, scheduled)
    with pytest.raises(MacroEvaluationError, match="does not link"):
        other.process(_bundle(revision=2, correction_parent=sha256_bytes("wrong")))
    assert first_result.release_sha256 == first.release.sha256


@pytest.mark.parametrize("unit", list(MacroMetricUnit))
def test_config_has_explicit_epsilon_for_every_unit(unit: MacroMetricUnit) -> None:
    assert _config().epsilon(unit) == 1


@pytest.mark.parametrize("event_type", list(MacroEventType))
def test_initial_event_type_set_is_stable(event_type: MacroEventType) -> None:
    assert MacroEventType(event_type.value) is event_type


def test_config_hash_and_validation() -> None:
    config = _config()
    assert config.sha256() == config.sha256()
    assert len(config.sha256()) == 64
    assert _specialist().configuration_sha256 == config.sha256()
    invalid_changes = (
        {"forecast_ttl_ns": 0},
        {"forecast_horizon_ns": 0},
        {"minimum_history_count": 0},
        {"minimum_source_quality_ppm": 1_000_001},
        {"yield_basis_point_scale_ppm": 0},
        {"epsilon_percent_ppm": 0},
        {"required_cross_asset_groups": ()},
        {
            "required_cross_asset_groups": (
                CrossAssetGroup.FX,
                CrossAssetGroup.FX,
            )
        },
    )
    for changes in invalid_changes:
        with pytest.raises(ValueError, match=r"macro|yield|required"):
            _config(**changes)


@pytest.mark.parametrize(
    ("quality", "score"),
    [
        (MacroSourceQuality.UNVERIFIED, 0),
        (MacroSourceQuality.PUBLIC_SECONDARY, 500_000),
        (MacroSourceQuality.LICENSED_AGGREGATOR, 800_000),
        (MacroSourceQuality.OFFICIAL_PRIMARY, 1_000_000),
        (MacroSourceQuality.SYNTHETIC_REPLAY, 900_000),
    ],
)
def test_source_quality_mapping(quality: MacroSourceQuality, score: int) -> None:
    source = _source(
        "quality",
        MacroSourceKind.DATA_PROVIDER,
        SCHEDULED,
        SCHEDULED,
        quality=quality,
    )
    assert specialist_module._source_quality_score(source) == score


@pytest.mark.parametrize(
    ("kind", "group"),
    [
        (CrossAssetKind.EQUITY_INDEX_FUTURE, CrossAssetGroup.EQUITY_INDEX),
        (CrossAssetKind.TREASURY_FUTURE, CrossAssetGroup.RATES),
        (CrossAssetKind.TREASURY_YIELD, CrossAssetGroup.RATES),
        (CrossAssetKind.FX_PROXY, CrossAssetGroup.FX),
        (CrossAssetKind.VOLATILITY_INSTRUMENT, CrossAssetGroup.VOLATILITY),
        (CrossAssetKind.SECTOR_ETF, CrossAssetGroup.SECTOR),
    ],
)
def test_cross_asset_group_mapping(
    kind: CrossAssetKind, group: CrossAssetGroup
) -> None:
    assert specialist_module._cross_asset_group(kind) is group


def test_numeric_helpers_and_output_primitives_fail_closed() -> None:
    assert specialist_module._trunc_div(-5, 2) == -2
    assert specialist_module._median([1, 3]) == 2
    assert specialist_module._median([1, 2, 3]) == 2
    with pytest.raises(MacroEvaluationError, match="denominator"):
        specialist_module._trunc_div(1, 0)
    with pytest.raises(MacroEvaluationError, match="median"):
        specialist_module._median([])
    with pytest.raises(MacroEvaluationError, match="signed 64-bit"):
        specialist_module._scaled_ratio_ppm(1 << 63, 1)
    with pytest.raises(ValueError, match="direction probabilities"):
        MacroDirectionDistribution(1, 2, 3)
    with pytest.raises(ValueError, match="normalized cross-asset"):
        CrossAssetResponseFeature(
            TARGET_INSTRUMENT,
            CrossAssetKind.EQUITY_INDEX_FUTURE,
            CrossAssetGroup.EQUITY_INDEX,
            CrossAssetUnit.RETURN_PPM,
            1,
            10_000_001,
        )


def test_canonical_helpers_reject_unsupported_values() -> None:
    assert b'"event_type":"CPI"' in _bundle().canonical_bytes()
    with pytest.raises(TypeError, match="unsupported canonical macro"):
        types_module._canonical_value(1.5)


def test_identity_reference_source_and_reference_count_validation() -> None:
    identity = _identity("cpi.all_items.mom", MacroMetricRole.HEADLINE)
    source = _source(
        "source",
        MacroSourceKind.DATA_PROVIDER,
        SCHEDULED,
        SCHEDULED,
        excerpt_ids=(1, 2),
    )
    reference = MacroEvidenceReference("source", 1)

    _assert_invalid(
        lambda: replace(identity, field_id=""),
        lambda: replace(identity, field_id="\x00"),
        lambda: replace(identity, field_id="x" * 65),
        lambda: replace(identity, reference_period=""),
        lambda: MacroEvidenceReference("", 1),
        lambda: MacroEvidenceReference("source", 0),
        lambda: replace(source, source_id=""),
        lambda: replace(source, provider_id=""),
        lambda: replace(source, document_id=""),
        lambda: replace(source, source_content_sha256=bytes(32)),
        lambda: replace(source, sanitized_content_sha256=b"short"),
        lambda: replace(source, publication_wall_clock_utc_ns=0),
        lambda: replace(source, received_wall_clock_utc_ns=SCHEDULED - 1),
        lambda: replace(source, excerpts=()),
        lambda: replace(source, excerpts=(source.excerpts[0],) * 17),
        lambda: replace(source, excerpts=(source.excerpts[0],) * 2),
        lambda: types_module._validate_reference_count((), "test"),
        lambda: types_module._validate_reference_count((reference,) * 17, "test"),
        lambda: types_module._validate_reference_count((reference,) * 2, "test"),
    )


def test_expected_field_and_calendar_validation() -> None:
    field = _fields()[0]
    calendar = _calendar()
    subcomponent = _fields()[2]
    second_core = replace(
        subcomponent,
        identity=replace(subcomponent.identity, role=MacroMetricRole.CORE),
    )

    _assert_invalid(
        lambda: replace(field, positive_surprise_direction=2),
        lambda: replace(field, forecast_weight_ppm=-1),
        lambda: replace(field, forecast_weight_ppm=1_000_001),
        lambda: replace(field, evidence=()),
        lambda: replace(calendar, calendar_version=""),
        lambda: replace(calendar, scheduled_release_wall_clock_utc_ns=0),
        lambda: replace(calendar, expected_fields=()),
        lambda: replace(calendar, expected_fields=(field,) * 65),
        lambda: replace(calendar, expected_fields=(field, field)),
        lambda: replace(
            calendar,
            expected_fields=(
                replace(
                    field,
                    identity=replace(field.identity, event_type=MacroEventType.PPI),
                ),
            ),
        ),
        lambda: replace(calendar, expected_fields=(subcomponent,)),
        lambda: replace(
            calendar,
            expected_fields=(*_fields(), second_core),
        ),
        lambda: replace(
            calendar,
            expected_fields=(replace(field, required=False), *_fields()[1:]),
        ),
    )


def test_consensus_value_prior_and_release_validation() -> None:
    calendar = _calendar()
    estimate = _estimate(calendar.expected_fields[0])
    snapshot = _consensus(calendar)
    bundle = _bundle()
    if bundle.release is None:
        raise AssertionError
    observation = bundle.release.observations[0]
    prior = bundle.release.prior_values[0]
    release = bundle.release

    _assert_invalid(
        lambda: replace(estimate, consensus_value=1 << 63),
        lambda: replace(estimate, minimum_value=-(1 << 63) - 1),
        lambda: replace(estimate, maximum_value=1 << 63),
        lambda: replace(estimate, dispersion=-1),
        lambda: replace(estimate, contributor_count=0),
        lambda: replace(estimate, minimum_value=estimate.consensus_value + 1),
        lambda: replace(estimate, available_wall_clock_utc_ns=0),
        lambda: replace(estimate, evidence=()),
        lambda: replace(snapshot, frozen_wall_clock_utc_ns=0),
        lambda: replace(snapshot, estimates=(estimate,) * 65),
        lambda: replace(snapshot, estimates=(estimate, estimate)),
        lambda: replace(
            snapshot,
            estimates=(
                replace(
                    estimate,
                    available_wall_clock_utc_ns=snapshot.frozen_wall_clock_utc_ns + 1,
                ),
            ),
        ),
        lambda: replace(observation, value=1 << 63),
        lambda: replace(observation, source_id=""),
        lambda: replace(observation, evidence=()),
        lambda: replace(prior, value_known_before_release=1 << 63),
        lambda: replace(prior, revised_value_in_release=1 << 63),
        lambda: replace(prior, known_available_wall_clock_utc_ns=0),
        lambda: replace(prior, evidence=()),
        lambda: replace(release, revision=0),
        lambda: replace(release, correction_of_release_sha256=sha256_bytes("parent")),
        lambda: replace(
            release,
            revision=2,
            correction_of_release_sha256=None,
        ),
        lambda: replace(
            release,
            revision=2,
            correction_of_release_sha256=bytes(32),
        ),
        lambda: replace(release, official_publication_wall_clock_utc_ns=0),
        lambda: replace(
            release,
            receipt_wall_clock_utc_ns=release.official_publication_wall_clock_utc_ns
            - 1,
        ),
        lambda: replace(release, observations=(observation,) * 129),
        lambda: replace(release, prior_values=(prior,) * 65),
        lambda: replace(release, prior_values=(prior, prior)),
        lambda: replace(release, observations=(observation, observation)),
    )
    assert len(release.canonical_bytes()) > 0
    assert len(release.sha256) == 32


def test_cross_asset_market_and_history_validation() -> None:
    bundle = _bundle()
    if bundle.market_snapshot is None:
        raise AssertionError
    market = bundle.market_snapshot
    response = market.observations[0]
    yield_response = market.observations[1]
    history = bundle.historical_surprises[0]

    _assert_invalid(
        lambda: replace(response, response_value=1 << 63),
        lambda: replace(response, window_start_wall_clock_utc_ns=0),
        lambda: replace(
            response,
            window_end_wall_clock_utc_ns=response.window_start_wall_clock_utc_ns,
        ),
        lambda: replace(response, unit=CrossAssetUnit.YIELD_CHANGE_BASIS_POINTS),
        lambda: replace(yield_response, unit=CrossAssetUnit.RETURN_PPM),
        lambda: replace(response, response_value=10_000_001),
        lambda: replace(market, source_id=""),
        lambda: replace(market, as_of_exchange_event_time_ns=0),
        lambda: replace(market, available_wall_clock_utc_ns=0),
        lambda: replace(market, observations=()),
        lambda: replace(market, observations=(response,) * 65),
        lambda: replace(market, observations=(response, response)),
        lambda: replace(history, official_publication_wall_clock_utc_ns=0),
        lambda: replace(
            history,
            available_wall_clock_utc_ns=(
                history.official_publication_wall_clock_utc_ns - 1
            ),
        ),
        lambda: replace(history, headline_surprise_ppm=1 << 63),
        lambda: replace(history, source_id=""),
    )


def test_bundle_top_level_source_and_evidence_validation() -> None:
    bundle = _bundle()
    scheduled = _bundle(release=False)
    wrong_event_id = GlobalEventId(_id(900))
    duplicate_source = (bundle.sources[0], bundle.sources[0], *bundle.sources[1:])
    no_calendar = tuple(
        item
        for item in bundle.sources
        if item.kind is not MacroSourceKind.OFFICIAL_CALENDAR
    )
    two_calendars = (
        *bundle.sources,
        replace(bundle.sources[0], source_id="calendar-two"),
    )
    no_consensus = tuple(
        item
        for item in bundle.sources
        if item.kind is not MacroSourceKind.CONSENSUS_SNAPSHOT
    )
    no_release_source = tuple(
        item
        for item in bundle.sources
        if item.kind is not MacroSourceKind.OFFICIAL_RELEASE
    )
    release_source = next(
        item for item in bundle.sources if item.kind is MacroSourceKind.OFFICIAL_RELEASE
    )

    _assert_invalid(
        lambda: replace(bundle, schema_version="2.0.0"),
        lambda: replace(bundle, as_of_wall_clock_utc_ns=0),
        lambda: replace(bundle, production_process_monotonic_time_ns=0),
        lambda: replace(bundle, sources=(bundle.sources[0],) * 49),
        lambda: replace(
            bundle,
            historical_surprises=(bundle.historical_surprises[0],) * 257,
        ),
        lambda: replace(
            bundle,
            consensus_snapshot=replace(
                bundle.consensus_snapshot,
                calendar_event_id=wrong_event_id,
            ),
        ),
        lambda: replace(
            bundle,
            consensus_snapshot=replace(
                bundle.consensus_snapshot,
                frozen_wall_clock_utc_ns=SCHEDULED,
                estimates=tuple(
                    replace(
                        item,
                        available_wall_clock_utc_ns=SCHEDULED,
                    )
                    for item in bundle.consensus_snapshot.estimates
                ),
            ),
        ),
        lambda: replace(bundle, sources=duplicate_source),
        lambda: replace(bundle, sources=no_calendar),
        lambda: replace(bundle, sources=two_calendars),
        lambda: replace(bundle, sources=no_consensus),
        lambda: replace(bundle, sources=no_release_source),
        lambda: replace(scheduled, sources=(*scheduled.sources, release_source)),
    )


def test_bundle_evidence_and_calendar_consensus_provenance_validation() -> None:
    bundle = _bundle()
    if bundle.market_snapshot is None:
        raise AssertionError
    unknown_reference = MacroEvidenceReference("unknown", 1)
    bad_calendar = replace(
        bundle.calendar,
        expected_fields=(
            replace(bundle.calendar.expected_fields[0], evidence=(unknown_reference,)),
            *bundle.calendar.expected_fields[1:],
        ),
    )
    bad_history = replace(bundle.historical_surprises[0], source_id="unknown")
    wrong_market_source = replace(bundle.market_snapshot, source_id="provider")
    wrong_event_estimate = replace(
        bundle.consensus_snapshot.estimates[0],
        identity=replace(
            bundle.consensus_snapshot.estimates[0].identity,
            event_type=MacroEventType.PPI,
        ),
    )
    wrong_calendar_evidence = replace(
        bundle.calendar,
        expected_fields=(
            replace(
                bundle.calendar.expected_fields[0],
                evidence=_reference("consensus"),
            ),
            *bundle.calendar.expected_fields[1:],
        ),
    )
    late_consensus_source = tuple(
        replace(
            item,
            publication_wall_clock_utc_ns=FREEZE_TIME,
            received_wall_clock_utc_ns=FREEZE_TIME + 1,
        )
        if item.source_id == "consensus"
        else item
        for item in bundle.sources
    )

    _assert_invalid(
        lambda: replace(bundle, calendar=bad_calendar),
        lambda: replace(
            bundle,
            historical_surprises=(bad_history, *bundle.historical_surprises[1:]),
        ),
        lambda: replace(bundle, market_snapshot=wrong_market_source),
        lambda: replace(
            bundle,
            consensus_snapshot=replace(
                bundle.consensus_snapshot,
                estimates=(
                    wrong_event_estimate,
                    *bundle.consensus_snapshot.estimates[1:],
                ),
            ),
        ),
        lambda: replace(bundle, calendar=wrong_calendar_evidence),
        lambda: replace(bundle, sources=late_consensus_source),
    )


def test_bundle_point_in_time_and_release_provenance_validation() -> None:
    bundle = _bundle()
    if bundle.release is None or bundle.market_snapshot is None:
        raise AssertionError
    release = bundle.release
    scheduled = _bundle(release=False)
    after_as_of_sources = tuple(
        replace(
            item,
            received_wall_clock_utc_ns=bundle.as_of_wall_clock_utc_ns + 1,
        )
        if item.source_id == "market"
        else item
        for item in bundle.sources
    )
    late_calendar_sources = tuple(
        replace(
            item,
            publication_wall_clock_utc_ns=SCHEDULED,
            received_wall_clock_utc_ns=SCHEDULED,
        )
        if item.source_id == "calendar"
        else item
        for item in bundle.sources
    )
    wrong_history_event = replace(
        bundle.historical_surprises[0], event_type=MacroEventType.PPI
    )
    current_history = replace(
        bundle.historical_surprises[0],
        official_publication_wall_clock_utc_ns=SCHEDULED,
        available_wall_clock_utc_ns=SCHEDULED,
    )
    future_history = replace(
        bundle.historical_surprises[0],
        available_wall_clock_utc_ns=FREEZE_TIME + 1,
    )

    _assert_invalid(
        lambda: replace(bundle, sources=after_as_of_sources),
        lambda: replace(bundle, sources=late_calendar_sources),
        lambda: replace(
            bundle,
            historical_surprises=(
                wrong_history_event,
                *bundle.historical_surprises[1:],
            ),
        ),
        lambda: replace(
            bundle,
            historical_surprises=(current_history, *bundle.historical_surprises[1:]),
        ),
        lambda: replace(
            bundle,
            historical_surprises=(future_history, *bundle.historical_surprises[1:]),
        ),
        lambda: replace(
            replace(
                bundle,
                release=None,
                sources=tuple(
                    item
                    for item in bundle.sources
                    if item.kind
                    not in {
                        MacroSourceKind.OFFICIAL_RELEASE,
                        MacroSourceKind.DATA_PROVIDER,
                    }
                ),
            ),
            market_snapshot=bundle.market_snapshot,
        ),
        lambda: _bundle(official_delay=-1),
        lambda: replace(
            bundle,
            release=replace(
                release,
                official_publication_wall_clock_utc_ns=(
                    release.official_publication_wall_clock_utc_ns + 1
                ),
            ),
        ),
    )
    with pytest.raises(ValueError, match="release-time validation"):
        scheduled._validate_release_time(
            {item.source_id: item for item in scheduled.sources},
            SCHEDULED,
        )


def test_release_observation_prior_and_market_window_validation() -> None:
    bundle = _bundle()
    if bundle.release is None or bundle.market_snapshot is None:
        raise AssertionError
    release = bundle.release
    first = release.observations[0]
    provider_source = next(
        item for item in bundle.sources if item.source_id == "provider"
    )
    bad_provider_sources = tuple(
        replace(
            provider_source,
            publication_wall_clock_utc_ns=SCHEDULED - 2,
            received_wall_clock_utc_ns=SCHEDULED - 1,
        )
        if item.source_id == "provider"
        else item
        for item in bundle.sources
    )
    wrong_event_observation = replace(
        first,
        identity=replace(first.identity, event_type=MacroEventType.PPI),
    )
    late_prior = replace(
        release.prior_values[0],
        known_available_wall_clock_utc_ns=release.official_publication_wall_clock_utc_ns,
    )
    wrong_prior_event = replace(
        release.prior_values[0],
        identity=replace(
            release.prior_values[0].identity,
            event_type=MacroEventType.PPI,
        ),
    )
    receipt = release.receipt_wall_clock_utc_ns
    snapshot = bundle.market_snapshot
    observations = snapshot.observations

    _assert_invalid(
        lambda: replace(
            bundle,
            release=replace(
                release,
                observations=(
                    replace(first, source_id="unknown"),
                    *release.observations[1:],
                ),
            ),
        ),
        lambda: replace(
            bundle,
            release=replace(
                release,
                observations=(
                    replace(
                        first,
                        source_id="market",
                        evidence=_reference("market"),
                    ),
                    *release.observations[1:],
                ),
            ),
        ),
        lambda: replace(bundle, sources=bad_provider_sources),
        lambda: replace(
            bundle,
            release=replace(
                release,
                observations=(wrong_event_observation, *release.observations[1:]),
            ),
        ),
        lambda: replace(bundle, release=replace(release, prior_values=(late_prior,))),
        lambda: replace(
            bundle,
            release=replace(release, prior_values=(wrong_prior_event,)),
        ),
        lambda: replace(
            bundle,
            market_snapshot=replace(
                snapshot,
                available_wall_clock_utc_ns=receipt - 1,
            ),
        ),
        lambda: replace(
            bundle,
            as_of_wall_clock_utc_ns=snapshot.available_wall_clock_utc_ns - 1,
        ),
        lambda: replace(
            bundle,
            market_snapshot=replace(
                snapshot,
                observations=(
                    replace(
                        observations[0],
                        window_start_wall_clock_utc_ns=receipt,
                    ),
                    *observations[1:],
                ),
            ),
        ),
        lambda: replace(
            bundle,
            market_snapshot=replace(
                snapshot,
                observations=(
                    replace(
                        observations[0],
                        window_end_wall_clock_utc_ns=receipt - 1,
                    ),
                    *observations[1:],
                ),
            ),
        ),
        lambda: replace(
            bundle,
            market_snapshot=replace(
                snapshot,
                observations=(
                    replace(
                        observations[0],
                        window_end_wall_clock_utc_ns=(
                            snapshot.available_wall_clock_utc_ns + 1
                        ),
                    ),
                    *observations[1:],
                ),
            ),
        ),
    )

    object.__setattr__(bundle, "as_of_wall_clock_utc_ns", receipt - 1)
    with pytest.raises(ValueError, match="after the as-of"):
        bundle._validate_release_time(
            {item.source_id: item for item in bundle.sources},
            SCHEDULED,
        )
    object.__setattr__(scheduled := _bundle(release=False), "market_snapshot", snapshot)
    with pytest.raises(ValueError, match="requires a release"):
        scheduled._validate_market_time()


def test_result_contract_validation_rejects_inconsistent_states() -> None:
    published = _specialist().evaluate(_bundle())
    abstained = _specialist().evaluate(_bundle(release=False))
    if published.forecast_id is None or published.forecast_contract_bytes is None:
        raise AssertionError

    _assert_invalid(
        lambda: replace(published, input_sha256=bytes(32)),
        lambda: replace(published, consensus_snapshot_sha256=b"short"),
        lambda: replace(published, release_sha256=bytes(32)),
        lambda: replace(published, revision=-1),
        lambda: replace(published, schema_version="2.0.0"),
        lambda: replace(published, scheduled_release_wall_clock_utc_ns=0),
        lambda: replace(published, source_quality_score_ppm=-1),
        lambda: replace(
            published,
            reason_codes=(MacroReasonCode.DELAYED_RELEASE,) * 2,
        ),
        lambda: replace(published, surprise_percentile_ppm=-1),
        lambda: replace(published, production_process_monotonic_time_ns=0),
        lambda: replace(published, revision=0),
        lambda: replace(published, receipt_wall_clock_utc_ns=None),
        lambda: replace(
            published,
            receipt_wall_clock_utc_ns=cast(
                "int", published.official_publication_wall_clock_utc_ns
            )
            - 1,
        ),
        lambda: replace(published, release_delay_ns=-1),
        lambda: replace(abstained, phase=MacroEventPhase.RELEASE_COMPLETE),
        lambda: replace(abstained, forecast_id=ForecastId(_id(999))),
        lambda: replace(published, forecast_id=None),
        lambda: replace(published, phase=MacroEventPhase.PARTIAL_RELEASE),
        lambda: replace(published, expected_return_ppm=10_000_001),
        lambda: replace(published, expected_volatility_ppm=-1),
        lambda: replace(
            published,
            valid_until_process_monotonic_time_ns=published.production_process_monotonic_time_ns,
        ),
        lambda: replace(published, forecast_contract_bytes=b"not-a-contract"),
    )


def test_specialist_internal_impossible_publish_guards() -> None:
    specialist = _specialist()
    bundle = _bundle()
    scheduled = _bundle(release=False)
    if bundle.release is None:
        raise AssertionError
    no_revision = replace(
        bundle,
        release=replace(
            bundle.release,
            prior_values=(
                replace(
                    bundle.release.prior_values[0],
                    revised_value_in_release=None,
                ),
            ),
        ),
    )
    assert specialist.evaluate(no_revision).revision_surprises == ()
    assert specialist._target_response(scheduled) == 0

    result = specialist.evaluate(bundle)
    analysis = specialist_module._MacroAnalysis(
        result.phase,
        result.headline_surprise,
        result.core_surprise,
        result.subcomponent_surprises,
        result.revision_surprises,
        result.surprise_percentile_ppm,
        result.cross_asset_response_features,
        result.source_quality_score_ppm,
    )
    with pytest.raises(MacroEvaluationError, match="market feature provenance"):
        specialist._publish_values(scheduled, analysis)
    with pytest.raises(MacroEvaluationError, match="surprise percentile"):
        specialist._publish_values(bundle, replace(analysis, percentile=None))
