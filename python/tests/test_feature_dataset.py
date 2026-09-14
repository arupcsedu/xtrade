"""Leakage-safe feature/label dataset construction tests."""

from __future__ import annotations

import hashlib
import json
import os
from dataclasses import replace
from datetime import date, timedelta
from pathlib import Path
from typing import TYPE_CHECKING, cast

import pyarrow as pa  # type: ignore[import-untyped]
import pyarrow.parquet as pq  # type: ignore[import-untyped]
import pytest
from aegis_mx_intelligence import ConfigurationVersion, Identifier128, InstrumentId
from aegis_mx_research import feature_dataset as feature_module
from aegis_mx_research.canonical_minute import (
    CANONICAL_MINUTE_SCHEMA_VERSION,
    CanonicalMinuteRecord,
    MinuteDataQuality,
)
from aegis_mx_research.data_repository import (
    DECIMAL_GB,
    AdmissionLease,
    DataRepository,
    FileSystemState,
    QuotaEvidence,
    StoragePolicy,
)
from aegis_mx_research.feature_dataset import (
    DATASET_SEED,
    FEATURE_NAMES,
    PURGE_SESSIONS,
    AggregatePoint,
    CanonicalPartitionInput,
    DatasetBuildCode,
    DatasetBuildError,
    EventCoverage,
    EventIndex,
    FeatureDatasetBuilder,
    FeatureEventKind,
    FeatureSample,
    InMemoryCanonicalSource,
    LabelValidity,
    NormalizationStat,
    ParquetCanonicalSource,
    SectorIndex,
    SectorRevision,
    SplitPlan,
    TemporalFeatureEvent,
    build_and_publish_feature_dataset,
    build_label_coverage,
    build_split_plan,
    fit_normalization,
    validate_leakage,
)
from aegis_mx_research.forecast_contracts import (
    REQUIRED_HORIZON_LABELS,
    ExchangeCalendar,
    HorizonSpec,
    HorizonTarget,
    InstrumentResolutionCode,
    StaticInstrumentResolver,
    TradingSession,
    UniverseSnapshot,
    parse_ticker_universe,
    resolve_horizon,
)
from jsonschema import (  # type: ignore[import-untyped]
    Draft202012Validator,
    FormatChecker,
)

if TYPE_CHECKING:
    from collections.abc import Callable, Iterator, Mapping, Sequence

MINUTE_NS = 60_000_000_000
DAY_NS = 86_400_000_000_000
BASE_NS = 1_700_000_000_000_000_000
SESSION_COUNT = 90
MINUTES_PER_SESSION = 61
INSTRUMENT = InstrumentId(Identifier128(0xA57, 1))
INSTRUMENT_HEX = cast("Identifier128", INSTRUMENT).hex()
SOURCE_SHA = hashlib.sha256(b"feature-source").hexdigest()


def _digest(value: object) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def _calendar() -> ExchangeCalendar:
    sessions = tuple(
        TradingSession(
            f"S{index:03d}",
            BASE_NS + index * DAY_NS,
            BASE_NS + index * DAY_NS + MINUTES_PER_SESSION * MINUTE_NS,
        )
        for index in range(SESSION_COUNT)
    )
    version = ConfigurationVersion(Identifier128(0xCA1, 1))
    return ExchangeCalendar("XNYS-FEATURE", version, sessions)


def _universe(*, include_unresolved: bool = False) -> UniverseSnapshot:
    symbols = b"AAA\nZZZ\n" if include_unresolved else b"AAA\n"
    unresolved = (
        {"ZZZ": InstrumentResolutionCode.RECENTLY_LISTED}
        if include_unresolved
        else None
    )
    resolver = StaticInstrumentResolver({"AAA": INSTRUMENT}, unresolved)
    return parse_ticker_universe(symbols, resolver)


def _record(
    session: int,
    minute: int,
    *,
    action_ids: tuple[str, ...] = (),
) -> CanonicalMinuteRecord:
    opened = BASE_NS + session * DAY_NS
    start = opened + minute * MINUTE_NS
    price = 10_000 + session * 10 + minute
    provisional = CanonicalMinuteRecord(
        instrument_id=INSTRUMENT_HEX,
        source_ticker="AAA",
        market_id="IEX",
        session_id=f"S{session:03d}",
        trading_date=date(2026, 1, 1) + timedelta(days=session),
        minute_start_exchange_time_ns=start,
        minute_end_exchange_time_ns=start + MINUTE_NS,
        session_open_exchange_time_ns=opened,
        session_close_exchange_time_ns=opened + MINUTES_PER_SESSION * MINUTE_NS,
        currency="USD",
        tick_value_currency_nanos=10_000_000,
        open_ticks=price,
        high_ticks=price + 2,
        low_ticks=price - 2,
        close_ticks=price + 1,
        volume_shares=100 + minute,
        trade_count=10,
        vwap_ticks=price,
        source_publication_time_ns=start + MINUTE_NS + 1,
        source_availability_time_ns=start + MINUTE_NS + 2,
        local_receipt_time_ns=start + MINUTE_NS + 3,
        local_processing_time_ns=start + MINUTE_NS + 4,
        data_quality_state=MinuteDataQuality.VALID,
        data_quality_reasons=(),
        source_manifest_id="source-" + "11" * 32,
        source_object_id="fixture",
        source_record_id=f"AAA:{session}:{minute}",
        mapping_id="mapping-aaa-v1",
        instrument_revision_id="instrument-aaa-v1",
        tick_revision_id="tick-aaa-v1",
        corporate_action_ids=action_ids,
        reference_sha256=SOURCE_SHA,
        record_sha256="",
    )
    return replace(
        provisional,
        record_sha256=_digest(provisional.payload(include_hash=False)),
    )


def _records(
    *, action_from_session: int | None = None
) -> tuple[CanonicalMinuteRecord, ...]:
    return tuple(
        _record(
            session,
            minute,
            action_ids=("split-v1",)
            if action_from_session is not None and session >= action_from_session
            else (),
        )
        for session in range(SESSION_COUNT)
        for minute in range(MINUTES_PER_SESSION)
    )


def _builder_events() -> tuple[FeatureDatasetBuilder, InMemoryCanonicalSource]:
    calendar = _calendar()
    sector = SectorRevision(
        "sector-aaa-v1",
        INSTRUMENT_HEX,
        "TECH",
        BASE_NS - 1,
        None,
        BASE_NS - 1,
        1,
        SOURCE_SHA,
    )
    event = TemporalFeatureEvent(
        "news-1",
        "news-revision-1",
        FeatureEventKind.NEWS,
        INSTRUMENT_HEX,
        BASE_NS,
        BASE_NS + 10 * DAY_NS,
        BASE_NS + SESSION_COUNT * DAY_NS,
        SOURCE_SHA,
    )
    return (
        FeatureDatasetBuilder(
            _universe(),
            calendar,
            sectors=SectorIndex((sector,)),
            events=EventIndex((event,)),
        ),
        InMemoryCanonicalSource(_records(), maximum_records=10_000),
    )


