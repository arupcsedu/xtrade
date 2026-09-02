"""Adversarial tests for point-in-time storage and leakage rejection."""

from __future__ import annotations

import hashlib
from dataclasses import replace
from typing import TYPE_CHECKING

import pytest
from aegis_mx_research import (
    POINT_IN_TIME_SCHEMA_VERSION,
    AnalystEstimateVintage,
    CorporateAction,
    CorporateActionType,
    DatasetManifest,
    DatasetSample,
    DatasetSplit,
    DataSource,
    Delisting,
    FilingRevision,
    IndexMembership,
    LeakageCode,
    LeakageDetectedError,
    LeakageValidator,
    MacroVintage,
    NewsRevision,
    PointInTimeRecord,
    PointInTimeStore,
    RecordKind,
    SourceKind,
    SplitMethod,
    SymbolMapping,
    ValidityInterval,
)

if TYPE_CHECKING:
    from collections.abc import Callable

    from aegis_mx_research.types import RecordPayload

BASE = 1_000_000


def _source(source_id: str = "synthetic-source") -> DataSource:
    return DataSource(
        source_id=source_id,
        kind=SourceKind.SYNTHETIC_REPLAY,
        provider="aegis-fixture",
        document_id=f"document-{source_id}",
        content_sha256=hashlib.sha256(source_id.encode()).digest(),
        authenticated=True,
    )


def _logical_key(payload: RecordPayload) -> str:
    if isinstance(payload, CorporateAction):
        return payload.action_id
    if isinstance(payload, SymbolMapping):
        return payload.symbol
    if isinstance(payload, Delisting):
        return payload.instrument_id
    if isinstance(payload, IndexMembership):
        return f"{payload.index_id}:{payload.instrument_id}"
    if isinstance(payload, AnalystEstimateVintage):
        return f"{payload.instrument_id}:{payload.metric_id}:{payload.fiscal_period}"
    if isinstance(payload, MacroVintage):
        return f"{payload.series_id}:{payload.reference_period}"
    if isinstance(payload, NewsRevision):
        return payload.document_id
    return payload.accession_id


def _kind(payload: RecordPayload) -> RecordKind:
    if isinstance(payload, CorporateAction):
        return RecordKind.CORPORATE_ACTION
    if isinstance(payload, SymbolMapping):
        return RecordKind.SYMBOL_MAPPING
    if isinstance(payload, Delisting):
        return RecordKind.DELISTING
    if isinstance(payload, IndexMembership):
        return RecordKind.INDEX_MEMBERSHIP
    if isinstance(payload, AnalystEstimateVintage):
        return RecordKind.ANALYST_ESTIMATE
    if isinstance(payload, MacroVintage):
        return RecordKind.MACRO_VINTAGE
    if isinstance(payload, NewsRevision):
        return RecordKind.NEWS_REVISION
    return RecordKind.FILING_REVISION


def _record(
    payload: RecordPayload,
    *,
    record_id: str | None = None,
    version: int = 1,
    event_time_ns: int = 500,
    publication_time_ns: int = 100,
    receive_time_ns: int = 110,
    processing_time_ns: int = 120,
    revision_time_ns: int = 100,
    valid_from_ns: int = 1,
    valid_to_ns: int | None = None,
) -> PointInTimeRecord:
    key = _logical_key(payload)
    return PointInTimeRecord(
        record_id=record_id or f"{key}-v{version}",
        logical_key=key,
        kind=_kind(payload),
        event_time_ns=event_time_ns,
        publication_time_ns=publication_time_ns,
        receive_time_ns=receive_time_ns,
        processing_time_ns=processing_time_ns,
        revision_time_ns=revision_time_ns,
        validity=ValidityInterval(valid_from_ns, valid_to_ns),
        source=_source(f"{key}-{version}"),
        version=version,
        payload=payload,
    )


def _membership(
    instrument_id: str,
    *,
    record_id: str | None = None,
    available_ns: int = 120,
    valid_from_ns: int = 1,
    valid_to_ns: int | None = None,
    included: bool = True,
) -> PointInTimeRecord:
    return _record(
        IndexMembership("SYNTH-100", instrument_id, included),
        record_id=record_id,
        event_time_ns=valid_from_ns,
        publication_time_ns=available_ns - 20,
        receive_time_ns=available_ns - 10,
        processing_time_ns=available_ns,
        revision_time_ns=available_ns - 20,
        valid_from_ns=valid_from_ns,
        valid_to_ns=valid_to_ns,
    )


