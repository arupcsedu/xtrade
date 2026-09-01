"""Deterministic and adversarial tests for the earnings-event specialist."""

from __future__ import annotations

import json
from dataclasses import replace
from pathlib import Path
from typing import TYPE_CHECKING, Any, cast

import aegis_mx_intelligence.earnings_specialist as specialist_module
import aegis_mx_intelligence.earnings_types as types_module
import pytest
from aegis.mx.contracts.v1.ContractPayload import ContractPayload
from aegis.mx.contracts.v1.ContractRecord import ContractRecord
from aegis.mx.contracts.v1.ModelForecast import ModelForecast
from aegis.mx.contracts.v1.RecordType import RecordType
from aegis_mx_intelligence import (
    AccountingBasis,
    AccountingQualityFlag,
    ConfigurationVersion,
    DirectionDistribution,
    EarningsEvaluationError,
    EarningsEventLifecycle,
    EarningsEvidenceReference,
    EarningsInputBundle,
    EarningsLifecycleTransition,
    EarningsMarketFeatures,
    EarningsMetricIdentity,
    EarningsMetricKind,
    EarningsMetricUnit,
    EarningsPhase,
    EarningsReasonCode,
    EarningsSourceEvidence,
    EarningsSourceKind,
    EarningsSpecialist,
    EarningsSpecialistConfig,
    EstimateDistribution,
    EvidenceExcerpt,
    ExpectedGapRange,
    FeatureSnapshotId,
    GlobalEventId,
    GuidanceRange,
    HistoricalEarningsReaction,
    Identifier128,
    InstrumentId,
    ModelId,
    ModelVersion,
    NormalizedEarningsMetric,
    OneTimeAdjustment,
    OptionImpliedMove,
    SessionId,
    sha256_bytes,
)

if TYPE_CHECKING:
    from collections.abc import Callable

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
FIXTURE_DIRECTORY = REPOSITORY_ROOT / "python/tests/fixtures/earnings"
RELEASE_TIME = 1_800_000_000_000_000_000
AS_OF_TIME = RELEASE_TIME + 1_000_000_000
PRODUCTION_TIME = 5_000_000
PERIOD = "FY2026-Q2"


def _id(value: int) -> Identifier128:
    return Identifier128(value, value + 1)


def _identity(
    kind: EarningsMetricKind,
    basis: AccountingBasis,
    unit: EarningsMetricUnit,
    segment: str = "",
    period: str = PERIOD,
) -> EarningsMetricIdentity:
    return EarningsMetricIdentity(kind, basis, unit, period, segment)


def _reference(
    source_id: str, excerpt_id: int = 1
) -> tuple[EarningsEvidenceReference, ...]:
    return (EarningsEvidenceReference(source_id, excerpt_id),)


def _source(
    source_id: str,
    kind: EarningsSourceKind,
    *,
    publication: int = RELEASE_TIME,
    received: int | None = None,
    excerpt_ids: tuple[int, ...] = (1,),
) -> EarningsSourceEvidence:
    selected_received = publication + 10 if received is None else received
    return EarningsSourceEvidence(
        source_id=source_id,
        source_global_event_id=GlobalEventId(
            Identifier128(
                int.from_bytes(sha256_bytes(source_id)[:8], "big") or 1,
                int.from_bytes(sha256_bytes(source_id)[8:16], "big"),
            )
        ),
        kind=kind,
        provider_id=f"{source_id}-provider",
        document_id=f"{source_id}-document",
        source_content_sha256=sha256_bytes(f"{source_id}-raw"),
        sanitized_content_sha256=sha256_bytes(f"{source_id}-sanitized"),
        publication_wall_clock_utc_ns=publication,
        received_wall_clock_utc_ns=selected_received,
        excerpts=tuple(
            EvidenceExcerpt(
                excerpt_id,
                (excerpt_id - 1) * 20,
                ((excerpt_id - 1) * 20) + len(f"{source_id} evidence {excerpt_id}"),
                f"{source_id} evidence {excerpt_id}",
            )
            for excerpt_id in excerpt_ids
        ),
    )


def _metric(
    identity: EarningsMetricIdentity,
    value: int,
    source_id: str = "release",
) -> NormalizedEarningsMetric:
    return NormalizedEarningsMetric(identity, value, _reference(source_id))


def _estimate(
    identity: EarningsMetricIdentity,
    consensus: int,
    dispersion: int,
    *,
    source_id: str = "consensus",
    available: int = RELEASE_TIME - 100,
) -> EstimateDistribution:
    width = max(1, dispersion * 2)
    return EstimateDistribution(
        identity,
        consensus,
        dispersion,
        consensus - width,
        consensus + width,
        12,
        available,
        _reference(source_id),
    )


def _guidance(
    identity: EarningsMetricIdentity,
    lower: int,
    upper: int,
) -> GuidanceRange:
    return GuidanceRange(identity, lower, upper, _reference("guidance"))