def _pipeline() -> tuple[
    FeatureDatasetBuilder,
    InMemoryCanonicalSource,
    SplitPlan,
    Mapping[int, AggregatePoint],
    tuple[NormalizationStat, ...],
    tuple[FeatureSample, ...],
]:
    builder, source = _builder_events()
    split = build_split_plan(builder.observed_sessions(source))
    aggregates = builder.cross_sectional_aggregates(source)
    normalization = builder.fit(source, split, aggregates)
    samples = tuple(
        builder.iter_samples(source, split, aggregates, normalization=normalization)
    )
    return builder, source, split, aggregates, normalization, samples


def test_builder_derives_features_labels_summaries_and_deterministic_ids() -> None:
    builder, source, split, aggregates, normalization, samples = _pipeline()
    assert DATASET_SEED == 20_260_831
    assert len(split.first_purge_sessions) == PURGE_SESSIONS
    assert len(split.second_purge_sessions) == PURGE_SESSIONS
    assert split.train_sessions
    assert split.validation_sessions
    assert split.test_sessions
    assert len(samples) == 6 * MINUTES_PER_SESSION
    assert tuple(stat.feature_name for stat in normalization) == FEATURE_NAMES
    assert all(
        stat.maximum_fit_exchange_time_ns > 0
        for stat in normalization
        if stat.count > 0
    )
    assert any(
        label.validity is LabelValidity.VALID
        for sample in samples
        for label in sample.labels
    )
    assert any(sample.raw_features[18] == 0 for sample in samples)
    assert any(sample.raw_features[18] == 1 for sample in samples)
    assert len(tuple(builder.session_summaries(source))) == SESSION_COUNT
    assert samples == tuple(
        builder.iter_samples(source, split, aggregates, normalization=normalization)
    )
    assert validate_leakage(samples, split, normalization).status == "PASS"