def _sample(
    sample_id: str,
    split: DatasetSplit,
    event_time_ns: int,
    record_ids: tuple[str, ...],
    universe: tuple[str, ...] = ("AAA-ID", "OLD-ID"),
) -> DatasetSample:
    return DatasetSample(
        sample_id=sample_id,
        event_time_ns=event_time_ns,
        feature_start_ns=event_time_ns - 100,
        feature_end_ns=event_time_ns - 50,
        label_start_ns=event_time_ns + 1,
        label_end_ns=event_time_ns + 20,
        split=split,
        record_ids=record_ids,
        universe_instrument_ids=universe,
    )


def _clean_store_and_manifest() -> tuple[PointInTimeStore, DatasetManifest]:
    store = PointInTimeStore(32)
    membership_a = _membership("AAA-ID", record_id="member-a")
    membership_old = _membership("OLD-ID", record_id="member-old", valid_to_ns=850)
    store.extend((membership_a, membership_old))
    record_ids = (membership_a.record_id, membership_old.record_id)
    manifest = DatasetManifest(
        dataset_id="clean-point-in-time-dataset",
        created_at_ns=BASE,
        split_method=SplitMethod.WALK_FORWARD,
        samples=(
            _sample("train", DatasetSplit.TRAIN, 500, record_ids),
            _sample("validation", DatasetSplit.VALIDATION, 650, record_ids),
            _sample(
                "test",
                DatasetSplit.TEST,
                800,
                record_ids,
            ),
        ),
        universe_index_id="SYNTH-100",
        embargo_ns=10,
    )
    return store, manifest


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("source_id", ""),
        ("provider", "bad\x00provider"),
        ("document_id", "x" * 161),
        ("content_sha256", b"\x00" * 32),
        ("content_sha256", b"short"),
    ],
)
def test_source_rejects_invalid_provenance(field: str, value: object) -> None:
    values: dict[str, object] = {
        "source_id": "source",
        "kind": SourceKind.OFFICIAL_PRIMARY,
        "provider": "provider",
        "document_id": "document",
        "content_sha256": hashlib.sha256(b"source").digest(),
        "authenticated": True,
    }
    values[field] = value
    with pytest.raises(ValueError):
        DataSource(**values)  # type: ignore[arg-type]


def test_validity_interval_is_half_open_and_validated() -> None:
    interval = ValidityInterval(10, 20)
    assert interval.contains(10)
    assert interval.contains(19)
    assert not interval.contains(9)
    assert not interval.contains(20)
    assert ValidityInterval(10, None).contains(10_000)
    with pytest.raises(ValueError, match="positive"):
        ValidityInterval(0, None)
    with pytest.raises(ValueError, match="greater"):
        ValidityInterval(10, 10)


@pytest.mark.parametrize(
    "payload",
    [
        CorporateAction("split-1", "AAA-ID", CorporateActionType.SPLIT, 2, 1),
        SymbolMapping("AAA", "AAA-ID", "SYNTH"),
        Delisting("OLD-ID", "synthetic acquisition"),
        IndexMembership("SYNTH-100", "AAA-ID", True),
        AnalystEstimateVintage("AAA-ID", "EPS", "2026Q3", 123, "NANOS"),
        MacroVintage("SYNTH-CPI", "2026-07", 300_000, "PPM"),
        NewsRevision("news-1", None),
        FilingRevision("filing-1", "10-Q", None),
    ],
)
def test_all_supported_record_families_are_immutable_and_queryable(
    payload: RecordPayload,
) -> None:
    record = _record(payload)
    assert record.schema_version == POINT_IN_TIME_SCHEMA_VERSION
    assert record.available_at_ns == 120
    store = PointInTimeStore()
    store.append(record)
    assert store.record_by_id(record.record_id) == record
    assert store.records_for(record.kind, record.logical_key) == (record,)


