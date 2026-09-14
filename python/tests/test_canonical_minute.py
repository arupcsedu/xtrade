"""Streaming canonical-minute and persistent publication tests."""

from __future__ import annotations

import hashlib
import json
import os
import sqlite3
import zlib
from dataclasses import replace
from datetime import date
from decimal import Decimal
from pathlib import Path
from typing import TYPE_CHECKING, cast

import pyarrow.parquet as pq  # type: ignore[import-untyped]
import pytest
from aegis_mx_intelligence import Identifier128, InstrumentId
from aegis_mx_research import canonical_minute as minute_module
from aegis_mx_research.canonical_minute import (
    CANONICAL_MINUTE_SCHEMA_VERSION,
    NANOSECONDS_PER_MINUTE,
    CanonicalMinuteNormalizer,
    CanonicalMinuteSpool,
    MinuteDataQuality,
    MinuteNormalizationCode,
    MinuteNormalizationError,
    RawMinuteBar,
    RawPriceEncoding,
    ReferenceStoreMinuteResolver,
    TickSizeRevision,
    TickSizeStore,
    build_partition_quality,
    iter_source_minutes,
    publish_partition,
    write_partition_parquet,
)
from aegis_mx_research.data_repository import (
    DECIMAL_GB,
    DataRepository,
    FileSystemState,
    ManifestLineage,
    ManifestTimeRange,
    QuotaEvidence,
    SourceManifest,
    StoragePolicy,
    StorageRequest,
)
from aegis_mx_research.reference_data import (
    BusinessInterval,
    CalendarDayKind,
    CalendarDayRevision,
    HaltRevision,
    InstrumentRevision,
    ListingVenue,
    MappingStatus,
    PointInTimeReferenceStore,
    ReferenceDataError,
    ReferenceErrorCode,
    ReferenceProvenance,
    ReferenceSnapshot,
    SecurityType,
    SymbolMappingRevision,
)
from jsonschema import (  # type: ignore[import-untyped]
    Draft202012Validator,
    FormatChecker,
)

if TYPE_CHECKING:
    from collections.abc import Iterator

EVENT_NS = 1_789_133_400_000_000_000
KNOWN_NS = EVENT_NS - NANOSECONDS_PER_MINUTE
PROCESSING_NS = EVENT_NS + 8 * 3_600_000_000_000
UNIVERSE_SHA256 = hashlib.sha256(b"fixture-universe").hexdigest()


def _instrument(value: int = 1) -> InstrumentId:
    return InstrumentId(Identifier128(0xA600, value))


def _provenance(document: str, *, complete: bool = True) -> ReferenceProvenance:
    return ReferenceProvenance(
        provider="aegis-fixture",
        dataset="synthetic-point-in-time-reference",
        document_id=document,
        content_sha256=hashlib.sha256(document.encode()).hexdigest(),
        observed_at_ns=KNOWN_NS,
        processed_at_ns=KNOWN_NS,
        revision_at_ns=KNOWN_NS,
        source_publication_time_ns=KNOWN_NS - 1,
        historical_completeness=complete,
        limitation="synthetic test fixture",
    )


def _reference_resolver(
    *,
    symbol: str = "AAA",
    session_minutes: int = 2,
    mapping_effective_ns: int = EVENT_NS - NANOSECONDS_PER_MINUTE,
    tick_revisions: tuple[TickSizeRevision, ...] | None = None,
    complete: bool = True,
    include_instrument: bool = True,
    calendar_date: date = date(2026, 9, 11),
    halted: bool = False,
    mapping_status: MappingStatus = MappingStatus.RESOLVED_CURRENT_ONLY,
) -> ReferenceStoreMinuteResolver:
    instrument_id = _instrument()
    snapshot = ReferenceSnapshot(
        universe_source_sha256=UNIVERSE_SHA256,
        source_report_sha256=hashlib.sha256(b"fixture-source").hexdigest(),
        as_of_ns=KNOWN_NS,
        instruments=(
            InstrumentRevision(
                revision_id="instrument-aaa-v1",
                instrument_id=instrument_id,
                primary_symbol=symbol,
                listing_venue=ListingVenue.NASDAQ,
                security_type=SecurityType.COMMON_STOCK,
                listing_date=date(2020, 1, 1),
                delisting_date=None,
                effective=BusinessInterval(mapping_effective_ns),
                version=1,
                unresolved_fields=(),
                provenance=_provenance("instrument", complete=complete),
            ),
        )
        if include_instrument
        else (),
        mappings=(
            SymbolMappingRevision(
                mapping_id="mapping-aaa-v1",
                symbol=symbol,
                instrument_id=instrument_id,
                listing_venue=ListingVenue.NASDAQ,
                status=mapping_status,
                effective=BusinessInterval(mapping_effective_ns),
                version=1,
                provenance=_provenance("mapping", complete=complete),
            ),
        ),
        corporate_actions=(),
        calendar_days=(
            CalendarDayRevision(
                calendar_id="XNAS-FIXTURE",
                session_date=calendar_date,
                kind=CalendarDayKind.REGULAR_SESSION,
                open_exchange_time_ns=EVENT_NS,
                close_exchange_time_ns=(
                    EVENT_NS + session_minutes * NANOSECONDS_PER_MINUTE
                ),
                version=1,
                provenance=_provenance("calendar", complete=complete),
            ),
        ),
        halts=(
            HaltRevision(
                halt_id="halt-aaa-v1",
                instrument_id=instrument_id,
                start_exchange_time_ns=EVENT_NS,
                end_exchange_time_ns=EVENT_NS + NANOSECONDS_PER_MINUTE,
                version=1,
                provenance=_provenance("halt", complete=complete),
            ),
        )
        if halted
        else (),
        halt_coverage_complete=complete,
    )
    revisions = (
        tick_revisions
        if tick_revisions is not None
        else (
            TickSizeRevision(
                revision_id="tick-aaa-v1",
                symbol=symbol,
                tick_value_currency_nanos=10_000_000,
                currency="USD",
                effective_from_ns=mapping_effective_ns,
                effective_to_ns=None,
                available_at_ns=KNOWN_NS,
                version=1,
                source_sha256=hashlib.sha256(b"tick-source").hexdigest(),
                historical_completeness=complete,
            ),
        )
    )
    return ReferenceStoreMinuteResolver(
        PointInTimeReferenceStore(snapshot), TickSizeStore(revisions)
    )