def _history(index: int) -> HistoricalEarningsReaction:
    return HistoricalEarningsReaction(
        f"historical-{index}",
        RELEASE_TIME - ((index + 2) * 10_000),
        RELEASE_TIME - ((index + 1) * 10_000),
        (-1 if index % 2 else 1) * (40_000 + (index * 10_000)),
        100_000 + (index * 20_000),
        900_000_000_000 + (index * 300_000_000_000),
    )


def _sources(*, include_optional: bool = True) -> tuple[EarningsSourceEvidence, ...]:
    required = (
        _source("release", EarningsSourceKind.OFFICIAL_EARNINGS_RELEASE),
        _source(
            "consensus",
            EarningsSourceKind.CONSENSUS_ESTIMATES,
            publication=RELEASE_TIME - 200,
            received=RELEASE_TIME - 100,
        ),
        _source(
            "prior",
            EarningsSourceKind.PRIOR_REPORTED_VALUES,
            publication=RELEASE_TIME - 20_000,
            received=RELEASE_TIME - 19_000,
        ),
        _source(
            "guidance",
            EarningsSourceKind.COMPANY_GUIDANCE,
            publication=RELEASE_TIME,
        ),
        _source(
            "option",
            EarningsSourceKind.OPTION_IMPLIED_MOVE,
            publication=RELEASE_TIME - 300,
            received=RELEASE_TIME - 200,
        ),
        _source("segments", EarningsSourceKind.SEGMENT_RESULTS),
    )
    if not include_optional:
        return required
    return (
        *required,
        _source("filing", EarningsSourceKind.REGULATORY_FILING),
        _source("transcript", EarningsSourceKind.TRANSCRIPT),
        _source(
            "history",
            EarningsSourceKind.HISTORICAL_REACTION,
            publication=RELEASE_TIME - 50_000,
            received=RELEASE_TIME - 40_000,
        ),
        _source(
            "features",
            EarningsSourceKind.POST_RELEASE_MARKET_FEATURES,
            publication=RELEASE_TIME + 50,
            received=RELEASE_TIME + 100,
        ),
    )


def _bundle(**changes: object) -> EarningsInputBundle:
    eps_gaap = _identity(
        EarningsMetricKind.EPS,
        AccountingBasis.GAAP,
        EarningsMetricUnit.CURRENCY_NANOS_PER_SHARE,
    )
    eps_non_gaap = replace(eps_gaap, basis=AccountingBasis.NON_GAAP)
    revenue = _identity(
        EarningsMetricKind.REVENUE,
        AccountingBasis.GAAP,
        EarningsMetricUnit.CURRENCY_NANOS,
    )
    free_cash_flow = _identity(
        EarningsMetricKind.FREE_CASH_FLOW,
        AccountingBasis.GAAP,
        EarningsMetricUnit.CURRENCY_NANOS,
    )
    capex = _identity(
        EarningsMetricKind.CAPEX,
        AccountingBasis.GAAP,
        EarningsMetricUnit.CURRENCY_NANOS,
    )
    segment_compute = _identity(
        EarningsMetricKind.SEGMENT_REVENUE,
        AccountingBasis.GAAP,
        EarningsMetricUnit.CURRENCY_NANOS,
        "Compute",
    )
    segment_services = replace(segment_compute, segment_name="Services")
    bundle = EarningsInputBundle(
        earnings_event_id=GlobalEventId(_id(10)),
        session_id=SessionId(_id(20)),
        instrument_id=InstrumentId(_id(30)),
        configuration_version=ConfigurationVersion(_id(40)),
        revision=1,
        as_of_wall_clock_utc_ns=AS_OF_TIME,
        production_process_monotonic_time_ns=PRODUCTION_TIME,
        sources=_sources(),
        actuals=(
            _metric(eps_gaap, 2_500_000_000),
            _metric(eps_non_gaap, 2_900_000_000),
            _metric(revenue, 1_200_000_000_000_000),
            _metric(free_cash_flow, 180_000_000_000_000, "filing"),
            _metric(capex, 90_000_000_000_000, "filing"),
            _metric(segment_compute, 650_000_000_000_000, "segments"),
            _metric(segment_services, 450_000_000_000_000, "segments"),
        ),
        consensus=(
            _estimate(eps_gaap, 2_300_000_000, 100_000_000),
            _estimate(eps_non_gaap, 2_700_000_000, 100_000_000),
            _estimate(revenue, 1_150_000_000_000_000, 25_000_000_000_000),
        ),
        prior_reported_values=(
            _metric(eps_gaap, 2_000_000_000, "prior"),
            _metric(revenue, 1_000_000_000_000_000, "prior"),
        ),
        current_guidance=(
            _guidance(revenue, 1_300_000_000_000_000, 1_400_000_000_000_000),
        ),
        prior_guidance=(
            _guidance(revenue, 1_200_000_000_000_000, 1_300_000_000_000_000),
        ),
        one_time_adjustments=(
            OneTimeAdjustment(
                "restructuring",
                150_000_000_000_000,
                _reference("filing"),
            ),
        ),
        historical_reactions=(_history(0), _history(1), _history(2)),
        market_features=EarningsMarketFeatures(
            feature_snapshot_id=FeatureSnapshotId(_id(50)),
            as_of_exchange_event_time_ns=RELEASE_TIME + 100,
            available_wall_clock_utc_ns=RELEASE_TIME + 100,
            return_since_release_ppm=60_000,
            realized_volatility_ppm=140_000,
            is_post_release=True,
        ),
        option_implied_move=OptionImpliedMove(
            80_000,
            RELEASE_TIME - 100,
            _reference("option"),
        ),
    )
    return replace(bundle, **cast("Any", changes))