@pytest.mark.parametrize(
    "factory",
    [
        lambda: CorporateAction("", "AAA", CorporateActionType.SPLIT, 2, 1),
        lambda: CorporateAction("a", "AAA", CorporateActionType.SPLIT, 0, 1),
        lambda: CorporateAction("a", "AAA", CorporateActionType.SPLIT, 1, 0),
        lambda: SymbolMapping("", "AAA", "X"),
        lambda: SymbolMapping("AAA", "", "X"),
        lambda: SymbolMapping("AAA", "AAA", ""),
        lambda: Delisting("", "reason"),
        lambda: Delisting("AAA", ""),
        lambda: IndexMembership("", "AAA", True),
        lambda: IndexMembership("I", "", True),
        lambda: AnalystEstimateVintage("", "EPS", "Q1", 1, "N"),
        lambda: AnalystEstimateVintage("A", "", "Q1", 1, "N"),
        lambda: AnalystEstimateVintage("A", "EPS", "", 1, "N"),
        lambda: AnalystEstimateVintage("A", "EPS", "Q1", 1, ""),
        lambda: MacroVintage("", "2026", 1, "PPM"),
        lambda: MacroVintage("CPI", "", 1, "PPM"),
        lambda: MacroVintage("CPI", "2026", 1, ""),
        lambda: NewsRevision("", None),
        lambda: NewsRevision("n", ""),
        lambda: FilingRevision("", "10-Q", None),
        lambda: FilingRevision("f", "", None),
        lambda: FilingRevision("f", "10-Q", ""),
    ],
)
def test_payloads_reject_invalid_identity_or_units(
    factory: Callable[[], object],
) -> None:
    with pytest.raises(ValueError, match=r".+"):
        factory()


@pytest.mark.parametrize(
    ("changes", "message"),
    [
        ({"record_id": ""}, "record_id"),
        ({"event_time_ns": 0}, "positive"),
        ({"publication_time_ns": 0}, "positive"),
        ({"receive_time_ns": 90}, "receive"),
        ({"processing_time_ns": 105}, "processing"),
        ({"revision_time_ns": 90}, "revision"),
        ({"event_time_ns": 900}, "validity"),
        ({"version": 0}, "version"),
        ({"schema_version": "2.0.0"}, "schema_version"),
        ({"kind": RecordKind.MACRO_VINTAGE}, "payload"),
        ({"logical_key": "wrong"}, "logical_key"),
    ],
)
def test_record_rejects_invalid_temporal_or_type_state(
    changes: dict[str, object], message: str
) -> None:
    record = _record(SymbolMapping("AAA", "AAA-ID", "SYNTH"), valid_to_ns=800)
    with pytest.raises(ValueError, match=message):
        replace(record, **changes)  # type: ignore[arg-type]


def test_record_availability_uses_later_processing_or_revision() -> None:
    record = _record(
        MacroVintage("CPI", "2026-07", 1, "PPM"),
        processing_time_ns=120,
        revision_time_ns=140,
    )
    assert record.available_at_ns == 140


@pytest.mark.parametrize(
    "changes",
    [
        {"sample_id": ""},
        {"event_time_ns": 0},
        {"feature_start_ns": 451},
        {"label_start_ns": 521},
        {"record_ids": ()},
        {"record_ids": ("a", "a")},
        {"universe_instrument_ids": ("A", "A")},
    ],
)
def test_dataset_sample_rejects_malformed_ranges(changes: dict[str, object]) -> None:
    sample = _sample("sample", DatasetSplit.TRAIN, 500, ("record",))
    with pytest.raises(ValueError):
        replace(sample, **changes)  # type: ignore[arg-type]


@pytest.mark.parametrize(
    "changes",
    [
        {"dataset_id": ""},
        {"created_at_ns": 0},
        {"universe_index_id": ""},
        {"samples": ()},
        {
            "samples": (
                _sample("same", DatasetSplit.TRAIN, 500, ("r",)),
                _sample("same", DatasetSplit.TEST, 800, ("r",)),
            )
        },
        {"embargo_ns": -1},
        {"split_method": SplitMethod.RANDOMIZED, "random_seed": None},
    ],
)
def test_dataset_manifest_rejects_malformed_metadata(
    changes: dict[str, object],
) -> None:
    sample = _sample("sample", DatasetSplit.TRAIN, 500, ("record",))
    manifest = DatasetManifest(
        "dataset",
        BASE,
        SplitMethod.BLOCKED_TIME,
        (sample,),
        "SYNTH-100",
    )
    with pytest.raises(ValueError):
        replace(manifest, **changes)  # type: ignore[arg-type]