def _raw(
    minute: int = 0,
    *,
    manifest_id: str = "source-" + "11" * 32,
    open_value: Decimal | int = Decimal("100.00"),
    high_value: Decimal | int = Decimal("100.05"),
    low_value: Decimal | int = Decimal("99.95"),
    close_value: Decimal | int = Decimal("100.01"),
    volume: int = 100,
    price_encoding: RawPriceEncoding = RawPriceEncoding.CURRENCY_UNITS_DECIMAL,
) -> RawMinuteBar:
    event_ns = EVENT_NS + minute * NANOSECONDS_PER_MINUTE
    return RawMinuteBar(
        symbol="AAA",
        minute_start_exchange_time_ns=event_ns,
        open_value=open_value,
        high_value=high_value,
        low_value=low_value,
        close_value=close_value,
        volume=volume,
        trade_count=10,
        vwap_value=Decimal("100.00")
        if price_encoding is RawPriceEncoding.CURRENCY_UNITS_DECIMAL
        else 10_000,
        price_encoding=price_encoding,
        source_manifest_id=manifest_id,
        source_object_id="fixture-source-object",
        source_record_id=f"AAA:{event_ns}",
        source_publication_time_ns=EVENT_NS + 3 * 3_600_000_000_000,
        source_availability_time_ns=EVENT_NS + 3 * 3_600_000_000_000,
        local_receipt_time_ns=EVENT_NS + 4 * 3_600_000_000_000,
        local_processing_time_ns=PROCESSING_NS,
        market_id="IEX",
    )


def _source_manifest(
    record_count: int, object_sha256: str = "22" * 32
) -> SourceManifest:
    return SourceManifest(
        source_name="fixture-minute-source",
        source_version="v1",
        source_object_id="fixture-source-object",
        storage_path=f"raw/fixture/{object_sha256}.json",
        object_sha256=object_sha256,
        size_bytes=1,
        record_count=record_count,
        schema_name="synthetic-minute-bars-jsonl",
        schema_version="1.0.0",
        times=ManifestTimeRange(
            event_time_min_ns=EVENT_NS,
            event_time_max_ns=EVENT_NS + NANOSECONDS_PER_MINUTE,
            publication_time_min_ns=EVENT_NS,
            publication_time_max_ns=EVENT_NS + NANOSECONDS_PER_MINUTE,
            receive_time_min_ns=EVENT_NS,
            receive_time_max_ns=EVENT_NS + NANOSECONDS_PER_MINUTE,
            processing_time_min_ns=EVENT_NS,
            processing_time_max_ns=EVENT_NS + NANOSECONDS_PER_MINUTE,
            revision_time_min_ns=EVENT_NS,
            revision_time_max_ns=EVENT_NS + NANOSECONDS_PER_MINUTE,
        ),
        universe_snapshot_sha256=UNIVERSE_SHA256,
        lineage=ManifestLineage(),
    )


def test_normalizer_converts_exact_prices_and_retains_all_provenance() -> None:
    record = CanonicalMinuteNormalizer(_reference_resolver()).normalize(_raw())
    assert record.schema_version == CANONICAL_MINUTE_SCHEMA_VERSION
    assert (
        record.open_ticks,
        record.high_ticks,
        record.low_ticks,
        record.close_ticks,
    ) == (
        10_000,
        10_005,
        9_995,
        10_001,
    )
    assert record.tick_value_currency_nanos == 10_000_000
    assert record.volume_shares == 100
    assert record.data_quality_state is MinuteDataQuality.VALID
    assert (
        record.record_sha256
        == hashlib.sha256(
            json.dumps(
                record.payload(include_hash=False),
                ensure_ascii=True,
                separators=(",", ":"),
                sort_keys=True,
            ).encode("ascii")
        ).hexdigest()
    )


def test_canonical_record_and_quality_documents_match_json_schemas(
    tmp_path: Path,
) -> None:
    schema_root = Path(__file__).resolve().parents[2] / "schemas"
    record_schema = json.loads(
        (schema_root / "canonical-minute-record-v1.schema.json").read_text()
    )
    record = CanonicalMinuteNormalizer(_reference_resolver()).normalize(_raw())
    Draft202012Validator(record_schema, format_checker=FormatChecker()).validate(
        record.payload()
    )

    manifest = _source_manifest(1)
    raw = replace(_raw(), source_manifest_id=manifest.manifest_id)
    with CanonicalMinuteSpool(
        tmp_path / "spool.sqlite3", instrument_buckets=1
    ) as spool:
        spool.ingest(manifest, (raw,), CanonicalMinuteNormalizer(_reference_resolver()))
        quality = build_partition_quality(spool, "2026-09-11", 0)
    assert quality.document["missing_minutes"] == 1
    assert quality.document["status"] == "DEGRADED"
    quality_schema = json.loads(
        (schema_root / "canonical-minute-quality-report-v1.schema.json").read_text()
    )
    Draft202012Validator(quality_schema, format_checker=FormatChecker()).validate(
        quality.document
    )


@pytest.mark.parametrize(
    ("changes", "code"),
    [
        ({"open_value": Decimal("100.005")}, MinuteNormalizationCode.INEXACT_PRICE),
        ({"high_value": Decimal("99.00")}, MinuteNormalizationCode.INVALID_PRICE),
        ({"volume": -1}, MinuteNormalizationCode.INVALID_QUANTITY),
        (
            {"minute_start_exchange_time_ns": EVENT_NS + 1},
            MinuteNormalizationCode.TIMESTAMP_DISORDER,
        ),
    ],
)
def test_normalizer_rejects_malformed_values(
    changes: dict[str, object], code: MinuteNormalizationCode
) -> None:
    normalizer = CanonicalMinuteNormalizer(_reference_resolver())
    with pytest.raises(MinuteNormalizationError) as captured:
        normalizer.normalize(replace(_raw(), **changes))  # type: ignore[arg-type]
    assert captured.value.code is code


def test_historical_reference_and_missing_tick_reject() -> None:
    current_only = _reference_resolver(mapping_effective_ns=PROCESSING_NS)
    with pytest.raises(MinuteNormalizationError) as captured:
        CanonicalMinuteNormalizer(current_only).normalize(_raw())
    assert captured.value.code is MinuteNormalizationCode.MISSING_REFERENCE

    no_ticks = _reference_resolver(tick_revisions=())
    with pytest.raises(MinuteNormalizationError) as captured:
        CanonicalMinuteNormalizer(no_ticks).normalize(_raw())
    assert captured.value.code is MinuteNormalizationCode.MISSING_TICK_SIZE