def _config(**changes: object) -> EarningsSpecialistConfig:
    config = EarningsSpecialistConfig(ModelId(_id(60)), ModelVersion(_id(70)))
    return replace(config, **cast("Any", changes))


def _specialist(**changes: object) -> EarningsSpecialist:
    return EarningsSpecialist(_config(**changes))


def test_synthetic_fixture_and_common_forecast_contract() -> None:
    fixture = json.loads(
        (FIXTURE_DIRECTORY / "synthetic_earnings_v1.json").read_text(encoding="utf-8")
    )
    bundle = _bundle()
    result = _specialist().evaluate(bundle)
    surprises = {
        item.identity: item.surprise_ppm
        for item in result.surprises
        if item.surprise_ppm is not None
    }
    eps = _identity(
        EarningsMetricKind.EPS,
        AccountingBasis.GAAP,
        EarningsMetricUnit.CURRENCY_NANOS_PER_SHARE,
    )
    revenue = _identity(
        EarningsMetricKind.REVENUE,
        AccountingBasis.GAAP,
        EarningsMetricUnit.CURRENCY_NANOS,
    )
    assert fixture["seed"] == 20260828
    assert bundle.official_release.source_id == "release"
    assert surprises[eps] == fixture["hand_calculated"]["gaap_eps_surprise_ppm"]
    assert surprises[revenue] == fixture["hand_calculated"]["revenue_surprise_ppm"]
    assert result.guidance_changes[0].change_ppm == 80_000
    assert result.phase is EarningsPhase.PRICE_DISCOVERY
    assert (
        result.direction.down_ppm + result.direction.flat_ppm + result.direction.up_ppm
        == 1_000_000
    )
    assert result.expected_gap.lower_return_ppm <= result.expected_return_ppm
    assert result.expected_return_ppm <= result.expected_gap.upper_return_ppm
    assert result.materiality_ppm > 0
    assert result.expected_volatility_ppm > 0
    assert result.forecast_contract_bytes[4:8] == b"AMCR"

    contract = ContractRecord.GetRootAs(result.forecast_contract_bytes, 0)
    assert contract.RecordType() == RecordType.MODEL_FORECAST
    assert contract.PayloadType() == ContractPayload.ModelForecast
    table = contract.Payload()
    assert table is not None
    forecast = ModelForecast()
    forecast.Init(table.Bytes, table.Pos)
    assert forecast.ProbabilityDownPpm() == result.direction.down_ppm
    assert forecast.ProbabilityFlatPpm() == result.direction.flat_ppm
    assert forecast.ProbabilityUpPpm() == result.direction.up_ppm
    assert forecast.ExpectedReturnPpm() == result.expected_return_ppm
    assert forecast.VolatilityPpm() == result.expected_volatility_ppm
    assert forecast.FeatureSnapshotId() is not None


def test_manual_fixture_missing_and_incompatible_metrics_are_explicit() -> None:
    fixture = json.loads(
        (FIXTURE_DIRECTORY / "manual_curated_earnings_v1.json").read_text(
            encoding="utf-8"
        )
    )
    eps_gaap = _identity(
        EarningsMetricKind.EPS,
        AccountingBasis.GAAP,
        EarningsMetricUnit.CURRENCY_NANOS_PER_SHARE,
        period="FY2026-Q3",
    )
    revenue = _identity(
        EarningsMetricKind.REVENUE,
        AccountingBasis.GAAP,
        EarningsMetricUnit.CURRENCY_NANOS,
        period="FY2026-Q3",
    )
    incompatible = replace(eps_gaap, basis=AccountingBasis.NON_GAAP)
    bundle = _bundle(
        sources=_sources(include_optional=False),
        actuals=(
            _metric(eps_gaap, 1_500_000_000),
            _metric(revenue, 800_000_000_000_000),
        ),
        consensus=(_estimate(incompatible, 1_600_000_000, 50_000_000),),
        prior_reported_values=(_metric(eps_gaap, 1_400_000_000, "prior"),),
        current_guidance=(
            _guidance(revenue, 780_000_000_000_000, 820_000_000_000_000),
        ),
        prior_guidance=(),
        one_time_adjustments=(),
        historical_reactions=(),
        market_features=replace(
            _bundle().market_features,
            is_post_release=False,
            return_since_release_ppm=0,
            realized_volatility_ppm=0,
            available_wall_clock_utc_ns=RELEASE_TIME - 1,
            as_of_exchange_event_time_ns=RELEASE_TIME - 1,
        ),
        option_implied_move=None,
    )
    result = _specialist().evaluate(bundle)
    assert fixture["fixture_type"] == "manual-curated-fictional"
    assert result.surprises[0].reason is EarningsReasonCode.INCOMPATIBLE_METRIC
    assert result.surprises[1].reason is EarningsReasonCode.MISSING_CONSENSUS
    assert (
        result.guidance_changes[0].reason is EarningsReasonCode.MISSING_PRIOR_GUIDANCE
    )
    assert EarningsReasonCode.NO_COMPARABLE_METRICS in result.reason_codes
    assert EarningsReasonCode.MISSING_FILING in result.reason_codes
    assert EarningsReasonCode.MISSING_TRANSCRIPT in result.reason_codes
    assert EarningsReasonCode.MISSING_OPTION_IMPLIED_MOVE in result.reason_codes
    assert EarningsReasonCode.INSUFFICIENT_HISTORY in result.reason_codes
    assert EarningsReasonCode.MISSING_POST_RELEASE_FEATURES in result.reason_codes
    assert result.phase is EarningsPhase.RELEASE_PROCESSING