def test_store_rejects_capacity_duplicates_and_bad_version_lineage() -> None:
    payload = MacroVintage("CPI", "2026-07", 1, "PPM")
    first = _record(payload, record_id="macro-v1")
    with pytest.raises(ValueError, match="positive"):
        PointInTimeStore(0)
    store = PointInTimeStore(1)
    store.append(first)
    with pytest.raises(OverflowError, match="capacity"):
        store.append(_record(SymbolMapping("A", "A-ID", "X")))

    duplicate_store = PointInTimeStore()
    duplicate_store.append(first)
    with pytest.raises(ValueError, match="duplicate"):
        duplicate_store.append(replace(first))
    with pytest.raises(ValueError, match="first"):
        PointInTimeStore().append(replace(first, version=2))
    with pytest.raises(ValueError, match="contiguous"):
        duplicate_store.append(
            _record(
                payload,
                record_id="macro-v3",
                version=3,
                revision_time_ns=200,
                processing_time_ns=210,
            )
        )
    with pytest.raises(ValueError, match="increase"):
        duplicate_store.append(_record(payload, record_id="macro-v2", version=2))


def test_news_corrections_and_filing_amendments_require_exact_parent() -> None:
    news_store = PointInTimeStore()
    first_news = _record(NewsRevision("news", None), record_id="news-v1")
    news_store.append(first_news)
    with pytest.raises(ValueError, match="prior"):
        news_store.append(
            _record(
                NewsRevision("news", "wrong"),
                record_id="news-v2-bad",
                version=2,
                revision_time_ns=200,
                processing_time_ns=210,
            )
        )
    second_news = _record(
        NewsRevision("news", "news-v1"),
        record_id="news-v2",
        version=2,
        revision_time_ns=200,
        processing_time_ns=210,
    )
    news_store.append(second_news)
    assert news_store.records_for(RecordKind.NEWS_REVISION, "news") == (
        first_news,
        second_news,
    )

    with pytest.raises(ValueError, match="initial news"):
        PointInTimeStore().append(
            _record(NewsRevision("orphan", "missing"), record_id="orphan")
        )
    with pytest.raises(ValueError, match="initial filing"):
        PointInTimeStore().append(
            _record(FilingRevision("filing", "10-Q/A", "missing"))
        )

    filing_store = PointInTimeStore()
    filing = _record(FilingRevision("filing", "10-Q", None), record_id="filing-v1")
    filing_store.append(filing)
    with pytest.raises(ValueError, match="prior"):
        filing_store.append(
            _record(
                FilingRevision("filing", "10-Q/A", "wrong"),
                record_id="filing-v2-bad",
                version=2,
                revision_time_ns=200,
                processing_time_ns=210,
            )
        )
    amendment = _record(
        FilingRevision("filing", "10-Q/A", "filing-v1"),
        record_id="filing-v2",
        version=2,
        revision_time_ns=200,
        processing_time_ns=210,
    )
    filing_store.append(amendment)
    assert filing_store.record_by_id("filing-v2") == amendment


def test_as_known_at_latest_before_and_revisions_preserve_vintages() -> None:
    store = PointInTimeStore()
    first = _record(
        MacroVintage("CPI", "2026-07", 300_000, "PPM"),
        record_id="cpi-v1",
        processing_time_ns=120,
        revision_time_ns=100,
    )
    second = _record(
        MacroVintage("CPI", "2026-07", 310_000, "PPM"),
        record_id="cpi-v2",
        version=2,
        publication_time_ns=190,
        receive_time_ns=195,
        processing_time_ns=210,
        revision_time_ns=200,
    )
    store.extend((first, second))
    assert store.as_known_at(120) == (first,)
    assert store.latest_available_before(120) == ()
    assert store.latest_available_before(121) == (first,)
    assert store.as_known_at(210) == (second,)
    assert store.revisions_after(150) == (second,)
    assert store.records_for(RecordKind.MACRO_VINTAGE, "missing") == ()
    assert store.record_by_id("missing") is None
    with pytest.raises(ValueError, match="positive"):
        store.as_known_at(0)
    with pytest.raises(ValueError, match="positive"):
        store.revisions_after(0)