def test_builder_fit_skips_future_labels_and_matches_reference(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    builder, source = _builder_events()
    split = build_split_plan(builder.observed_sessions(source))
    aggregates = builder.cross_sectional_aggregates(source)
    reference = fit_normalization(
        sample
        for sample in builder.iter_samples(source, split, aggregates)
        if sample.split == "TRAIN"
    )

    def unexpected_labels(*_args: object, **_kwargs: object) -> object:
        raise AssertionError

    monkeypatch.setattr(FeatureDatasetBuilder, "_labels", unexpected_labels)
    assert builder.fit(source, split, aggregates) == reference


def test_cross_sectional_features_never_use_later_processed_bars() -> None:
    second_instrument = InstrumentId(Identifier128(0xA57, 2))
    second_hex = cast("Identifier128", second_instrument).hex()
    universe = parse_ticker_universe(
        b"AAA\nBBB\n",
        StaticInstrumentResolver({"AAA": INSTRUMENT, "BBB": second_instrument}),
    )

    def second_record(record: CanonicalMinuteRecord) -> CanonicalMinuteRecord:
        provisional = replace(
            record,
            instrument_id=second_hex,
            source_ticker="BBB",
            source_record_id=record.source_record_id.replace("AAA", "BBB"),
            mapping_id="mapping-bbb-v1",
            instrument_revision_id="instrument-bbb-v1",
            local_processing_time_ns=record.local_processing_time_ns + 100,
            record_sha256="",
        )
        return replace(
            provisional,
            record_sha256=_digest(provisional.payload(include_hash=False)),
        )

    first = tuple(
        _record(session, minute) for session in range(87) for minute in range(2)
    )
    source = InMemoryCanonicalSource(
        (*first, *(second_record(record) for record in first)),
        maximum_records=1_000,
    )
    builder = FeatureDatasetBuilder(universe, _calendar())
    split = build_split_plan(builder.observed_sessions(source))
    aggregates = builder.cross_sectional_aggregates(source)
    samples = tuple(builder.iter_samples(source, split, aggregates))
    target_time = BASE_NS + 2 * MINUTE_NS
    first_sample = next(
        sample
        for sample in samples
        if sample.instrument_id == INSTRUMENT_HEX
        and sample.as_of_exchange_time_ns == target_time
    )
    second_sample = next(
        sample
        for sample in samples
        if sample.instrument_id == second_hex
        and sample.as_of_exchange_time_ns == target_time
    )
    assert first_sample.raw_features[12] is None
    assert "MARKET_AGGREGATE_UNAVAILABLE" in first_sample.feature_reason_codes
    assert second_sample.raw_features[12] is not None


def test_train_only_normalization_and_overlap_are_rejected() -> None:
    _, _, split, _, normalization, samples = _pipeline()
    validation = next(sample for sample in samples if sample.split == "VALIDATION")
    with pytest.raises(DatasetBuildError) as error:
        fit_normalization((validation,))
    assert error.value.code is DatasetBuildCode.NORMALIZATION_LEAKAGE
    valid_label = next(
        label for label in validation.labels if label.validity is LabelValidity.VALID
    )
    leaking_label = replace(
        valid_label, target_exchange_time_ns=validation.feature_end_exchange_time_ns
    )
    with pytest.raises(DatasetBuildError) as error:
        replace(
            validation,
            labels=tuple(
                leaking_label if label is valid_label else label
                for label in validation.labels
            ),
        )
    assert error.value.code is DatasetBuildCode.LABEL_FEATURE_OVERLAP
    bad_normalization = tuple(
        replace(
            stat,
            maximum_fit_exchange_time_ns=validation.as_of_exchange_time_ns,
        )
        for stat in normalization
    )
    assert (
        DatasetBuildCode.NORMALIZATION_LEAKAGE.value
        in validate_leakage(samples, split, bad_normalization).violations
    )


def test_future_event_revision_is_not_exposed_and_direct_poison_is_rejected() -> None:
    _, _, _, _, _, samples = _pipeline()
    before = next(
        sample
        for sample in samples
        if sample.as_of_exchange_time_ns < BASE_NS + 10 * DAY_NS
    )
    after = next(
        sample
        for sample in samples
        if sample.as_of_exchange_time_ns > BASE_NS + 10 * DAY_NS
    )
    assert before.raw_features[18] == 0
    assert after.raw_features[18] == 1
    with pytest.raises(DatasetBuildError) as error:
        replace(
            after,
            max_event_availability_time_ns=after.knowledge_cutoff_time_ns + 1,
        )
    assert error.value.code is DatasetBuildCode.FUTURE_EVENT_REVISION


def test_event_flags_distinguish_unobserved_source_from_observed_no_event(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    uncovered = EventIndex(())
    assert uncovered.active(INSTRUMENT_HEX, BASE_NS)[:2] == (None, None)
    coverage = EventCoverage(
        FeatureEventKind.NEWS,
        "TEST_NEWS",
        BASE_NS,
        BASE_NS + DAY_NS,
        SOURCE_SHA,
    )
    covered = EventIndex((), (coverage,))
    assert covered.active(INSTRUMENT_HEX, BASE_NS)[:2] == (False, None)
    assert covered.active(INSTRUMENT_HEX, BASE_NS - 1)[:2] == (None, None)
    with pytest.raises(DatasetBuildError):
        replace(coverage, kind=cast("FeatureEventKind", "bad"))
    with pytest.raises(DatasetBuildError):
        replace(coverage, end_time_ns=BASE_NS)
    with pytest.raises(DatasetBuildError):
        EventIndex((), authenticated_snapshot_sha256="bad")
    monkeypatch.setattr(feature_module, "MAX_EVENT_COVERAGE", 0)
    with pytest.raises(DatasetBuildError):
        EventIndex((), (coverage,))


def test_future_corporate_action_invalidates_cross_action_label() -> None:
    builder = FeatureDatasetBuilder(_universe(), _calendar())
    source = InMemoryCanonicalSource(
        _records(action_from_session=50), maximum_records=10_000
    )
    split = build_split_plan(builder.observed_sessions(source))
    aggregates = builder.cross_sectional_aggregates(source)
    samples = tuple(builder.iter_samples(source, split, aggregates))
    assert any(
        label.missing_reason == DatasetBuildCode.CORPORATE_ACTION_CHANGED.value
        for sample in samples
        for label in sample.labels
    )
    coverage = build_label_coverage(_universe(), samples)
    assert any(item.invalid > 0 for item in coverage)


def test_coverage_retains_recent_listing_and_all_horizons() -> None:
    _, _, _, _, _, samples = _pipeline()
    coverage = build_label_coverage(_universe(include_unresolved=True), samples)
    assert len(coverage) == 24
    recent = [item for item in coverage if item.symbol == "ZZZ"]
    assert len(recent) == 12
    assert all(item.samples == 0 for item in recent)
    assert all(item.reason_counts == {"RECENTLY_LISTED": 1} for item in recent)


def _parquet_rows(records: Sequence[CanonicalMinuteRecord]) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for record in records:
        row = record.payload()
        row["trading_date"] = record.trading_date
        rows.append(row)
    return rows


def _write_canonical_parquet(
    path: Path,
    records: Sequence[CanonicalMinuteRecord],
    *,
    metadata: bool = True,
    compression: str = "zstd",
) -> str:
    schema = pa.Table.from_pylist(_parquet_rows(records)).schema
    if metadata:
        schema = schema.with_metadata(
            {
                b"aegis.schema": b"canonical-minute-record",
                b"aegis.schema_version": CANONICAL_MINUTE_SCHEMA_VERSION.encode(),
            }
        )
    pq.write_table(
        pa.Table.from_pylist(_parquet_rows(records), schema=schema),
        path,
        compression=compression,
    )
    return hashlib.sha256(path.read_bytes()).hexdigest()


def test_parquet_source_hash_schema_and_streaming_round_trip(tmp_path: Path) -> None:
    records = (_record(0, 0), _record(0, 1))
    path = tmp_path / "canonical.parquet"
    digest = _write_canonical_parquet(path, records)
    source = ParquetCanonicalSource(
        (CanonicalPartitionInput(path, digest, "partition-fixture"),)
    )
    assert source.instrument_ids == (INSTRUMENT_HEX,)
    assert source.record_count == 2
    assert source.source_partition_ids == ("partition-fixture",)
    assert tuple(source.iter_instrument(INSTRUMENT_HEX)) == records
    assert tuple(source.iter_instrument("missing")) == ()
    second_id = cast("Identifier128", InstrumentId(Identifier128(0xA57, 2))).hex()
    second_provisional = replace(
        records[0],
        instrument_id=second_id,
        source_ticker="BBB",
        source_record_id="BBB:0:0",
        record_sha256="",
    )
    second = replace(
        second_provisional,
        record_sha256=_digest(second_provisional.payload(include_hash=False)),
    )
    mixed_path = tmp_path / "mixed.parquet"
    mixed_digest = _write_canonical_parquet(mixed_path, (second, records[0]))
    mixed = ParquetCanonicalSource(
        (CanonicalPartitionInput(mixed_path, mixed_digest, "partition-mixed"),)
    )
    assert tuple(mixed.iter_instrument(INSTRUMENT_HEX)) == (records[0],)
    with pytest.raises(DatasetBuildError) as error:
        ParquetCanonicalSource(
            (CanonicalPartitionInput(path, "11" * 32, "partition-bad"),)
        )
    assert error.value.code is DatasetBuildCode.CORRUPT_CANONICAL_INPUT


def test_parquet_source_rejects_path_metadata_codec_size_and_order(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    with pytest.raises(DatasetBuildError):
        CanonicalPartitionInput(Path("relative"), SOURCE_SHA, "partition-relative")
    with pytest.raises(DatasetBuildError):
        ParquetCanonicalSource(())
    valid_path = tmp_path / "valid.parquet"
    digest = _write_canonical_parquet(valid_path, (_record(0, 0),))
    valid_input = CanonicalPartitionInput(valid_path, digest, "partition-valid")
    monkeypatch.setattr(feature_module, "MAX_DATASET_INSTRUMENTS", 0)
    with pytest.raises(DatasetBuildError):
        ParquetCanonicalSource((valid_input,))
    monkeypatch.setattr(feature_module, "MAX_DATASET_INSTRUMENTS", 10_000)
    with pytest.raises(DatasetBuildError):
        ParquetCanonicalSource((valid_input,), batch_rows=0)
    with pytest.raises(DatasetBuildError):
        ParquetCanonicalSource((valid_input, valid_input))
    with pytest.raises(DatasetBuildError) as error:
        ParquetCanonicalSource((valid_input,), maximum_records=0)
    assert error.value.code is DatasetBuildCode.STORAGE_LIMIT

    no_metadata = tmp_path / "no-metadata.parquet"
    no_metadata_digest = _write_canonical_parquet(
        no_metadata, (_record(0, 0),), metadata=False
    )
    with pytest.raises(DatasetBuildError):
        ParquetCanonicalSource(
            (
                CanonicalPartitionInput(
                    no_metadata, no_metadata_digest, "partition-no-metadata"
                ),
            )
        )
    wrong_codec = tmp_path / "wrong-codec.parquet"
    wrong_codec_digest = _write_canonical_parquet(
        wrong_codec, (_record(0, 0),), compression="snappy"
    )
    with pytest.raises(DatasetBuildError):
        ParquetCanonicalSource(
            (
                CanonicalPartitionInput(
                    wrong_codec, wrong_codec_digest, "partition-wrong-codec"
                ),
            )
        )
    bad_file = tmp_path / "bad.parquet"
    bad_file.write_bytes(b"not parquet")
    with pytest.raises(DatasetBuildError):
        ParquetCanonicalSource(
            (
                CanonicalPartitionInput(
                    bad_file,
                    hashlib.sha256(bad_file.read_bytes()).hexdigest(),
                    "partition-bad-file",
                ),
            )
        )
    alias = tmp_path / "alias.parquet"
    alias.symlink_to(valid_path)
    with pytest.raises(DatasetBuildError):
        ParquetCanonicalSource(
            (CanonicalPartitionInput(alias, digest, "partition-alias"),)
        )

    reversed_path = tmp_path / "reversed.parquet"
    reversed_digest = _write_canonical_parquet(
        reversed_path, (_record(0, 1), _record(0, 0))
    )
    reversed_source = ParquetCanonicalSource(
        (CanonicalPartitionInput(reversed_path, reversed_digest, "partition-reversed"),)
    )
    with pytest.raises(DatasetBuildError) as error:
        tuple(reversed_source.iter_instrument(INSTRUMENT_HEX))
    assert error.value.code is DatasetBuildCode.NON_CHRONOLOGICAL_INPUT

    duplicate_path = tmp_path / "duplicate.parquet"
    duplicate_digest = _write_canonical_parquet(duplicate_path, (_record(0, 0),))
    duplicate_source = ParquetCanonicalSource(
        (
            valid_input,
            CanonicalPartitionInput(
                duplicate_path, duplicate_digest, "partition-duplicate"
            ),
        )
    )
    with pytest.raises(DatasetBuildError) as error:
        tuple(duplicate_source.iter_instrument(INSTRUMENT_HEX))
    assert error.value.code is DatasetBuildCode.DUPLICATE_MINUTE
    payload = _record(0, 0).payload()
    assert feature_module._record_from_arrow(payload) == _record(0, 0)


def test_split_and_input_faults_fail_closed() -> None:
    with pytest.raises(DatasetBuildError) as error:
        build_split_plan(tuple(f"S{index}" for index in range(86)))
    assert error.value.code is DatasetBuildCode.INSUFFICIENT_SPLIT_SESSIONS
    minimum = build_split_plan(tuple(f"S{index}" for index in range(87)))
    assert tuple(
        len(values)
        for values in (
            minimum.train_sessions,
            minimum.validation_sessions,
            minimum.test_sessions,
        )
    ) == (1, 1, 1)
    duplicate = (_record(0, 0), replace(_record(0, 0), close_ticks=99))
    with pytest.raises(DatasetBuildError) as error:
        InMemoryCanonicalSource(duplicate)
    assert error.value.code is DatasetBuildCode.DUPLICATE_MINUTE


def _repository(tmp_path: Path) -> DataRepository:
    repository = DataRepository(
        tmp_path / "poc-data",
        policy=StoragePolicy(),
        filesystem_probe=lambda _path: FileSystemState(
            total_bytes=500 * DECIMAL_GB, free_bytes=400 * DECIMAL_GB
        ),
        git_worktree=Path.cwd(),
    )
    repository.initialize()
    return repository


def _quota() -> QuotaEvidence:
    return QuotaEvidence(
        limit_bytes=10 * (1 << 40),
        used_bytes=1_000_000,
        source="authoritative-test-quota",
        observed_at_utc="2026-09-12T12:00:00Z",
        authoritative=True,
    )


def test_manifest_last_publication_recovers_and_is_idempotent(tmp_path: Path) -> None:
    builder = FeatureDatasetBuilder(_universe(), _calendar())
    source = InMemoryCanonicalSource(
        tuple(_record(session, 0) for session in range(87))
    )
    repository = _repository(tmp_path)

    def interrupt(stage: str) -> None:
        if stage == "FEATURE_PARTITION_PUBLISHED":
            message = "injected interruption"
            raise RuntimeError(message)

    with pytest.raises(RuntimeError, match="injected interruption"):
        build_and_publish_feature_dataset(
            repository, _quota(), builder, source, fault_injector=interrupt
        )
    assert not tuple(
        (repository.root / "datasets/feature-poc/manifests").glob("*.json")
    )
    published = build_and_publish_feature_dataset(repository, _quota(), builder, source)
    repeated = build_and_publish_feature_dataset(repository, _quota(), builder, source)
    assert published == repeated
    assert published.leakage.status == "PASS"
    assert published.sample_count == 3
    assert published.session_summary_count == 87
    document = json.loads(published.manifest_path.read_bytes())
    assert document["dataset_id"] == published.dataset_id
    assert document["derived_daily_bars_downloaded"] is False
    assert document["live_trading_capable"] is False
    schema_root = Path("schemas")
    coverage_schema = json.loads(
        (schema_root / "feature-label-coverage-v1.schema.json").read_bytes()
    )
    manifest_schema = json.loads(
        (schema_root / "feature-dataset-manifest-v1.schema.json").read_bytes()
    )
    manifest_schema["properties"]["coverage"] = coverage_schema
    Draft202012Validator.check_schema(manifest_schema)
    Draft202012Validator(manifest_schema, format_checker=FormatChecker()).validate(
        document
    )
    feature_object = next(
        item for item in document["objects"] if item["kind"] == "FEATURE_LABEL"
    )
    summary_object = next(
        item for item in document["objects"] if item["kind"] == "SESSION_SUMMARY"
    )
    feature_row = pq.read_table(
        repository.root / feature_object["storage_path"]
    ).to_pylist()[0]
    summary_row = pq.read_table(
        repository.root / summary_object["storage_path"]
    ).to_pylist()[0]
    feature_schema = json.loads(
        (schema_root / "feature-label-row-v1.schema.json").read_bytes()
    )
    summary_schema = json.loads(
        (schema_root / "derived-session-summary-v1.schema.json").read_bytes()
    )
    Draft202012Validator(feature_schema, format_checker=FormatChecker()).validate(
        feature_row
    )
    Draft202012Validator(summary_schema, format_checker=FormatChecker()).validate(
        summary_row
    )


def test_committed_real_coverage_report_retains_complete_admitted_matrix() -> None:
    report = json.loads(
        Path("docs/testing/forecasting-poc-label-coverage.json").read_bytes()
    )
    schema = json.loads(
        Path("schemas/feature-label-coverage-v1.schema.json").read_bytes()
    )
    Draft202012Validator(schema, format_checker=FormatChecker()).validate(report)
    symbols = tuple(
        line.strip()
        for line in Path("ticker.txt").read_text(encoding="ascii").splitlines()
        if line.strip()
    )
    horizons = REQUIRED_HORIZON_LABELS
    assert len(report) == len(symbols) * len(horizons)
    for index, symbol in enumerate(symbols):
        rows = report[index * len(horizons) : (index + 1) * len(horizons)]
        assert tuple(row["horizon"] for row in rows) == horizons
        assert {row["symbol"] for row in rows} == {symbol}
        if rows[0]["resolution_code"] == "RESOLVED":
            assert all(row["samples"] > 0 for row in rows)
            assert all(row["valid"] >= 100 for row in rows)
            assert all(
                "MISSING_CANONICAL_DATA" not in row["reason_counts"] for row in rows
            )
        else:
            assert symbol == "KRKNF"
            assert all(row["resolution_code"] == "UNSUPPORTED" for row in rows)
            assert all(row["samples"] == 0 and row["valid"] == 0 for row in rows)
    assert sum(row["samples"] for row in report if row["horizon"] == "5m") == 7_684_385
    assert sum(row["valid"] for row in report) == 79_065_673


@pytest.mark.parametrize(
    ("operation", "code"),
    [
        (
            lambda: feature_module._require_text("é", "field"),
            DatasetBuildCode.INVALID_CONFIGURATION,
        ),
        (
            lambda: feature_module._require_text("", "field"),
            DatasetBuildCode.INVALID_CONFIGURATION,
        ),
        (
            lambda: feature_module._require_sha256("bad", "field"),
            DatasetBuildCode.INVALID_CONFIGURATION,
        ),
        (
            lambda: feature_module._checked_int64(value=True, field="field"),
            DatasetBuildCode.INTEGER_OVERFLOW,
        ),
        (
            lambda: feature_module._divide_round_half_away(1, 0),
            DatasetBuildCode.CORRUPT_CANONICAL_INPUT,
        ),
        (
            lambda: feature_module._return_ppm(0, 1),
            DatasetBuildCode.CORRUPT_CANONICAL_INPUT,
        ),
        (lambda: feature_module._mean_int(()), DatasetBuildCode.INVALID_CONFIGURATION),
    ],
)
def test_low_level_numeric_and_text_guards_fail_closed(
    operation: object, code: DatasetBuildCode
) -> None:
    with pytest.raises(DatasetBuildError) as error:
        cast("Callable[[], object]", operation)()
    assert error.value.code is code
    assert feature_module._divide_round_half_away(-3, 2) == -2


def test_sector_and_event_contract_guards(monkeypatch: pytest.MonkeyPatch) -> None:
    valid = SectorRevision(
        "sector-v1", INSTRUMENT_HEX, "TECH", BASE_NS, None, BASE_NS, 1, SOURCE_SHA
    )
    assert valid.applies(BASE_NS, BASE_NS)
    assert not valid.applies(BASE_NS - 1, BASE_NS)
    assert (
        SectorIndex((valid,)).at(
            INSTRUMENT_HEX, event_time_ns=BASE_NS, known_at_ns=BASE_NS
        )
        == valid
    )
    assert (
        SectorIndex((valid,)).at("missing", event_time_ns=BASE_NS, known_at_ns=BASE_NS)
        is None
    )
    for sector_changes in (
        {"revision_id": ""},
        {"effective_from_ns": 0},
        {"effective_to_ns": BASE_NS},
        {"version": 0},
        {"source_sha256": "bad"},
    ):
        with pytest.raises(DatasetBuildError):
            replace(valid, **sector_changes)
    with pytest.raises(DatasetBuildError) as error:
        SectorIndex((valid, replace(valid, revision_id="sector-v3", version=3)))
    assert error.value.code is DatasetBuildCode.FUTURE_REFERENCE
    with pytest.raises(DatasetBuildError):
        SectorIndex(
            (
                valid,
                replace(
                    valid,
                    revision_id="sector-v2",
                    version=2,
                    available_at_ns=BASE_NS,
                ),
            )
        )
    monkeypatch.setattr(feature_module, "MAX_SECTOR_REVISIONS", 0)
    with pytest.raises(DatasetBuildError):
        SectorIndex((valid,))

    event = TemporalFeatureEvent(
        "event-1",
        "revision-1",
        FeatureEventKind.MACRO,
        None,
        BASE_NS,
        BASE_NS,
        BASE_NS + DAY_NS,
        SOURCE_SHA,
    )
    assert EventIndex((event,)).active(INSTRUMENT_HEX, BASE_NS)[:2] == (None, True)
    for event_changes in (
        {"kind": cast("FeatureEventKind", "bad")},
        {"event_time_ns": 0},
        {"available_at_ns": BASE_NS - 1},
        {"available_at_ns": BASE_NS + DAY_NS},
        {"source_sha256": "bad"},
    ):
        with pytest.raises(DatasetBuildError):
            replace(event, **event_changes)
    with pytest.raises(DatasetBuildError):
        EventIndex((event, event))
    ignored_news = replace(
        event,
        record_id="event-ignored",
        kind=FeatureEventKind.NEWS,
        instrument_id=None,
    )
    assert EventIndex((ignored_news,)).active(INSTRUMENT_HEX, BASE_NS)[:2] == (
        False,
        None,
    )


def test_event_interval_index_finds_long_lived_older_event() -> None:
    events = (
        TemporalFeatureEvent(
            "long-lived",
            "revision-long",
            FeatureEventKind.MACRO,
            None,
            BASE_NS,
            BASE_NS,
            BASE_NS + 10 * DAY_NS,
            SOURCE_SHA,
        ),
        TemporalFeatureEvent(
            "expired-newer",
            "revision-expired",
            FeatureEventKind.MACRO,
            None,
            BASE_NS + DAY_NS,
            BASE_NS + DAY_NS,
            BASE_NS + 2 * DAY_NS,
            SOURCE_SHA,
        ),
        TemporalFeatureEvent(
            "expired-newest",
            "revision-newest",
            FeatureEventKind.MACRO,
            None,
            BASE_NS + 3 * DAY_NS,
            BASE_NS + 3 * DAY_NS,
            BASE_NS + 4 * DAY_NS,
            SOURCE_SHA,
        ),
    )
    coverage = EventCoverage(
        FeatureEventKind.MACRO,
        "MACRO_FIXTURE",
        BASE_NS,
        BASE_NS + 12 * DAY_NS,
        SOURCE_SHA,
    )
    index = EventIndex(events, (coverage,))
    active = index.active(INSTRUMENT_HEX, BASE_NS + 5 * DAY_NS)
    assert active[:2] == (None, True)
    assert active[2] == ("long-lived",)
    assert index.active(INSTRUMENT_HEX, BASE_NS + 10 * DAY_NS)[:2] == (
        None,
        False,
    )


def test_calendar_horizon_targets_are_bounded_and_cached_across_symbols(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    builder = FeatureDatasetBuilder(_universe(), _calendar())
    source = InMemoryCanonicalSource(_records(), maximum_records=10_000)
    record = next(source.iter_instrument(INSTRUMENT_HEX))
    records_by_endpoint = {record.minute_end_exchange_time_ns: record}
    original = resolve_horizon
    calls = 0

    def counted_resolve(
        spec: HorizonSpec, as_of_ns: int, calendar: ExchangeCalendar
    ) -> HorizonTarget:
        nonlocal calls
        calls += 1
        return original(spec, as_of_ns, calendar)

    monkeypatch.setattr(feature_module, "resolve_horizon", counted_resolve)
    first = builder._labels(record, records_by_endpoint)
    second = builder._labels(record, records_by_endpoint)
    assert first == second
    assert calls == len(builder.horizons)

    builder._horizon_targets_by_endpoint[record.minute_end_exchange_time_ns] = tuple(
        (None, None) for _ in builder.horizons
    )
    with pytest.raises(DatasetBuildError) as error:
        builder._labels(record, records_by_endpoint)
    assert error.value.code is DatasetBuildCode.CORRUPT_CANONICAL_INPUT

    bounded = FeatureDatasetBuilder(_universe(), _calendar())
    monkeypatch.setattr(feature_module, "MAX_AGGREGATE_POINTS", 0)
    with pytest.raises(DatasetBuildError) as error:
        bounded._horizon_targets(record.minute_end_exchange_time_ns)
    assert error.value.code is DatasetBuildCode.STORAGE_LIMIT


def test_fixture_source_and_split_contract_guards() -> None:
    with pytest.raises(DatasetBuildError):
        InMemoryCanonicalSource((), maximum_records=0)
    with pytest.raises(DatasetBuildError) as error:
        InMemoryCanonicalSource((_record(0, 0),), maximum_records=0)
    assert error.value.code is DatasetBuildCode.INVALID_CONFIGURATION
    with pytest.raises(DatasetBuildError) as error:
        InMemoryCanonicalSource((_record(0, 0), _record(0, 1)), maximum_records=1)
    assert error.value.code is DatasetBuildCode.STORAGE_LIMIT
    with pytest.raises(DatasetBuildError):
        InMemoryCanonicalSource((), source_partition_ids=())
    with pytest.raises(DatasetBuildError):
        InMemoryCanonicalSource((), source_partition_ids=("same", "same"))
    with pytest.raises(DatasetBuildError):
        InMemoryCanonicalSource((), source_partition_ids=("",))
    empty = InMemoryCanonicalSource(())
    assert empty.instrument_ids == ()
    assert empty.record_count == 0
    assert empty.source_partition_ids == ("partition-synthetic",)
    assert tuple(empty.iter_instrument("missing")) == ()
    with pytest.raises(DatasetBuildError) as error:
        build_split_plan(("same", "same"))
    assert error.value.code is DatasetBuildCode.NON_CHRONOLOGICAL_SPLIT


def test_feature_label_and_sample_contract_guards() -> None:
    _, _, _, _, _, samples = _pipeline()
    sample = samples[0]
    valid = next(
        label for label in sample.labels if label.validity is LabelValidity.VALID
    )
    missing = next(
        label
        for candidate in samples
        for label in candidate.labels
        if label.validity is LabelValidity.MISSING
    )
    for label, changes in (
        (valid, {"horizon": "bad"}),
        (valid, {"direction": None}),
        (missing, {"return_ppm": 1}),
        (missing, {"missing_reason": None}),
        (valid, {"corporate_action_version": "bad"}),
    ):
        with pytest.raises(DatasetBuildError):
            replace(label, **changes)
    with pytest.raises(DatasetBuildError) as error:
        replace(sample, split="RANDOM")
    assert error.value.code is DatasetBuildCode.NON_CHRONOLOGICAL_SPLIT
    with pytest.raises(DatasetBuildError):
        replace(sample, raw_features=())
    with pytest.raises(DatasetBuildError):
        replace(sample, labels=tuple(reversed(sample.labels)))
    with pytest.raises(DatasetBuildError):
        replace(sample, feature_start_exchange_time_ns=0)
    with pytest.raises(DatasetBuildError):
        replace(sample, sample_id="bad")
    zero_stat = NormalizationStat("zero", 0, 0, 0, 0)
    constant_stat = NormalizationStat("constant", 2, 4, 8, BASE_NS)
    varying_stat = NormalizationStat("varying", 2, 3, 5, BASE_NS)
    assert zero_stat.normalize(1) is None
    assert zero_stat.normalize(None) is None
    assert constant_stat.normalize(2) == 0
    assert varying_stat.normalize(2) == 1_000_000


def test_leakage_validator_detects_all_mutated_boundaries() -> None:
    _, _, split, _, normalization, samples = _pipeline()
    sample = samples[0]
    outside = replace(sample)
    object.__setattr__(outside, "instrument_id", "ff" * 16)
    mutated = replace(sample)
    object.__setattr__(mutated, "split", "TEST")
    object.__setattr__(
        mutated, "feature_end_exchange_time_ns", sample.as_of_exchange_time_ns - 1
    )
    object.__setattr__(
        mutated,
        "max_event_availability_time_ns",
        sample.knowledge_cutoff_time_ns + 1,
    )
    labels = list(mutated.labels)
    valid_index = next(
        index
        for index, label in enumerate(labels)
        if label.validity is LabelValidity.VALID
    )
    object.__setattr__(
        labels[valid_index],
        "target_exchange_time_ns",
        mutated.feature_end_exchange_time_ns,
    )
    object.__setattr__(mutated, "labels", tuple(labels))
    no_train = replace(split, train_sessions=())
    bad_purge = replace(no_train, first_purge_sessions=())
    report = validate_leakage((mutated,), bad_purge, normalization)
    assert report.status == "REJECTED"
    assert set(report.violations) == {
        DatasetBuildCode.FUTURE_EVENT_REVISION.value,
        DatasetBuildCode.LABEL_FEATURE_OVERLAP.value,
        DatasetBuildCode.NON_CHRONOLOGICAL_SPLIT.value,
        DatasetBuildCode.NORMALIZATION_LEAKAGE.value,
        DatasetBuildCode.PURGE_EMBARGO_VIOLATION.value,
    }
    with pytest.raises(DatasetBuildError):
        build_label_coverage(_universe(), (outside,))


class _FaultSource:
    def __init__(
        self,
        instrument_ids: tuple[str, ...],
        records: tuple[CanonicalMinuteRecord, ...],
        *,
        reported_count: int | None = None,
    ) -> None:
        self._instrument_ids = instrument_ids
        self._records = records
        self._reported_count = (
            len(records) if reported_count is None else reported_count
        )

    @property
    def instrument_ids(self) -> tuple[str, ...]:
        return self._instrument_ids

    @property
    def record_count(self) -> int:
        return self._reported_count

    @property
    def source_partition_ids(self) -> tuple[str, ...]:
        return ("partition-fault",)

    def iter_instrument(self, instrument_id: str) -> Iterator[CanonicalMinuteRecord]:
        del instrument_id
        yield from self._records


def test_builder_rejects_source_identity_order_bounds_and_corruption() -> None:
    calendar = _calendar()
    with pytest.raises(DatasetBuildError):
        FeatureDatasetBuilder(_universe(), calendar, maximum_records_per_instrument=0)
    builder = FeatureDatasetBuilder(
        _universe(), calendar, maximum_records_per_instrument=1
    )
    with pytest.raises(DatasetBuildError) as error:
        builder.observed_sessions(
            _FaultSource((INSTRUMENT_HEX,), (_record(0, 0), _record(0, 1)))
        )
    assert error.value.code is DatasetBuildCode.STORAGE_LIMIT
    with pytest.raises(DatasetBuildError) as error:
        FeatureDatasetBuilder(_universe(), calendar).observed_sessions(
            _FaultSource(("ff" * 16,), ())
        )
    assert error.value.code is DatasetBuildCode.UNRESOLVED_INSTRUMENT
    wrong_instrument = replace(_record(0, 0), instrument_id="ff" * 16)
    with pytest.raises(DatasetBuildError):
        FeatureDatasetBuilder(_universe(), calendar).observed_sessions(
            _FaultSource((INSTRUMENT_HEX,), (wrong_instrument,))
        )
    duplicate = (_record(0, 0), _record(0, 0))
    with pytest.raises(DatasetBuildError) as error:
        FeatureDatasetBuilder(_universe(), calendar).observed_sessions(
            _FaultSource((INSTRUMENT_HEX,), duplicate)
        )
    assert error.value.code is DatasetBuildCode.DUPLICATE_MINUTE
    reverse = (_record(0, 1), _record(0, 0))
    with pytest.raises(DatasetBuildError) as error:
        FeatureDatasetBuilder(_universe(), calendar).observed_sessions(
            _FaultSource((INSTRUMENT_HEX,), reverse)
        )
    assert error.value.code is DatasetBuildCode.NON_CHRONOLOGICAL_INPUT
    corrupt = replace(_record(0, 0), record_sha256="11" * 32)
    with pytest.raises(DatasetBuildError) as error:
        FeatureDatasetBuilder(_universe(), calendar).observed_sessions(
            _FaultSource((INSTRUMENT_HEX,), (corrupt,))
        )
    assert error.value.code is DatasetBuildCode.CORRUPT_CANONICAL_INPUT
    empty_source = _FaultSource((INSTRUMENT_HEX,), ())
    empty_builder = FeatureDatasetBuilder(_universe(), calendar)
    assert tuple(empty_builder.session_summaries(empty_source)) == ()
    assert empty_builder._summaries_for_records(()) == ()


def test_aggregate_point_bound_fails_closed(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    builder = FeatureDatasetBuilder(_universe(), _calendar())
    source = InMemoryCanonicalSource((_record(0, 0), _record(0, 1)))
    monkeypatch.setattr(feature_module, "MAX_AGGREGATE_POINTS", 0)
    with pytest.raises(DatasetBuildError) as error:
        builder.cross_sectional_aggregates(source)
    assert error.value.code is DatasetBuildCode.STORAGE_LIMIT


def test_tick_grid_change_and_sample_input_contracts() -> None:
    builder, source = _builder_events()
    split = build_split_plan(builder.observed_sessions(source))
    aggregates = builder.cross_sectional_aggregates(source)
    normalization = builder.fit(source, split, aggregates)
    with pytest.raises(DatasetBuildError):
        tuple(
            builder.iter_samples(
                source,
                split,
                aggregates,
                normalization=tuple(reversed(normalization)),
            )
        )
    source_outside = _FaultSource(("ff" * 16,), ())
    with pytest.raises(DatasetBuildError):
        tuple(builder.iter_samples(source_outside, split, aggregates))
    with pytest.raises(DatasetBuildError) as fit_error:
        builder.fit(source_outside, split, aggregates)
    assert fit_error.value.code is DatasetBuildCode.UNRESOLVED_INSTRUMENT

    records = list(_records())
    target_index = 5
    target = records[target_index]
    changed = replace(
        target,
        tick_revision_id="tick-aaa-v2",
        tick_value_currency_nanos=20_000_000,
        record_sha256="",
    )
    changed = replace(
        changed, record_sha256=_digest(changed.payload(include_hash=False))
    )
    records[target_index] = changed
    changed_source = InMemoryCanonicalSource(records, maximum_records=10_000)
    changed_split = build_split_plan(builder.observed_sessions(changed_source))
    changed_aggregates = builder.cross_sectional_aggregates(changed_source)
    samples = tuple(
        builder.iter_samples(changed_source, changed_split, changed_aggregates)
    )
    assert any(
        label.missing_reason == DatasetBuildCode.TICK_GRID_CHANGED.value
        for sample in samples
        for label in sample.labels
    )


def test_storage_admission_rejects_projected_dataset(tmp_path: Path) -> None:
    builder = FeatureDatasetBuilder(_universe(), _calendar())
    records = tuple(_record(session, 0) for session in range(87))
    oversized = _FaultSource((INSTRUMENT_HEX,), records, reported_count=800_000_000)
    with pytest.raises(RuntimeError, match="admission denied"):
        build_and_publish_feature_dataset(
            _repository(tmp_path), _quota(), builder, oversized
        )


def test_private_writers_reject_empty_invalid_and_interrupted_output(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    row = {"value": 1}
    schema = pa.schema((pa.field("value", pa.int64(), nullable=False),))
    written = feature_module._write_parquet(
        tmp_path / "one.parquet", schema, (row, row), batch_rows=1
    )
    assert written.record_count == 2
    with pytest.raises(DatasetBuildError):
        feature_module._write_parquet(
            tmp_path / "one.parquet", schema, (row,), batch_rows=1
        )
    with pytest.raises(DatasetBuildError):
        feature_module._write_parquet(
            tmp_path / "empty.parquet", schema, (), batch_rows=1
        )

    def broken_rows() -> Iterator[dict[str, int]]:
        yield row
        message = "injected row failure"
        raise RuntimeError(message)

    broken_path = tmp_path / "broken.parquet"
    with pytest.raises(RuntimeError, match="injected row failure"):
        feature_module._write_parquet(broken_path, schema, broken_rows(), batch_rows=2)
    assert not broken_path.exists()

    byte_path = tmp_path / "bytes.part"
    original_fsync = os.fsync

    def fail_fsync(_descriptor: int) -> None:
        message = "injected fsync failure"
        raise OSError(message)

    monkeypatch.setattr(os, "fsync", fail_fsync)
    with pytest.raises(OSError, match="injected fsync failure"):
        feature_module._write_bytes(byte_path, b"value")
    assert not byte_path.exists()
    monkeypatch.setattr(os, "fsync", original_fsync)


def test_published_manifest_tamper_symlink_and_final_fault_fail_closed(
    tmp_path: Path,
) -> None:
    builder = FeatureDatasetBuilder(_universe(), _calendar())
    source = InMemoryCanonicalSource(
        tuple(_record(session, 0) for session in range(87))
    )
    repository = _repository(tmp_path)
    published = build_and_publish_feature_dataset(repository, _quota(), builder, source)
    original = published.manifest_path.read_bytes()
    published.manifest_path.chmod(0o600)
    published.manifest_path.write_bytes(b"{}")
    with pytest.raises(DatasetBuildError):
        build_and_publish_feature_dataset(repository, _quota(), builder, source)
    published.manifest_path.unlink()
    outside = tmp_path / "outside.json"
    outside.write_bytes(original)
    published.manifest_path.symlink_to(outside)
    with pytest.raises(DatasetBuildError):
        build_and_publish_feature_dataset(repository, _quota(), builder, source)

    other_repository = _repository(tmp_path / "other")
    observed: list[str] = []

    def final_fault(stage: str) -> None:
        observed.append(stage)
        if stage == "DATASET_MANIFEST_PUBLISHED":
            message = "injected final fault"
            raise RuntimeError(message)

    with pytest.raises(RuntimeError, match="injected final fault"):
        build_and_publish_feature_dataset(
            other_repository,
            _quota(),
            builder,
            source,
            fault_injector=final_fault,
        )
    assert "DATASET_MANIFEST_PUBLISHED" in observed


def test_publication_rejects_injected_leakage(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    builder = FeatureDatasetBuilder(_universe(), _calendar())
    source = InMemoryCanonicalSource(
        tuple(_record(session, 0) for session in range(87))
    )
    monkeypatch.setattr(
        feature_module,
        "_sample_leakage_reason",
        lambda _sample, _split: DatasetBuildCode.FUTURE_REFERENCE.value,
    )
    with pytest.raises(DatasetBuildError, match="leakage validation"):
        build_and_publish_feature_dataset(
            _repository(tmp_path), _quota(), builder, source
        )


def test_direct_publication_leakage_guards_cover_mutated_rows() -> None:
    _, _, split, _, _, samples = _pipeline()
    sample = samples[0]
    cases = (
        ("split", "TEST", DatasetBuildCode.NON_CHRONOLOGICAL_SPLIT.value),
        (
            "feature_end_exchange_time_ns",
            sample.feature_end_exchange_time_ns - 1,
            DatasetBuildCode.LABEL_FEATURE_OVERLAP.value,
        ),
        (
            "max_event_availability_time_ns",
            sample.knowledge_cutoff_time_ns + 1,
            DatasetBuildCode.FUTURE_EVENT_REVISION.value,
        ),
    )
    for field, value, reason in cases:
        candidate = replace(sample)
        object.__setattr__(candidate, field, value)
        assert feature_module._sample_leakage_reason(candidate, split) == reason
    overlap = replace(sample)
    labels = list(overlap.labels)
    selected = next(label for label in labels if label.validity is LabelValidity.VALID)
    object.__setattr__(
        selected,
        "target_exchange_time_ns",
        overlap.feature_end_exchange_time_ns,
    )
    assert (
        feature_module._sample_leakage_reason(overlap, split)
        == DatasetBuildCode.LABEL_FEATURE_OVERLAP.value
    )


def test_manifest_hash_tamper_reaches_identity_check(tmp_path: Path) -> None:
    builder = FeatureDatasetBuilder(_universe(), _calendar())
    source = InMemoryCanonicalSource(
        tuple(_record(session, 0) for session in range(87))
    )
    repository = _repository(tmp_path)
    published = build_and_publish_feature_dataset(repository, _quota(), builder, source)
    document = json.loads(published.manifest_path.read_bytes())
    document["manifest_sha256"] = "11" * 32
    published.manifest_path.chmod(0o600)
    published.manifest_path.write_text(json.dumps(document), encoding="ascii")
    with pytest.raises(DatasetBuildError):
        build_and_publish_feature_dataset(repository, _quota(), builder, source)


def test_idempotent_reopen_rejects_corrupted_dataset_object(tmp_path: Path) -> None:
    builder = FeatureDatasetBuilder(_universe(), _calendar())
    source = InMemoryCanonicalSource(
        tuple(_record(session, 0) for session in range(87))
    )
    repository = _repository(tmp_path)
    published = build_and_publish_feature_dataset(repository, _quota(), builder, source)
    manifest = json.loads(published.manifest_path.read_bytes())
    object_path = repository.root / manifest["objects"][0]["storage_path"]
    object_path.chmod(0o600)
    object_path.write_bytes(b"corrupt")
    with pytest.raises(DatasetBuildError, match="size mismatch"):
        build_and_publish_feature_dataset(repository, _quota(), builder, source)


@pytest.mark.parametrize("storage_path", ["", "../escape", "/absolute/path"])
def test_published_object_rejects_unsafe_storage_paths(
    tmp_path: Path, storage_path: str
) -> None:
    repository = _repository(tmp_path)
    with pytest.raises(DatasetBuildError, match="unsafe published object path"):
        feature_module._verify_published_objects(
            repository,
            (
                {
                    "object_sha256": "00" * 32,
                    "size_bytes_decimal": 1,
                    "storage_path": storage_path,
                },
            ),
        )


def test_published_object_rejects_missing_symlink_escape_and_hash_mismatch(
    tmp_path: Path,
) -> None:
    repository = _repository(tmp_path)

    def item(path: str, digest: str = "00" * 32) -> Mapping[str, object]:
        return {
            "object_sha256": digest,
            "size_bytes_decimal": 4,
            "storage_path": path,
        }

    with pytest.raises(DatasetBuildError, match="not a regular file"):
        feature_module._verify_published_objects(repository, (item("missing"),))
    outside = tmp_path / "outside"
    outside.mkdir()
    (outside / "object").write_bytes(b"data")
    direct_link = repository.root / "direct-link"
    direct_link.symlink_to(outside / "object")
    with pytest.raises(DatasetBuildError, match="not a regular file"):
        feature_module._verify_published_objects(repository, (item("direct-link"),))
    parent_link = repository.root / "parent-link"
    parent_link.symlink_to(outside, target_is_directory=True)
    with pytest.raises(DatasetBuildError, match="escapes repository root"):
        feature_module._verify_published_objects(
            repository, (item("parent-link/object"),)
        )
    local = repository.root / "local"
    local.write_bytes(b"data")
    with pytest.raises(DatasetBuildError, match="SHA-256 mismatch"):
        feature_module._verify_published_objects(repository, (item("local"),))


def test_successful_publication_preserves_untrusted_matching_symlink(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    builder, records = _small_build_inputs()
    source = InMemoryCanonicalSource(records)
    repository = _repository(tmp_path)
    outside = tmp_path / "outside-preserved"
    outside.write_bytes(b"keep")
    original_publish = repository.publish_staged_object
    injected: list[Path] = []

    def publish_and_inject(
        staged_relative_path: str,
        final_relative_path: str,
        *,
        expected_sha256: str,
        expected_size_bytes: int,
        lease: AdmissionLease,
    ) -> Path:
        result = original_publish(
            staged_relative_path,
            final_relative_path,
            expected_sha256=expected_sha256,
            expected_size_bytes=expected_size_bytes,
            lease=lease,
        )
        if not injected:
            untrusted = repository.root / f"{staged_relative_path}-untrusted"
            untrusted.symlink_to(outside)
            injected.append(untrusted)
        return result

    monkeypatch.setattr(repository, "publish_staged_object", publish_and_inject)
    build_and_publish_feature_dataset(repository, _quota(), builder, source)
    assert injected[0].is_symlink()
    assert outside.read_bytes() == b"keep"


def _small_build_inputs() -> tuple[
    FeatureDatasetBuilder, tuple[CanonicalMinuteRecord, ...]
]:
    return (
        FeatureDatasetBuilder(
            _universe(), _calendar(), maximum_records_per_instrument=100
        ),
        tuple(_record(session, 0) for session in range(87)),
    )


def test_publication_detects_actual_output_over_projection(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    builder, records = _small_build_inputs()
    source = _FaultSource((INSTRUMENT_HEX,), records, reported_count=10_000)
    monkeypatch.setattr(feature_module, "DEFAULT_OUTPUT_BYTES_PER_SAMPLE", -1_000)
    with pytest.raises(DatasetBuildError) as error:
        build_and_publish_feature_dataset(
            _repository(tmp_path), _quota(), builder, source
        )
    assert error.value.code is DatasetBuildCode.STORAGE_LIMIT


def test_publication_revalidates_purge_and_normalization(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    builder, records = _small_build_inputs()
    source = InMemoryCanonicalSource(records)
    original_split = feature_module.build_split_plan

    def bad_split(sessions: Sequence[str]) -> SplitPlan:
        return replace(original_split(sessions), first_purge_sessions=())

    monkeypatch.setattr(feature_module, "build_split_plan", bad_split)
    with pytest.raises(DatasetBuildError, match="leakage validation"):
        build_and_publish_feature_dataset(
            _repository(tmp_path / "purge"), _quota(), builder, source
        )
    monkeypatch.setattr(feature_module, "build_split_plan", original_split)
    original_fit = FeatureDatasetBuilder.fit

    def future_fit(
        active_builder: FeatureDatasetBuilder,
        active_source: object,
        active_split: SplitPlan,
        active_aggregates: Mapping[int, AggregatePoint],
    ) -> tuple[NormalizationStat, ...]:
        values = original_fit(
            active_builder,
            cast("InMemoryCanonicalSource", active_source),
            active_split,
            active_aggregates,
        )
        return tuple(
            replace(value, maximum_fit_exchange_time_ns=(1 << 63) - 1)
            for value in values
        )

    monkeypatch.setattr(FeatureDatasetBuilder, "fit", future_fit)
    with pytest.raises(DatasetBuildError, match="leakage validation"):
        build_and_publish_feature_dataset(
            _repository(tmp_path / "normalization"), _quota(), builder, source
        )


def test_publication_removes_only_its_failed_staging_file(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    builder, records = _small_build_inputs()
    source = InMemoryCanonicalSource(records)
    repository = _repository(tmp_path)

    def fail_publish(*_args: object, **_kwargs: object) -> Path:
        message = "injected object publication failure"
        raise RuntimeError(message)

    monkeypatch.setattr(repository, "publish_staged_object", fail_publish)
    with pytest.raises(RuntimeError, match="injected object publication failure"):
        build_and_publish_feature_dataset(repository, _quota(), builder, source)
    assert not tuple((repository.root / "tmp/feature-dataset").glob("*.part"))