def test_zero_dispersion_uses_unit_epsilon_and_replay_is_byte_stable() -> None:
    bundle = _bundle()
    estimate = replace(bundle.consensus[0], consensus_value=2_499_999_000, dispersion=0)
    bundle = replace(bundle, consensus=(estimate, *bundle.consensus[1:]))
    specialist = _specialist(epsilon_currency_nanos_per_share=1_000)
    result = specialist.evaluate(bundle)
    assert result.surprises[0].surprise_ppm == 1_000_000
    assert specialist.verify_replay(bundle, result.sha256) == result
    assert (
        specialist.configuration_sha256
        == _config(epsilon_currency_nanos_per_share=1_000).sha256()
    )
    assert bundle.canonical_bytes() == bundle.canonical_bytes()
    assert bundle.sha256 == sha256_bytes(bundle.canonical_bytes())
    assert result.canonical_bytes() == result.canonical_bytes()


def test_replay_rejects_bad_expected_digest() -> None:
    specialist = _specialist()
    bundle = _bundle()
    with pytest.raises(EarningsEvaluationError, match="32 bytes"):
        specialist.verify_replay(bundle, b"short")
    with pytest.raises(EarningsEvaluationError, match="mismatch"):
        specialist.verify_replay(bundle, bytes(32))


def test_corrected_release_is_linked_and_flagged() -> None:
    original = _bundle()
    corrected = replace(
        original,
        revision=2,
        correction_of_input_sha256=original.sha256,
        actuals=(
            replace(original.actuals[0], value=2_600_000_000),
            *original.actuals[1:],
        ),
        production_process_monotonic_time_ns=PRODUCTION_TIME + 1,
    )
    result = _specialist().evaluate(corrected)
    assert result.revision == 2
    assert result.phase is EarningsPhase.PRICE_DISCOVERY
    assert AccountingQualityFlag.CORRECTED_RELEASE in result.accounting_quality_flags
    assert EarningsReasonCode.CORRECTED_RELEASE in result.reason_codes
    assert result.input_sha256 == corrected.sha256


def test_quality_flags_are_deterministic_diagnostics() -> None:
    result = _specialist().evaluate(_bundle())
    assert result.accounting_quality_flags == (
        AccountingQualityFlag.GAAP_NON_GAAP_DIVERGENCE,
        AccountingQualityFlag.MATERIAL_ONE_TIME_ADJUSTMENTS,
        AccountingQualityFlag.SEGMENT_TOTAL_MISMATCH,
    )
    assert EarningsReasonCode.GAAP_NON_GAAP_DIVERGENCE in result.reason_codes
    assert EarningsReasonCode.MATERIAL_ONE_TIME_ADJUSTMENTS in result.reason_codes
    assert EarningsReasonCode.SEGMENT_TOTAL_MISMATCH in result.reason_codes


def test_quality_clean_paths_and_missing_free_cash_flow() -> None:
    bundle = _bundle()
    revenue = bundle.actuals[2]
    free_cash_flow = bundle.actuals[3]
    clean = replace(
        bundle,
        actuals=(revenue, free_cash_flow),
        consensus=(bundle.consensus[2],),
        prior_reported_values=(bundle.prior_reported_values[1],),
        one_time_adjustments=(),
    )
    assert _specialist().evaluate(clean).accounting_quality_flags == ()

    without_revenue_or_fcf = replace(
        bundle,
        actuals=(bundle.actuals[4],),
        consensus=(),
        prior_reported_values=(),
        current_guidance=(),
        prior_guidance=(),
        one_time_adjustments=(),
    )
    result = _specialist().evaluate(without_revenue_or_fcf)
    assert result.accounting_quality_flags == (
        AccountingQualityFlag.MISSING_FREE_CASH_FLOW,
    )
    assert EarningsReasonCode.MISSING_FREE_CASH_FLOW in result.reason_codes