def test_membership_symbol_changes_and_corporate_action_history_are_temporal() -> None:
    store = PointInTimeStore()
    old_member = _membership("OLD-ID", record_id="old-member", valid_to_ns=600)
    new_member = _membership("NEW-ID", record_id="new-member", valid_from_ns=600)
    exclusion = _membership(
        "REMOVED-ID",
        record_id="removed",
        valid_from_ns=1,
        included=False,
    )
    old_symbol = _record(
        SymbolMapping("OLD", "STABLE-ID", "SYNTH"),
        record_id="old-symbol",
        event_time_ns=1,
        valid_from_ns=1,
        valid_to_ns=600,
    )
    new_symbol = _record(
        SymbolMapping("NEW", "STABLE-ID", "SYNTH"),
        record_id="new-symbol",
        event_time_ns=600,
        valid_from_ns=600,
    )
    action = _record(
        CorporateAction("split", "STABLE-ID", CorporateActionType.SPLIT, 2, 1),
        record_id="split",
        event_time_ns=700,
        valid_from_ns=700,
    )
    unrelated_action = _record(
        CorporateAction("other", "OTHER-ID", CorporateActionType.MERGER, 1, 1),
        record_id="other-action",
        event_time_ns=700,
        valid_from_ns=700,
    )
    store.extend(
        (
            old_member,
            new_member,
            exclusion,
            old_symbol,
            new_symbol,
            action,
            unrelated_action,
        )
    )
    assert store.membership_at(500, index_id="SYNTH-100") == ("OLD-ID",)
    assert store.membership_at(700, index_id="SYNTH-100") == ("NEW-ID",)
    assert store.membership_at(700, index_id="OTHER") == ()
    assert store.symbol_mapping_at(500, symbol="OLD") == old_symbol.payload
    assert store.symbol_mapping_at(700, symbol="NEW") == new_symbol.payload
    assert store.symbol_mapping_at(700, symbol="OLD") is None
    assert store.symbol_mapping_at(700, symbol="MISSING") is None
    assert store.corporate_action_history("STABLE-ID", known_at_ns=800) == (action,)


def test_clean_walk_forward_dataset_is_accepted_deterministically() -> None:
    store, manifest = _clean_store_and_manifest()
    validator = LeakageValidator()
    first = validator.validate(manifest, store)
    second = validator.validate_or_raise(manifest, store)
    assert first == second
    assert first.accepted
    assert first.violations == ()
    first.raise_if_invalid()


@pytest.mark.parametrize(
    ("payload", "expected_code"),
    [
        (MacroVintage("CPI", "2026-07", 1, "PPM"), LeakageCode.FUTURE_MACRO_REVISION),
        (
            IndexMembership("SYNTH-100", "FUTURE-ID", True),
            LeakageCode.FUTURE_CONSTITUENT,
        ),
        (
            AnalystEstimateVintage("AAA-ID", "EPS", "2026Q3", 1, "NANOS"),
            LeakageCode.POST_EVENT_ESTIMATE,
        ),
        (
            CorporateAction("future-split", "AAA-ID", CorporateActionType.SPLIT, 2, 1),
            LeakageCode.FUTURE_CORPORATE_ACTION,
        ),
        (NewsRevision("future-news", None), LeakageCode.FUTURE_RECORD),
    ],
)
def test_future_records_are_rejected_with_specific_reason(
    payload: RecordPayload, expected_code: LeakageCode
) -> None:
    store = PointInTimeStore()
    event_time = 600 if isinstance(payload, (IndexMembership, CorporateAction)) else 500
    future = _record(
        payload,
        record_id="future",
        event_time_ns=event_time,
        publication_time_ns=470,
        receive_time_ns=480,
        processing_time_ns=490,
        revision_time_ns=470,
        valid_from_ns=1 if event_time == 500 else event_time,
    )
    store.append(future)
    sample = _sample(
        "leaking",
        DatasetSplit.TRAIN,
        450,
        (future.record_id,),
        universe=(),
    )
    manifest = DatasetManifest(
        "future-data",
        BASE,
        SplitMethod.BLOCKED_TIME,
        (sample,),
        "SYNTH-100",
    )
    report = LeakageValidator().validate(manifest, store)
    assert expected_code in {item.code for item in report.violations}
    with pytest.raises(LeakageDetectedError) as caught:
        report.raise_if_invalid()
    assert caught.value.report == report
    assert "rejected" in str(caught.value)