def test_tick_revision_and_store_validate_lineage_and_bounds() -> None:
    valid = _reference_resolver()._ticks.at(
        "AAA", event_time_ns=EVENT_NS, known_at_ns=PROCESSING_NS
    )
    assert valid.effective_at(EVENT_NS)
    assert not replace(valid, effective_to_ns=EVENT_NS + 1).effective_at(EVENT_NS + 1)
    invalid_changes = (
        {"revision_id": ""},
        {"tick_value_currency_nanos": 0},
        {"effective_to_ns": 1},
        {"version": 0},
        {"source_sha256": "bad"},
        {"source_sha256": "A" * 64},
        {"source_sha256": "g" * 64},
        {"source_sha256": "0" * 64},
    )
    for changes in invalid_changes:
        with pytest.raises(MinuteNormalizationError):
            replace(valid, **changes)

    with pytest.raises(MinuteNormalizationError, match="contiguous"):
        TickSizeStore((valid, replace(valid, revision_id="tick-v3", version=3)))
    with pytest.raises(MinuteNormalizationError, match="knowledge"):
        TickSizeStore(
            (
                valid,
                replace(valid, revision_id="tick-v2", version=2),
            )
        )
    second = replace(
        valid,
        revision_id="tick-v2",
        version=2,
        available_at_ns=valid.available_at_ns + 1,
        tick_value_currency_nanos=1_000_000,
    )
    store = TickSizeStore((valid, second))
    assert (
        store.at("AAA", event_time_ns=EVENT_NS, known_at_ns=second.available_at_ns)
        == second
    )
    with pytest.raises(MinuteNormalizationError, match="no known"):
        store.at("AAA", event_time_ns=EVENT_NS, known_at_ns=KNOWN_NS - 1)


def test_reference_resolution_rejects_missing_reference_calendar_and_halt() -> None:
    with pytest.raises(MinuteNormalizationError) as captured:
        CanonicalMinuteNormalizer(
            _reference_resolver(include_instrument=False)
        ).normalize(_raw())
    assert captured.value.code is MinuteNormalizationCode.MISSING_REFERENCE

    with pytest.raises(MinuteNormalizationError) as captured:
        CanonicalMinuteNormalizer(
            _reference_resolver(calendar_date=date(2026, 9, 10))
        ).normalize(_raw())
    assert captured.value.code is MinuteNormalizationCode.MISSING_CALENDAR

    with pytest.raises(MinuteNormalizationError) as captured:
        CanonicalMinuteNormalizer(_reference_resolver()).normalize(_raw(2))
    assert captured.value.code is MinuteNormalizationCode.OUTSIDE_SESSION

    with pytest.raises(MinuteNormalizationError) as captured:
        CanonicalMinuteNormalizer(_reference_resolver(halted=True)).normalize(_raw())
    assert captured.value.code is MinuteNormalizationCode.OUTSIDE_SESSION

    with pytest.raises(MinuteNormalizationError) as captured:
        CanonicalMinuteNormalizer(
            _reference_resolver(mapping_status=MappingStatus.UNSUPPORTED_VENUE)
        ).normalize(_raw())
    assert captured.value.code is MinuteNormalizationCode.MISSING_REFERENCE