def test_negative_signals_and_fallback_volatility_are_bounded() -> None:
    bundle = _bundle()
    actuals = tuple(
        replace(item, value=-abs(item.value))
        if item.identity.kind is EarningsMetricKind.EPS
        else item
        for item in bundle.actuals
    )
    sparse = replace(
        bundle,
        actuals=actuals,
        historical_reactions=(),
        option_implied_move=None,
        market_features=replace(
            bundle.market_features,
            return_since_release_ppm=-500_000,
            realized_volatility_ppm=0,
        ),
    )
    result = _specialist().evaluate(sparse)
    assert result.expected_return_ppm < 0
    assert result.direction.down_ppm > result.direction.up_ppm
    assert result.expected_volatility_ppm > 0


@pytest.mark.parametrize(
    ("config_changes", "message"),
    [
        ({"forecast_ttl_ns": 0}, "durations"),
        ({"default_discovery_duration_ns": 1}, "outside"),
        ({"minimum_history_count": 0}, "history"),
        ({"gaap_divergence_threshold_ppm": 1_000_001}, "thresholds"),
        ({"epsilon_ppm": 0}, "epsilon"),
    ],
)
def test_specialist_configuration_rejects_invalid_values(
    config_changes: dict[str, int], message: str
) -> None:
    with pytest.raises(ValueError, match=message):
        _config(**config_changes)


@pytest.mark.parametrize(
    "unit",
    list(EarningsMetricUnit),
)
def test_every_metric_unit_has_an_explicit_epsilon(unit: EarningsMetricUnit) -> None:
    assert _config().epsilon(unit) > 0


@pytest.mark.parametrize(
    ("constructor", "message"),
    [
        (
            lambda: EarningsMetricIdentity(
                EarningsMetricKind.EPS,
                AccountingBasis.GAAP,
                EarningsMetricUnit.PPM,
                "",
            ),
            "fiscal_period",
        ),
        (
            lambda: EarningsMetricIdentity(
                EarningsMetricKind.SEGMENT_REVENUE,
                AccountingBasis.GAAP,
                EarningsMetricUnit.CURRENCY_NANOS,
                PERIOD,
            ),
            "segment metrics",
        ),
        (
            lambda: EarningsMetricIdentity(
                EarningsMetricKind.REVENUE,
                AccountingBasis.GAAP,
                EarningsMetricUnit.CURRENCY_NANOS,
                PERIOD,
                "unexpected",
            ),
            "segment metrics",
        ),
        (lambda: EarningsEvidenceReference("", 1), "source_id"),
        (lambda: EarningsEvidenceReference("source", 0), "positive"),
        (
            lambda: DirectionDistribution(1, 1, 1),
            "sum",
        ),
        (
            lambda: ExpectedGapRange(1, 0, 2),
            "range",
        ),
    ],
)
def test_small_contracts_reject_invalid_values(
    constructor: Callable[[], object], message: str
) -> None:
    with pytest.raises(ValueError, match=message):
        constructor()


def test_source_evidence_validation() -> None:
    source = _source("valid", EarningsSourceKind.REGULATORY_FILING)
    with pytest.raises(ValueError, match="digest"):
        replace(source, source_content_sha256=bytes(32))
    with pytest.raises(ValueError, match="precedes"):
        replace(source, received_wall_clock_utc_ns=RELEASE_TIME - 1)
    with pytest.raises(ValueError, match="one to sixteen"):
        replace(source, excerpts=())
    with pytest.raises(ValueError, match="unique"):
        replace(source, excerpts=(source.excerpts[0], source.excerpts[0]))


def test_metric_estimate_guidance_adjustment_and_option_validation() -> None:
    identity = _identity(
        EarningsMetricKind.REVENUE,
        AccountingBasis.GAAP,
        EarningsMetricUnit.CURRENCY_NANOS,
    )
    reference = _reference("release")
    with pytest.raises(ValueError, match="signed 64-bit"):
        NormalizedEarningsMetric(identity, 1 << 63, reference)
    with pytest.raises(ValueError, match="one to sixteen"):
        NormalizedEarningsMetric(identity, 1, ())
    with pytest.raises(ValueError, match="unique"):
        NormalizedEarningsMetric(identity, 1, reference * 2)
    with pytest.raises(ValueError, match="range"):
        EstimateDistribution(identity, 5, 1, 6, 7, 1, 1, reference)
    with pytest.raises(ValueError, match="range"):
        EstimateDistribution(identity, 5, -1, 4, 6, 1, 1, reference)
    with pytest.raises(ValueError, match="positive"):
        replace(_estimate(identity, 5, 1), available_wall_clock_utc_ns=0)
    with pytest.raises(ValueError, match="exceeds"):
        GuidanceRange(identity, 2, 1, reference)
    with pytest.raises(ValueError, match="adjustment label"):
        OneTimeAdjustment("", 1, reference)
    with pytest.raises(ValueError, match="signed 64-bit"):
        OneTimeAdjustment("label", 1 << 63, reference)
    with pytest.raises(ValueError, match="PPM bound"):
        OptionImpliedMove(0, 1, reference)
    with pytest.raises(ValueError, match="positive"):
        OptionImpliedMove(1, 0, reference)