def test_future_effective_membership_and_action_are_rejected_even_if_announced() -> (
    None
):
    store = PointInTimeStore()
    future_member = _membership(
        "FUTURE-ID",
        record_id="future-member",
        available_ns=300,
        valid_from_ns=600,
    )
    future_action = _record(
        CorporateAction("split", "AAA-ID", CorporateActionType.SPLIT, 2, 1),
        record_id="future-action",
        event_time_ns=600,
        publication_time_ns=250,
        receive_time_ns=260,
        processing_time_ns=270,
        revision_time_ns=250,
        valid_from_ns=600,
    )
    store.extend((future_member, future_action))
    sample = _sample(
        "before-effective",
        DatasetSplit.TRAIN,
        500,
        (future_member.record_id, future_action.record_id),
        universe=(),
    )
    manifest = DatasetManifest(
        "future-effective",
        BASE,
        SplitMethod.BLOCKED_TIME,
        (sample,),
        "SYNTH-100",
    )
    codes = {
        item.code for item in LeakageValidator().validate(manifest, store).violations
    }
    assert LeakageCode.FUTURE_CONSTITUENT in codes
    assert LeakageCode.FUTURE_CORPORATE_ACTION in codes


def test_missing_provenance_future_features_and_local_label_overlap_reject() -> None:
    store = PointInTimeStore()
    sample = DatasetSample(
        "malformed-window",
        500,
        400,
        510,
        500,
        550,
        DatasetSplit.TRAIN,
        ("missing",),
        (),
    )
    manifest = DatasetManifest(
        "malformed-window-dataset",
        BASE,
        SplitMethod.BLOCKED_TIME,
        (sample,),
        "SYNTH-100",
    )
    codes = {
        item.code for item in LeakageValidator().validate(manifest, store).violations
    }
    assert codes == {
        LeakageCode.FUTURE_FEATURE,
        LeakageCode.LABEL_OVERLAP,
        LeakageCode.MISSING_PROVENANCE,
    }


def test_historical_delisted_constituent_cannot_be_removed_from_old_universe() -> None:
    store = PointInTimeStore()
    member = _membership("DELISTED-ID", record_id="historical-member")
    delisting = _record(
        Delisting("DELISTED-ID", "synthetic delisting"),
        record_id="delisting",
        event_time_ns=900,
        publication_time_ns=850,
        receive_time_ns=860,
        processing_time_ns=870,
        revision_time_ns=850,
        valid_from_ns=900,
    )
    store.extend((member, delisting))
    sample = _sample(
        "historical-sample",
        DatasetSplit.TRAIN,
        500,
        (member.record_id,),
        universe=(),
    )
    manifest = DatasetManifest(
        "survivorship-biased",
        BASE,
        SplitMethod.BLOCKED_TIME,
        (sample,),
        "SYNTH-100",
    )
    report = LeakageValidator().validate(manifest, store)
    assert [item.code for item in report.violations] == [LeakageCode.SURVIVORSHIP_BIAS]


def test_randomized_nonchronological_and_cross_split_label_overlap_reject() -> None:
    store = PointInTimeStore()
    membership = _membership("AAA-ID", record_id="member")
    store.append(membership)
    train = _sample(
        "train",
        DatasetSplit.TRAIN,
        700,
        (membership.record_id,),
        universe=("AAA-ID",),
    )
    test = _sample(
        "test",
        DatasetSplit.TEST,
        650,
        (membership.record_id,),
        universe=("AAA-ID",),
    )
    manifest = DatasetManifest(
        "randomized",
        BASE,
        SplitMethod.RANDOMIZED,
        (train, test),
        "SYNTH-100",
        embargo_ns=100,
        random_seed=20260831,
    )
    report = LeakageValidator().validate(manifest, store)
    codes = {item.code for item in report.violations}
    assert LeakageCode.RANDOMIZED_TIME_SPLIT in codes
    assert LeakageCode.NON_CHRONOLOGICAL_SPLIT in codes
    assert LeakageCode.LABEL_OVERLAP in codes
    with pytest.raises(LeakageDetectedError):
        LeakageValidator().validate_or_raise(manifest, store)