def test_reference_resolution_wraps_mapping_conflict(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    resolver = _reference_resolver()

    def raise_conflict(*_args: object, **_kwargs: object) -> None:
        raise ReferenceDataError(ReferenceErrorCode.CONFLICT, "synthetic conflict")

    monkeypatch.setattr(PointInTimeReferenceStore, "symbol_mapping_at", raise_conflict)
    with pytest.raises(MinuteNormalizationError) as captured:
        resolver.resolve("AAA", event_time_ns=EVENT_NS, known_at_ns=PROCESSING_NS)
    assert captured.value.code is MinuteNormalizationCode.MISSING_REFERENCE


def test_incomplete_reference_and_missing_publication_are_explicitly_degraded() -> None:
    record = CanonicalMinuteNormalizer(_reference_resolver(complete=False)).normalize(
        replace(_raw(), source_publication_time_ns=None)
    )
    assert record.data_quality_state is MinuteDataQuality.DEGRADED
    assert record.data_quality_reasons == (
        "CALENDAR_HISTORY_INCOMPLETE",
        "HALT_HISTORY_INCOMPLETE",
        "INSTRUMENT_HISTORY_INCOMPLETE",
        "MAPPING_HISTORY_INCOMPLETE",
        "SOURCE_PUBLICATION_TIME_UNAVAILABLE",
        "TICK_HISTORY_INCOMPLETE",
    )


@pytest.mark.parametrize(
    "changes",
    [
        {"open_value": True},
        {"open_value": Decimal("NaN")},
        {"open_value": Decimal("0.0000000001")},
        {"open_value": Decimal(0)},
        {"open_value": Decimal(1 << 63)},
        {
            "price_encoding": RawPriceEncoding.RESOLVED_TICKS,
            "open_value": Decimal(1),
        },
        {"trade_count": -1},
        {"source_publication_time_ns": EVENT_NS + 5 * 3_600_000_000_000},
        {"local_receipt_time_ns": EVENT_NS},
        {"local_processing_time_ns": EVENT_NS},
    ],
)
def test_additional_numeric_and_temporal_fail_closed(
    changes: dict[str, object],
) -> None:
    with pytest.raises(MinuteNormalizationError):
        CanonicalMinuteNormalizer(_reference_resolver()).normalize(
            replace(_raw(), **changes)  # type: ignore[arg-type]
        )


def test_resolved_ticks_without_vwap_and_record_decoder_integrity() -> None:
    raw = replace(
        _raw(
            open_value=10_000,
            high_value=10_001,
            low_value=9_999,
            close_value=10_000,
            price_encoding=RawPriceEncoding.RESOLVED_TICKS,
        ),
        vwap_value=None,
        market_id=None,
    )
    record = CanonicalMinuteNormalizer(_reference_resolver()).normalize(raw)
    encoded = json.dumps(
        record.payload(), separators=(",", ":"), sort_keys=True
    ).encode()
    assert minute_module.CanonicalMinuteRecord.from_bytes(encoded) == record
    for corrupted in (b"[]", b"{}", b"not-json"):
        with pytest.raises(MinuteNormalizationError) as captured:
            minute_module.CanonicalMinuteRecord.from_bytes(corrupted)
        assert captured.value.code is MinuteNormalizationCode.CORRUPT_SPOOL
    tampered = record.payload()
    tampered["record_sha256"] = "1" * 64
    with pytest.raises(MinuteNormalizationError, match="hash"):
        minute_module.CanonicalMinuteRecord.from_bytes(
            json.dumps(tampered, separators=(",", ":"), sort_keys=True).encode()
        )


def test_normalizer_covers_optional_trade_count_and_overflow_guards() -> None:
    normalizer = CanonicalMinuteNormalizer(_reference_resolver())
    record = normalizer.normalize(replace(_raw(), trade_count=None, vwap_value=None))
    assert record.trade_count is None
    assert record.vwap_ticks is None
    with pytest.raises(MinuteNormalizationError, match="overflows"):
        normalizer.normalize(
            replace(
                _raw(),
                minute_start_exchange_time_ns=(
                    minute_module.MAX_INT64 // NANOSECONDS_PER_MINUTE
                )
                * NANOSECONDS_PER_MINUTE,
            )
        )
    with pytest.raises(MinuteNormalizationError, match="finite decimal"):
        normalizer._currency_nanos(object())  # type: ignore[arg-type]
    assert minute_module._decimal_source_value(Decimal("1.25")) == Decimal("1.25")


def test_spool_is_idempotent_detects_conflicts_and_recovers_transactions(
    tmp_path: Path,
) -> None:
    manifest = _source_manifest(2)
    records = (
        replace(_raw(0), source_manifest_id=manifest.manifest_id),
        replace(_raw(1), source_manifest_id=manifest.manifest_id),
    )
    normalizer = CanonicalMinuteNormalizer(_reference_resolver())
    with CanonicalMinuteSpool(
        tmp_path / "spool.sqlite3", instrument_buckets=1
    ) as spool:
        with pytest.raises(RuntimeError, match="injected"):
            spool.ingest(
                manifest,
                records,
                normalizer,
                fault_injector=lambda _stage: (_ for _ in ()).throw(
                    RuntimeError("injected crash")
                ),
            )
        assert spool.partition_keys() == ()
        first = spool.ingest(manifest, records, normalizer)
        assert (first.inserted_records, first.identical_duplicates) == (2, 0)
        assert first.rejected_outside_session == 0
        assert spool.record_count == 2
        assert spool.ingest_totals() == {
            "identical_duplicates": 0,
            "input_records": 2,
            "inserted_records": 2,
            "rejected_outside_session": 0,
            "source_objects": 1,
        }
        replay = spool.ingest(manifest, records, normalizer)
        assert replay.already_processed
        duplicate_manifest = _source_manifest(2, "33" * 32)
        duplicate_records = tuple(
            replace(record, source_manifest_id=duplicate_manifest.manifest_id)
            for record in records
        )
        duplicate = spool.ingest(duplicate_manifest, duplicate_records, normalizer)
        assert duplicate.identical_duplicates == 2

        conflict_manifest = _source_manifest(1, "44" * 32)
        conflict = replace(
            _raw(0, close_value=Decimal("100.02")),
            source_manifest_id=conflict_manifest.manifest_id,
        )
        with pytest.raises(MinuteNormalizationError) as captured:
            spool.ingest(conflict_manifest, (conflict,), normalizer)
        assert captured.value.code is MinuteNormalizationCode.CONFLICTING_DUPLICATE


def test_spool_records_regular_session_filtering_without_hiding_input(
    tmp_path: Path,
) -> None:
    manifest = _source_manifest(2)
    records = (
        replace(_raw(0), source_manifest_id=manifest.manifest_id),
        replace(_raw(2), source_manifest_id=manifest.manifest_id),
    )
    with CanonicalMinuteSpool(
        tmp_path / "filtered.sqlite3", instrument_buckets=1
    ) as spool:
        result = spool.ingest(
            manifest,
            records,
            CanonicalMinuteNormalizer(_reference_resolver()),
        )
        assert result.input_records == 2
        assert result.inserted_records == 1
        assert result.rejected_outside_session == 1
        assert spool.record_count == 1
        replay = spool.ingest(
            manifest,
            records,
            CanonicalMinuteNormalizer(_reference_resolver()),
        )
        assert replay.already_processed
        assert replay.rejected_outside_session == 1


@pytest.mark.parametrize(
    "kwargs",
    [
        {"instrument_buckets": 0},
        {"instrument_buckets": 257},
        {"maximum_records": 0},
        {"maximum_bytes": 1},
    ],
)
def test_spool_rejects_invalid_bounds(tmp_path: Path, kwargs: dict[str, int]) -> None:
    with pytest.raises(ValueError, match=r"from|positive|too small"):
        CanonicalMinuteSpool(tmp_path / "spool.sqlite3", **kwargs)


def test_spool_rejects_symlink_corruption_disorder_count_and_capacity(
    tmp_path: Path,
) -> None:
    target = tmp_path / "target.sqlite3"
    target.touch()
    link = tmp_path / "spool-link.sqlite3"
    link.symlink_to(target)
    with pytest.raises(MinuteNormalizationError, match="symlink"):
        CanonicalMinuteSpool(link)

    corrupt = tmp_path / "corrupt.sqlite3"
    corrupt.write_bytes(b"not sqlite")
    with pytest.raises(MinuteNormalizationError, match="opened"):
        CanonicalMinuteSpool(corrupt)

    metadata = tmp_path / "metadata.sqlite3"
    connection = sqlite3.connect(metadata)
    connection.execute("CREATE TABLE metadata(key TEXT PRIMARY KEY, value TEXT)")
    connection.execute("INSERT INTO metadata VALUES('schema_version', 'bad')")
    connection.commit()
    connection.close()
    with pytest.raises(MinuteNormalizationError, match="schema"):
        CanonicalMinuteSpool(metadata)

    normalizer = CanonicalMinuteNormalizer(_reference_resolver())
    disorder_manifest = _source_manifest(2)
    disorder = (
        replace(_raw(1), source_manifest_id=disorder_manifest.manifest_id),
        replace(_raw(0), source_manifest_id=disorder_manifest.manifest_id),
    )
    with CanonicalMinuteSpool(
        tmp_path / "bounded.sqlite3", instrument_buckets=1, maximum_records=1
    ) as spool:
        invalid_manifest = _source_manifest(1, "44" * 32)
        invalid = replace(
            _raw(), volume=-1, source_manifest_id=invalid_manifest.manifest_id
        )
        with pytest.raises(MinuteNormalizationError) as captured:
            spool.ingest(invalid_manifest, (invalid,), normalizer)
        assert captured.value.code is MinuteNormalizationCode.INVALID_QUANTITY
        with pytest.raises(MinuteNormalizationError) as captured:
            spool.ingest(disorder_manifest, disorder, normalizer)
        assert captured.value.code is MinuteNormalizationCode.TIMESTAMP_DISORDER
        with pytest.raises(MinuteNormalizationError, match="count"):
            spool.ingest(_source_manifest(2, "55" * 32), (_raw(),), normalizer)
        capacity_manifest = _source_manifest(2, "66" * 32)
        capacity_records = tuple(
            replace(_raw(index), source_manifest_id=capacity_manifest.manifest_id)
            for index in range(2)
        )
        with pytest.raises(MinuteNormalizationError) as captured:
            spool.ingest(capacity_manifest, capacity_records, normalizer)
        assert captured.value.code is MinuteNormalizationCode.RECORD_LIMIT
        assert spool.duplicate_count(()) == 0
        with pytest.raises(ValueError, match="batch_rows"):
            tuple(spool.records_for_partition("2026-09-11", 0, batch_rows=0))
        spool.close()
        spool.close()


def test_spool_contains_open_and_schema_initialization_failures(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(
        sqlite3,
        "connect",
        lambda *_args, **_kwargs: (_ for _ in ()).throw(
            sqlite3.DatabaseError("connect failure")
        ),
    )
    with pytest.raises(MinuteNormalizationError) as captured:
        CanonicalMinuteSpool(tmp_path / "connect.sqlite3")
    assert captured.value.code is MinuteNormalizationCode.CORRUPT_SPOOL
    monkeypatch.undo()

    page_limited = tmp_path / "page-limited.sqlite3"
    connection = sqlite3.connect(page_limited)
    connection.execute("PRAGMA page_size=65536")
    connection.execute("VACUUM")
    connection.close()
    with pytest.raises(MinuteNormalizationError) as captured:
        CanonicalMinuteSpool(page_limited, maximum_bytes=65_536)
    assert captured.value.code is MinuteNormalizationCode.RECORD_LIMIT


def test_spool_maps_sqlite_full_to_bounded_record_limit(tmp_path: Path) -> None:
    manifest = _source_manifest(100)
    records = tuple(
        replace(_raw(index), source_manifest_id=manifest.manifest_id)
        for index in range(100)
    )
    with CanonicalMinuteSpool(
        tmp_path / "byte-bounded.sqlite3",
        instrument_buckets=1,
        maximum_records=1_000,
        maximum_bytes=65_536,
    ) as spool:
        with pytest.raises(MinuteNormalizationError) as captured:
            spool.ingest(
                manifest,
                records,
                CanonicalMinuteNormalizer(_reference_resolver(session_minutes=100)),
            )
        assert captured.value.code is MinuteNormalizationCode.RECORD_LIMIT
        assert spool.partition_keys() == ()


def test_spool_maps_unexpected_database_error_to_corruption(tmp_path: Path) -> None:
    spool = CanonicalMinuteSpool(tmp_path / "broken.sqlite3", instrument_buckets=1)
    spool._connection.execute("DROP TABLE records")
    manifest = _source_manifest(1)
    with pytest.raises(MinuteNormalizationError) as captured:
        spool.ingest(
            manifest,
            (replace(_raw(), source_manifest_id=manifest.manifest_id),),
            CanonicalMinuteNormalizer(_reference_resolver()),
        )
    assert captured.value.code is MinuteNormalizationCode.CORRUPT_SPOOL
    spool.close()


def test_spool_detects_processed_hash_change_and_corrupt_record(tmp_path: Path) -> None:
    manifest = _source_manifest(1)
    raw = replace(_raw(), source_manifest_id=manifest.manifest_id)
    path = tmp_path / "spool.sqlite3"
    spool = CanonicalMinuteSpool(path, instrument_buckets=1)
    spool.ingest(manifest, (raw,), CanonicalMinuteNormalizer(_reference_resolver()))
    spool._connection.execute(
        "UPDATE processed_sources SET object_sha256='changed' "
        "WHERE source_manifest_id=?",
        (manifest.manifest_id,),
    )
    with pytest.raises(MinuteNormalizationError) as captured:
        spool.ingest(manifest, (raw,), CanonicalMinuteNormalizer(_reference_resolver()))
    assert captured.value.code is MinuteNormalizationCode.HASH_MISMATCH
    spool._connection.execute(
        "UPDATE records SET record_json='{}' WHERE instrument_id=?",
        (_instrument().hex(),),
    )
    with pytest.raises(MinuteNormalizationError) as captured:
        tuple(spool.records_for_partition("2026-09-11", 0, batch_rows=1))
    assert captured.value.code is MinuteNormalizationCode.CORRUPT_SPOOL
    spool.close()


def test_spool_record_compression_is_bounded_and_fail_closed() -> None:
    payload = b'{"record":"fixture"}'
    encoded = minute_module._encode_spool_record(payload)
    assert minute_module._decode_spool_record(encoded) == payload
    with pytest.raises(MinuteNormalizationError, match="encoding bound"):
        minute_module._encode_spool_record(
            b"x" * (minute_module._MAX_SPOOL_RECORD_BYTES + 1)
        )
    malformed = (
        minute_module._SPOOL_RECORD_MAGIC + b"not-zlib",
        encoded[:-1],
        encoded + b"trailing",
        minute_module._SPOOL_RECORD_MAGIC
        + zlib.compress(b"x" * (minute_module._MAX_SPOOL_RECORD_BYTES + 1)),
    )
    for value in malformed:
        with pytest.raises(MinuteNormalizationError) as captured:
            minute_module._decode_spool_record(value)
        assert captured.value.code is MinuteNormalizationCode.CORRUPT_SPOOL


def test_spool_chooses_canonical_provenance_and_contains_sync_failure(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    manifests = (_source_manifest(1, "ee" * 32), _source_manifest(1, "11" * 32))
    normalizer = CanonicalMinuteNormalizer(_reference_resolver())
    candidates = tuple(
        (manifest, replace(_raw(), source_manifest_id=manifest.manifest_id))
        for manifest in manifests
    )
    ordered = sorted(
        candidates,
        key=lambda item: minute_module._canonical_bytes(
            normalizer.normalize(item[1]).payload()
        ),
        reverse=True,
    )
    with CanonicalMinuteSpool(
        tmp_path / "provenance.sqlite3", instrument_buckets=1
    ) as spool:
        spool.ingest(ordered[0][0], (ordered[0][1],), normalizer)
        spool.ingest(ordered[1][0], (ordered[1][1],), normalizer)
        selected = tuple(spool.records_for_partition("2026-09-11", 0, batch_rows=1))
        assert selected[0][0].source_manifest_id == ordered[1][0].manifest_id

    with CanonicalMinuteSpool(
        tmp_path / "provenance-forward.sqlite3", instrument_buckets=1
    ) as spool:
        spool.ingest(ordered[1][0], (ordered[1][1],), normalizer)
        spool.ingest(ordered[0][0], (ordered[0][1],), normalizer)

    spool = CanonicalMinuteSpool(tmp_path / "sync.sqlite3", instrument_buckets=1)
    monkeypatch.setattr(
        spool,
        "_sync_parent",
        lambda: (_ for _ in ()).throw(RuntimeError("sync failed")),
    )
    with pytest.raises(RuntimeError, match="sync failed"):
        spool.ingest(manifests[0], (candidates[0][1],), normalizer)
    assert not spool._connection.in_transaction
    spool.close()


def test_parquet_is_deterministic_zstd_and_quality_precedes_acceptance(
    tmp_path: Path,
) -> None:
    manifest = _source_manifest(2)
    records = tuple(
        replace(_raw(minute), source_manifest_id=manifest.manifest_id)
        for minute in range(2)
    )
    with CanonicalMinuteSpool(
        tmp_path / "spool.sqlite3", instrument_buckets=1
    ) as spool:
        spool.ingest(
            manifest, records, CanonicalMinuteNormalizer(_reference_resolver())
        )
        quality = build_partition_quality(spool, "2026-09-11", 0, batch_rows=1)
        assert quality.accepted
        assert quality.document["status"] == "VALID"
        assert quality.document["missing_minutes"] == 0
        first = write_partition_parquet(
            spool, "2026-09-11", 0, tmp_path / "first.parquet", batch_rows=1
        )
        second = write_partition_parquet(
            spool, "2026-09-11", 0, tmp_path / "second.parquet", batch_rows=1
        )
    assert first.sha256 == second.sha256
    assert first.record_count == 2
    metadata = pq.read_metadata(first.path)
    assert metadata.num_rows == 2
    assert {
        metadata.row_group(group).column(column).compression
        for group in range(metadata.num_row_groups)
        for column in range(metadata.num_columns)
    } == {"ZSTD"}


def test_quality_reports_degraded_records_reasons_and_missing_minutes(
    tmp_path: Path,
) -> None:
    manifest = _source_manifest(1)
    raw = replace(_raw(), source_manifest_id=manifest.manifest_id)
    with CanonicalMinuteSpool(
        tmp_path / "spool.sqlite3", instrument_buckets=1
    ) as spool:
        spool.ingest(
            manifest,
            (raw,),
            CanonicalMinuteNormalizer(_reference_resolver(complete=False)),
        )
        quality = build_partition_quality(spool, "2026-09-11", 0)
    assert quality.document["status"] == "DEGRADED"
    assert quality.document["degraded_records"] == 1
    assert quality.document["missing_minutes"] == 1
    assert quality.document["data_quality_reason_counts"]


def test_empty_or_existing_parquet_paths_fail_closed(tmp_path: Path) -> None:
    with CanonicalMinuteSpool(
        tmp_path / "spool.sqlite3", instrument_buckets=1
    ) as spool:
        quality = build_partition_quality(spool, "2026-09-11", 0)
        assert not quality.accepted
        assert quality.document["status"] == "REJECTED"
        with pytest.raises(MinuteNormalizationError, match="no records"):
            write_partition_parquet(spool, "2026-09-11", 0, tmp_path / "empty.parquet")
        with pytest.raises(ValueError, match="batch_rows"):
            write_partition_parquet(
                spool, "2026-09-11", 0, tmp_path / "invalid.parquet", batch_rows=0
            )
        existing = tmp_path / "existing.parquet"
        existing.touch()
        with pytest.raises(MinuteNormalizationError, match="must not already"):
            write_partition_parquet(spool, "2026-09-11", 0, existing)


def test_parquet_verification_rejects_row_count_and_codec(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    manifest = _source_manifest(1)
    raw = replace(_raw(), source_manifest_id=manifest.manifest_id)
    with CanonicalMinuteSpool(
        tmp_path / "spool.sqlite3", instrument_buckets=1
    ) as spool:
        spool.ingest(manifest, (raw,), CanonicalMinuteNormalizer(_reference_resolver()))
        actual_metadata = pq.read_metadata

        class _BadRows:
            num_rows = 0

        monkeypatch.setattr(pq, "read_metadata", lambda _path: _BadRows())
        with pytest.raises(MinuteNormalizationError, match="row count"):
            write_partition_parquet(spool, "2026-09-11", 0, tmp_path / "rows.parquet")

        monkeypatch.setattr(pq, "read_metadata", actual_metadata)
        valid_path = tmp_path / "source.parquet"
        write_partition_parquet(spool, "2026-09-11", 0, valid_path)
        metadata = actual_metadata(valid_path)

        class _BadColumn:
            compression = "SNAPPY"

        class _BadGroup:
            def column(self, _column: int) -> _BadColumn:
                return _BadColumn()

        class _BadCodec:
            num_rows = metadata.num_rows
            num_row_groups = 1
            num_columns = 1

            def row_group(self, _group: int) -> _BadGroup:
                return _BadGroup()

        valid_path.unlink()
        monkeypatch.setattr(pq, "read_metadata", lambda _path: _BadCodec())
        with pytest.raises(MinuteNormalizationError, match="Zstandard"):
            write_partition_parquet(spool, "2026-09-11", 0, tmp_path / "codec.parquet")


def _repository(tmp_path: Path) -> DataRepository:
    repository = DataRepository(
        tmp_path / "data",
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
        observed_at_utc="2026-09-11T12:00:00Z",
        authoritative=True,
    )


def _publish_source(repository: DataRepository, count: int) -> SourceManifest:
    payload = b"x"
    digest = hashlib.sha256(payload).hexdigest()
    manifest = _source_manifest(count, digest)
    staged = repository.root / "tmp/source.part"
    staged.write_bytes(payload)
    with repository.acquire_admission(
        StorageRequest("source", output_bytes=1_000_000, temporary_bytes=1), _quota()
    ) as lease:
        repository.publish_staged_object(
            "tmp/source.part",
            manifest.storage_path,
            expected_sha256=digest,
            expected_size_bytes=1,
            lease=lease,
        )
        repository.publish_manifest(manifest, lease)
    return manifest


def test_publication_uses_manifest_as_final_acceptance_marker(tmp_path: Path) -> None:
    repository = _repository(tmp_path)
    manifest = _publish_source(repository, 2)
    records = tuple(
        replace(_raw(minute), source_manifest_id=manifest.manifest_id)
        for minute in range(2)
    )
    with CanonicalMinuteSpool(
        tmp_path / "spool.sqlite3", instrument_buckets=1
    ) as spool:
        spool.ingest(
            manifest, records, CanonicalMinuteNormalizer(_reference_resolver())
        )
        published = publish_partition(
            repository,
            _quota(),
            spool,
            "2026-09-11",
            0,
            universe_snapshot_sha256=UNIVERSE_SHA256,
            batch_rows=1,
        )
    assert published.quality_report_path.exists()
    assert published.parquet_path.exists()
    assert published.manifest_path.exists()
    assert published.manifest_path.name == f"{published.manifest_id}.json"


def test_publication_recovers_after_quality_only_orphan(tmp_path: Path) -> None:
    repository = _repository(tmp_path)
    manifest = _publish_source(repository, 2)
    records = tuple(
        replace(_raw(minute), source_manifest_id=manifest.manifest_id)
        for minute in range(2)
    )
    with CanonicalMinuteSpool(
        tmp_path / "spool.sqlite3", instrument_buckets=1
    ) as spool:
        spool.ingest(
            manifest, records, CanonicalMinuteNormalizer(_reference_resolver())
        )
        with pytest.raises(RuntimeError, match="publication crash"):
            publish_partition(
                repository,
                _quota(),
                spool,
                "2026-09-11",
                0,
                universe_snapshot_sha256=UNIVERSE_SHA256,
                batch_rows=1,
                fault_injector=lambda stage: (
                    (_ for _ in ()).throw(RuntimeError("publication crash"))
                    if stage == "QUALITY_PUBLISHED"
                    else None
                ),
            )
        assert not tuple((repository.root / "manifests/partition").glob("*.json"))
        recovered = publish_partition(
            repository,
            _quota(),
            spool,
            "2026-09-11",
            0,
            universe_snapshot_sha256=UNIVERSE_SHA256,
            batch_rows=1,
        )
    assert recovered.manifest_path.exists()


def test_publication_rejects_empty_oversized_and_parquet_stage_faults(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    empty_repository = _repository(tmp_path / "empty")
    with (
        CanonicalMinuteSpool(
            tmp_path / "empty-spool.sqlite3", instrument_buckets=1
        ) as empty_spool,
        pytest.raises(MinuteNormalizationError, match="empty partition"),
    ):
        publish_partition(
            empty_repository,
            _quota(),
            empty_spool,
            "2026-09-11",
            0,
            universe_snapshot_sha256=UNIVERSE_SHA256,
        )

    repository = _repository(tmp_path / "oversized")
    manifest = _publish_source(repository, 1)
    raw = replace(_raw(), source_manifest_id=manifest.manifest_id)
    with CanonicalMinuteSpool(
        tmp_path / "oversized-spool.sqlite3", instrument_buckets=1
    ) as spool:
        spool.ingest(manifest, (raw,), CanonicalMinuteNormalizer(_reference_resolver()))
        real_writer = minute_module.write_partition_parquet

        def oversized_writer(*args: object, **kwargs: object) -> object:
            result = real_writer(*args, **kwargs)  # type: ignore[arg-type]
            return replace(result, size_bytes=10_000_000)

        monkeypatch.setattr(minute_module, "write_partition_parquet", oversized_writer)
        with pytest.raises(MinuteNormalizationError, match="admitted upper bound"):
            publish_partition(
                repository,
                _quota(),
                spool,
                "2026-09-11",
                0,
                universe_snapshot_sha256=UNIVERSE_SHA256,
            )
        monkeypatch.setattr(minute_module, "write_partition_parquet", real_writer)

    repository = _repository(tmp_path / "parquet-fault")
    manifest = _publish_source(repository, 1)
    raw = replace(_raw(), source_manifest_id=manifest.manifest_id)
    with CanonicalMinuteSpool(
        tmp_path / "fault-spool.sqlite3", instrument_buckets=1
    ) as spool:
        spool.ingest(manifest, (raw,), CanonicalMinuteNormalizer(_reference_resolver()))
        with pytest.raises(RuntimeError, match="parquet fault"):
            publish_partition(
                repository,
                _quota(),
                spool,
                "2026-09-11",
                0,
                universe_snapshot_sha256=UNIVERSE_SHA256,
                fault_injector=lambda stage: (
                    (_ for _ in ()).throw(RuntimeError("parquet fault"))
                    if stage == "PARQUET_PUBLISHED"
                    else None
                ),
            )
        assert not tuple((repository.root / "manifests/partition").glob("*.json"))


def test_staged_write_removes_partial_output_on_sync_failure(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    path = tmp_path / "staged.part"
    monkeypatch.setattr(
        os,
        "fsync",
        lambda _descriptor: (_ for _ in ()).throw(OSError("sync failure")),
    )
    with pytest.raises(OSError, match="sync failure"):
        minute_module._write_staged(path, b"payload")
    assert not path.exists()


def test_source_reader_verifies_hash_count_and_streams_synthetic_lines(
    tmp_path: Path,
) -> None:
    root = tmp_path / "data"
    path = root / "raw/fixture/bars.jsonl"
    path.parent.mkdir(parents=True)
    lines = [
        {
            "close_ticks": 10_000,
            "event_time_ns": EVENT_NS,
            "high_ticks": 10_001,
            "low_ticks": 9_999,
            "open_ticks": 10_000,
            "quantity_units": 10,
            "symbol": "AAA",
        },
        {
            "close_ticks": 10_001,
            "event_time_ns": EVENT_NS + NANOSECONDS_PER_MINUTE,
            "high_ticks": 10_002,
            "low_ticks": 10_000,
            "open_ticks": 10_000,
            "quantity_units": 11,
            "symbol": "AAA",
        },
    ]
    payload = b"".join(
        json.dumps(line, separators=(",", ":"), sort_keys=True).encode() + b"\n"
        for line in lines
    )
    path.write_bytes(payload)
    digest = hashlib.sha256(payload).hexdigest()
    manifest = replace(
        _source_manifest(2, digest),
        storage_path="raw/fixture/bars.jsonl",
        size_bytes=len(payload),
    )
    records: Iterator[RawMinuteBar] = iter_source_minutes(root, manifest)
    assert [record.minute_start_exchange_time_ns for record in records] == [
        EVENT_NS,
        EVENT_NS + NANOSECONDS_PER_MINUTE,
    ]
    path.write_bytes(payload + b"corruption")
    with pytest.raises(MinuteNormalizationError) as captured:
        tuple(iter_source_minutes(root, manifest))
    assert captured.value.code is MinuteNormalizationCode.HASH_MISMATCH


def _manifest_for_payload(
    root: Path,
    payload: bytes,
    *,
    schema: str,
    record_count: int,
    filename: str = "object.json",
) -> SourceManifest:
    digest = hashlib.sha256(payload).hexdigest()
    relative = f"raw/fixture/{filename}"
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)
    return replace(
        _source_manifest(record_count, digest),
        storage_path=relative,
        size_bytes=len(payload),
        schema_name=schema,
    )


def test_alpaca_source_reader_preserves_decimal_fields(tmp_path: Path) -> None:
    root = tmp_path / "data"
    payload = json.dumps(
        {
            "bars": {
                "AAA": [
                    {
                        "c": 100,
                        "h": "100.01",
                        "l": 99,
                        "n": 2,
                        "o": 100,
                        "t": "2026-09-11T13:30:00Z",
                        "v": 10,
                        "vw": "100",
                    }
                ]
            },
            "next_page_token": None,
        },
        separators=(",", ":"),
        sort_keys=True,
    ).encode()
    manifest = _manifest_for_payload(
        root,
        payload,
        schema="alpaca-market-data-v2-bars-response",
        record_count=1,
    )
    processing_time = manifest.times.processing_time_min_ns + 1_000
    records = tuple(
        iter_source_minutes(
            root, manifest, canonical_processing_time_ns=processing_time
        )
    )
    assert records[0].open_value == Decimal(100)
    assert records[0].vwap_value == Decimal(100)
    assert records[0].market_id == "IEX"
    assert records[0].source_publication_time_ns is None
    assert records[0].local_processing_time_ns == processing_time


def test_alpaca_source_reader_rejects_timestamp_disorder(tmp_path: Path) -> None:
    root = tmp_path / "data"
    bars = [
        {"c": 100, "h": 100, "l": 100, "o": 100, "t": timestamp, "v": 1}
        for timestamp in (
            "2026-09-11T13:31:00Z",
            "2026-09-11T13:30:00Z",
        )
    ]
    payload = json.dumps({"bars": {"AAA": bars}}).encode()
    manifest = _manifest_for_payload(
        root,
        payload,
        schema="alpaca-market-data-v2-bars-response",
        record_count=2,
    )
    with pytest.raises(MinuteNormalizationError) as captured:
        tuple(iter_source_minutes(root, manifest))
    assert captured.value.code is MinuteNormalizationCode.TIMESTAMP_DISORDER


@pytest.mark.parametrize(
    "document",
    [
        b"not-json",
        b"[]",
        b'{"bars":{"AAA":1}}',
        b'{"bars":{"AAA":[1]}}',
        b'{"bars":{"AAA":[{"t":"bad","o":1,"h":1,"l":1,"c":1,"v":1}]}}',
        b'{"bars":{"AAA":[{"t":"2026-09-11T13:30:00Z","o":{},"h":1,"l":1,"c":1,"v":1}]}}',
        b'{"bars":{"AAA":[{"t":"2026-09-11T13:30:00Z","o":1,"h":1,"l":1,"c":1,"v":"1"}]}}',
    ],
)
def test_alpaca_source_reader_rejects_malformed_documents(
    tmp_path: Path, document: bytes
) -> None:
    root = tmp_path / "data"
    manifest = _manifest_for_payload(
        root,
        document,
        schema="alpaca-market-data-v2-bars-response",
        record_count=1,
    )
    with pytest.raises(MinuteNormalizationError):
        tuple(iter_source_minutes(root, manifest))


@pytest.mark.parametrize("timestamp", [1, "2026-09-11", "not-a-timeZ"])
def test_utc_parser_rejects_ambiguous_or_malformed_values(timestamp: object) -> None:
    with pytest.raises(MinuteNormalizationError):
        minute_module._parse_utc_ns(timestamp)


@pytest.mark.parametrize(
    "payload",
    [b"not-json\n", b"[]\n", b'{"symbol":"AAA"}\n'],
)
def test_synthetic_source_reader_rejects_malformed_lines(
    tmp_path: Path, payload: bytes
) -> None:
    root = tmp_path / "data"
    manifest = _manifest_for_payload(
        root,
        payload,
        schema="synthetic-minute-bars-jsonl",
        record_count=1,
    )
    with pytest.raises(MinuteNormalizationError):
        tuple(iter_source_minutes(root, manifest))


def test_source_reader_bounds_schema_counts_and_paths(tmp_path: Path) -> None:
    root = tmp_path / "data"
    valid = (
        b'{"close_ticks":1,"event_time_ns":1789133400000000000,'
        b'"high_ticks":1,"low_ticks":1,"open_ticks":1,'
        b'"quantity_units":0,"symbol":"AAA"}\n'
    )
    manifest = _manifest_for_payload(
        root,
        b"\n" + valid,
        schema="synthetic-minute-bars-jsonl",
        record_count=1,
    )
    assert len(tuple(iter_source_minutes(root, manifest))) == 1
    with pytest.raises(MinuteNormalizationError) as captured:
        tuple(iter_source_minutes(root, manifest, maximum_source_object_bytes=0))
    assert captured.value.code is MinuteNormalizationCode.SOURCE_TOO_LARGE
    with pytest.raises(MinuteNormalizationError) as captured:
        tuple(iter_source_minutes(root, manifest, maximum_source_object_bytes=1))
    assert captured.value.code is MinuteNormalizationCode.SOURCE_TOO_LARGE

    unsupported = replace(manifest, schema_name="unsupported")
    with pytest.raises(MinuteNormalizationError) as captured:
        tuple(iter_source_minutes(root, unsupported))
    assert captured.value.code is MinuteNormalizationCode.UNSUPPORTED_SCHEMA

    for count in (0, 2):
        wrong_count = replace(manifest, record_count=count)
        with pytest.raises(MinuteNormalizationError, match="record count"):
            tuple(iter_source_minutes(root, wrong_count))

    path = root / manifest.storage_path
    path.unlink()
    target = root / "raw/fixture/target.json"
    target.write_bytes(b"x")
    path.symlink_to(target)
    with pytest.raises(MinuteNormalizationError, match="symlink"):
        tuple(iter_source_minutes(root, manifest))
    path.unlink()
    path.mkdir()
    with pytest.raises(MinuteNormalizationError, match="regular"):
        tuple(iter_source_minutes(root, manifest))


def test_source_reader_rejects_hardlink_and_read_failure(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    root = tmp_path / "data"
    payload = b"{}"
    manifest = _manifest_for_payload(
        root, payload, schema="synthetic-minute-bars-jsonl", record_count=1
    )
    path = root / manifest.storage_path
    other = root / "raw/fixture/hardlink.json"
    os.link(path, other)
    with pytest.raises(MinuteNormalizationError, match="regular"):
        tuple(iter_source_minutes(root, manifest))
    other.unlink()
    monkeypatch.setattr(
        Path, "read_bytes", lambda _self: (_ for _ in ()).throw(OSError())
    )
    with pytest.raises(MinuteNormalizationError, match="cannot be read"):
        tuple(iter_source_minutes(root, manifest))


def test_source_reader_rejects_missing_root_outside_path_and_same_size_hash_change(
    tmp_path: Path,
) -> None:
    root = tmp_path / "data"
    payload = b"{}"
    manifest = _manifest_for_payload(
        root, payload, schema="synthetic-minute-bars-jsonl", record_count=1
    )
    with pytest.raises(MinuteNormalizationError, match="outside the data root"):
        minute_module._safe_source_path(tmp_path / "missing", manifest)

    outside = tmp_path / "outside.json"
    outside.write_bytes(payload)

    class _OutsideManifest:
        storage_path = "../outside.json"

    with pytest.raises(MinuteNormalizationError, match="outside the data root"):
        minute_module._safe_source_path(
            root, cast("SourceManifest", _OutsideManifest())
        )

    path = root / manifest.storage_path
    path.write_bytes(b"[]")
    with pytest.raises(MinuteNormalizationError) as captured:
        tuple(iter_source_minutes(root, manifest))
    assert captured.value.code is MinuteNormalizationCode.HASH_MISMATCH