@pytest.mark.parametrize(
    ("changes", "message"),
    [
        ({"historical_event_id": ""}, "historical_event_id"),
        ({"publication_wall_clock_utc_ns": 0}, "positive"),
        ({"available_wall_clock_utc_ns": 1}, "availability"),
        ({"gap_return_ppm": 10_000_001}, "gap"),
        ({"realized_volatility_ppm": 10_000_001}, "volatility"),
        ({"price_discovery_duration_ns": 0}, "positive"),
    ],
)
def test_historical_reaction_validation(
    changes: dict[str, object], message: str
) -> None:
    with pytest.raises(ValueError, match=message):
        replace(_history(0), **cast("Any", changes))


@pytest.mark.parametrize(
    ("changes", "message"),
    [
        ({"as_of_exchange_event_time_ns": 0}, "positive"),
        ({"available_wall_clock_utc_ns": 0}, "positive"),
        ({"return_since_release_ppm": 10_000_001}, "return"),
        ({"realized_volatility_ppm": 10_000_001}, "volatility"),
    ],
)
def test_market_feature_validation(changes: dict[str, object], message: str) -> None:
    with pytest.raises(ValueError, match=message):
        replace(_bundle().market_features, **cast("Any", changes))


def test_bundle_schema_revision_bounds_and_required_sources() -> None:
    bundle = _bundle()
    with pytest.raises(ValueError, match="schema"):
        replace(bundle, schema_version="2.0.0")
    with pytest.raises(ValueError, match="revision"):
        replace(bundle, revision=0)
    with pytest.raises(ValueError, match="corrected"):
        replace(bundle, revision=2)
    with pytest.raises(ValueError, match="corrected"):
        replace(bundle, correction_of_input_sha256=sha256_bytes("parent"))
    with pytest.raises(ValueError, match="digest"):
        replace(bundle, revision=2, correction_of_input_sha256=bytes(32))
    with pytest.raises(ValueError, match="as_of"):
        replace(bundle, as_of_wall_clock_utc_ns=0)
    with pytest.raises(ValueError, match="production"):
        replace(bundle, production_process_monotonic_time_ns=0)
    with pytest.raises(ValueError, match="actual"):
        replace(bundle, actuals=())
    with pytest.raises(ValueError, match="sources"):
        replace(bundle, sources=bundle.sources * 4)
    with pytest.raises(ValueError, match="exactly one"):
        replace(
            bundle,
            sources=tuple(
                source
                for source in bundle.sources
                if source.kind is not EarningsSourceKind.OFFICIAL_EARNINGS_RELEASE
            ),
        )
    with pytest.raises(ValueError, match="unique"):
        replace(bundle, sources=(bundle.sources[0], bundle.sources[0]))


def test_bundle_collection_bounds_and_duplicate_metrics() -> None:
    bundle = _bundle()
    with pytest.raises(ValueError, match="actuals"):
        replace(bundle, actuals=(bundle.actuals[0],) * 129)
    with pytest.raises(ValueError, match="duplicate actual"):
        replace(bundle, actuals=(bundle.actuals[0], bundle.actuals[0]))
    with pytest.raises(ValueError, match="duplicate consensus"):
        replace(bundle, consensus=(bundle.consensus[0], bundle.consensus[0]))
    with pytest.raises(ValueError, match="duplicate prior"):
        replace(
            bundle,
            prior_reported_values=(
                bundle.prior_reported_values[0],
                bundle.prior_reported_values[0],
            ),
        )
    with pytest.raises(ValueError, match="duplicate current"):
        replace(
            bundle,
            current_guidance=(bundle.current_guidance[0],) * 2,
        )
    with pytest.raises(ValueError, match="duplicate prior guidance"):
        replace(bundle, prior_guidance=(bundle.prior_guidance[0],) * 2)


def test_bundle_evidence_graph_rejects_missing_source_or_excerpt() -> None:
    bundle = _bundle()
    invalid_reference = EarningsEvidenceReference("absent", 1)
    with pytest.raises(ValueError, match="does not resolve"):
        replace(
            bundle,
            actuals=(replace(bundle.actuals[0], evidence=(invalid_reference,)),),
        )
    invalid_excerpt = EarningsEvidenceReference("option", 99)
    with pytest.raises(ValueError, match="option-implied"):
        replace(
            bundle,
            option_implied_move=replace(
                cast("OptionImpliedMove", bundle.option_implied_move),
                evidence=(invalid_excerpt,),
            ),
        )


@pytest.mark.parametrize(
    ("mutator", "message"),
    [
        (
            lambda bundle: replace(
                bundle,
                sources=(
                    replace(
                        bundle.sources[0],
                        received_wall_clock_utc_ns=AS_OF_TIME + 1,
                    ),
                    *bundle.sources[1:],
                ),
            ),
            "unavailable",
        ),
        (
            lambda bundle: replace(
                bundle,
                market_features=replace(
                    bundle.market_features,
                    available_wall_clock_utc_ns=AS_OF_TIME + 1,
                ),
            ),
            "unavailable",
        ),
        (
            lambda bundle: replace(
                bundle,
                consensus=(
                    replace(
                        bundle.consensus[0],
                        available_wall_clock_utc_ns=RELEASE_TIME + 1,
                    ),
                ),
            ),
            "consensus became",
        ),
        (
            lambda bundle: replace(
                bundle,
                sources=tuple(
                    replace(source, received_wall_clock_utc_ns=RELEASE_TIME + 1)
                    if source.source_id == "consensus"
                    else source
                    for source in bundle.sources
                ),
            ),
            "consensus evidence",
        ),
        (
            lambda bundle: replace(
                bundle,
                option_implied_move=replace(
                    cast("OptionImpliedMove", bundle.option_implied_move),
                    available_wall_clock_utc_ns=RELEASE_TIME + 1,
                ),
            ),
            "option-implied move",
        ),
        (
            lambda bundle: replace(
                bundle,
                sources=tuple(
                    replace(source, received_wall_clock_utc_ns=RELEASE_TIME + 1)
                    if source.source_id == "option"
                    else source
                    for source in bundle.sources
                ),
            ),
            "option-implied evidence",
        ),
        (
            lambda bundle: replace(
                bundle,
                historical_reactions=(
                    replace(
                        bundle.historical_reactions[0],
                        publication_wall_clock_utc_ns=RELEASE_TIME,
                        available_wall_clock_utc_ns=RELEASE_TIME,
                    ),
                ),
            ),
            "historical reaction",
        ),
        (
            lambda bundle: replace(
                bundle,
                market_features=replace(
                    bundle.market_features,
                    available_wall_clock_utc_ns=RELEASE_TIME - 1,
                ),
            ),
            "post-release",
        ),
    ],
)
def test_future_data_leakage_is_rejected(
    mutator: Callable[[EarningsInputBundle], EarningsInputBundle], message: str
) -> None:
    with pytest.raises(ValueError, match=message):
        mutator(_bundle())


def test_invalid_market_features_and_expiration_overflow_fail_closed() -> None:
    bundle = _bundle()
    with pytest.raises(EarningsEvaluationError, match="features"):
        _specialist().evaluate(
            replace(
                bundle, market_features=replace(bundle.market_features, valid=False)
            )
        )
    with pytest.raises(EarningsEvaluationError, match="overflows"):
        _specialist().evaluate(
            replace(bundle, production_process_monotonic_time_ns=(1 << 64) - 1)
        )


class RecordingHook:
    """Bounded test hook recording proposed transitions."""

    def __init__(self, *, approved: bool = True) -> None:
        """Create a hook with one deterministic approval response."""
        self.approved = approved
        self.transitions: list[EarningsLifecycleTransition] = []

    def approve_transition(self, transition: EarningsLifecycleTransition) -> bool:
        """Record and return the configured response."""
        self.transitions.append(transition)
        return self.approved


def test_lifecycle_original_release_discovery_recovery_and_reset() -> None:
    hook = RecordingHook()
    lifecycle = EarningsEventLifecycle((hook,))
    result = _specialist().evaluate(_bundle())
    assert int(lifecycle.phase) == int(EarningsPhase.PRE_EARNINGS)
    lifecycle.accept_release(result)
    assert int(lifecycle.phase) == int(EarningsPhase.RELEASE_PROCESSING)
    lifecycle.begin_price_discovery(result)
    assert int(lifecycle.phase) == int(EarningsPhase.PRICE_DISCOVERY)
    assert not lifecycle.complete_price_discovery(
        result,
        result.production_process_monotonic_time_ns
        + result.estimated_price_discovery_duration_ns
        - 1,
    )
    assert lifecycle.complete_price_discovery(
        result,
        result.production_process_monotonic_time_ns
        + result.estimated_price_discovery_duration_ns,
    )
    assert int(lifecycle.phase) == int(EarningsPhase.RECOVERY)
    lifecycle.reset(result.valid_until_process_monotonic_time_ns + 1)
    assert int(lifecycle.phase) == int(EarningsPhase.PRE_EARNINGS)
    assert len(lifecycle.history) == 4
    assert len(hook.transitions) == 4


def test_lifecycle_correction_reenters_release_processing() -> None:
    specialist = _specialist()
    original_bundle = _bundle()
    original = specialist.evaluate(original_bundle)
    lifecycle = EarningsEventLifecycle()
    lifecycle.accept_release(original)
    lifecycle.begin_price_discovery(original)
    corrected_bundle = replace(
        original_bundle,
        revision=2,
        correction_of_input_sha256=original_bundle.sha256,
        production_process_monotonic_time_ns=PRODUCTION_TIME + 10,
    )
    corrected = specialist.evaluate(corrected_bundle)
    lifecycle.accept_release(corrected)
    assert int(lifecycle.phase) == int(EarningsPhase.RELEASE_PROCESSING)
    lifecycle.begin_price_discovery(corrected)
    assert int(lifecycle.phase) == int(EarningsPhase.PRICE_DISCOVERY)


def test_lifecycle_rejects_invalid_transitions_and_hooks() -> None:
    result = _specialist().evaluate(_bundle())
    with pytest.raises(ValueError, match="hook count"):
        EarningsEventLifecycle(tuple(RecordingHook() for _ in range(17)))
    rejected = EarningsEventLifecycle((RecordingHook(approved=False),))
    with pytest.raises(EarningsEvaluationError, match="hook rejected"):
        rejected.accept_release(result)
    assert rejected.phase is EarningsPhase.PRE_EARNINGS

    lifecycle = EarningsEventLifecycle()
    with pytest.raises(EarningsEvaluationError, match="price discovery"):
        lifecycle.begin_price_discovery(result)
    with pytest.raises(EarningsEvaluationError, match="recovery"):
        lifecycle.complete_price_discovery(result, PRODUCTION_TIME)
    with pytest.raises(EarningsEvaluationError, match="reset"):
        lifecycle.reset(PRODUCTION_TIME)
    lifecycle.accept_release(result)
    with pytest.raises(EarningsEvaluationError, match="stale"):
        lifecycle.accept_release(result)

    pre_release_result = _specialist().evaluate(
        replace(
            _bundle(),
            market_features=replace(
                _bundle().market_features,
                is_post_release=False,
                available_wall_clock_utc_ns=RELEASE_TIME - 1,
            ),
        )
    )
    separate = EarningsEventLifecycle()
    separate.accept_release(pre_release_result)
    with pytest.raises(EarningsEvaluationError, match="post-release"):
        separate.begin_price_discovery(pre_release_result)


def test_lifecycle_internal_bounds_and_timestamp_checks() -> None:
    result = _specialist().evaluate(_bundle())
    lifecycle = EarningsEventLifecycle()
    with pytest.raises(EarningsEvaluationError, match="positive"):
        lifecycle._transition(
            result.earnings_event_id,
            result.revision,
            EarningsPhase.RELEASE_PROCESSING,
            0,
        )
    transition = EarningsLifecycleTransition(
        result.earnings_event_id,
        result.revision,
        EarningsPhase.PRE_EARNINGS,
        EarningsPhase.RELEASE_PROCESSING,
        PRODUCTION_TIME,
    )
    lifecycle._history = [transition] * 32
    with pytest.raises(EarningsEvaluationError, match="history is full"):
        lifecycle.accept_release(result)


def test_numeric_helpers_and_canonical_type_failures() -> None:
    with pytest.raises(EarningsEvaluationError, match="denominator"):
        specialist_module._trunc_div(1, 0)
    assert specialist_module._trunc_div(-5, 2) == -2
    assert specialist_module._median([3]) == 3
    assert specialist_module._median([1, 3]) == 2
    with pytest.raises(EarningsEvaluationError, match="median"):
        specialist_module._median([])
    with pytest.raises(EarningsEvaluationError, match="signed 64-bit"):
        specialist_module._scaled_ratio_ppm(1 << 63, 1)
    with pytest.raises(TypeError, match="unsupported"):
        types_module._canonical_value(1.5)


def test_result_contract_validation() -> None:
    result = _specialist().evaluate(_bundle())
    with pytest.raises(ValueError, match="digest"):
        replace(result, input_sha256=bytes(32))
    with pytest.raises(ValueError, match="revision"):
        replace(result, revision=0)
    with pytest.raises(ValueError, match="materiality"):
        replace(result, materiality_ppm=1_000_001)
    with pytest.raises(ValueError, match="return"):
        replace(result, expected_return_ppm=10_000_001)
    with pytest.raises(ValueError, match="volatility"):
        replace(result, expected_volatility_ppm=10_000_001)
    with pytest.raises(ValueError, match="duration"):
        replace(result, estimated_price_discovery_duration_ns=0)
    with pytest.raises(ValueError, match="lifetime"):
        replace(
            result,
            valid_until_process_monotonic_time_ns=(
                result.production_process_monotonic_time_ns
            ),
        )
    with pytest.raises(ValueError, match="canonical"):
        replace(result, forecast_contract_bytes=b"invalid")
    with pytest.raises(ValueError, match="unique"):
        replace(
            result,
            reason_codes=(
                EarningsReasonCode.MISSING_FILING,
                EarningsReasonCode.MISSING_FILING,
            ),
        )
    if result.accounting_quality_flags:
        with pytest.raises(ValueError, match="unique"):
            replace(
                result,
                accounting_quality_flags=(
                    result.accounting_quality_flags[0],
                    result.accounting_quality_flags[0],
                ),
            )
