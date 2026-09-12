"""Point-in-time instrument, action, and calendar reference-data tests."""

from __future__ import annotations

import ast
import hashlib
import json
import sys
from dataclasses import FrozenInstanceError, replace
from datetime import date
from pathlib import Path
from typing import TYPE_CHECKING, cast

import pytest
from aegis_mx_intelligence import Identifier128, InstrumentId
from aegis_mx_research import reference_data
from aegis_mx_research.reference_data import (
    REFERENCE_REPORT_SCHEMA_VERSION,
    REFERENCE_SCHEMA_VERSION,
    BusinessInterval,
    CalendarDayKind,
    CalendarDayRevision,
    CorporateActionKind,
    CorporateActionRevision,
    ExactRatio,
    HaltRevision,
    InstrumentRevision,
    ListingVenue,
    MappingStatus,
    PointInTimeReferenceStore,
    ReferenceDataError,
    ReferenceErrorCode,
    ReferenceProvenance,
    ReferenceSnapshot,
    RoundingPolicy,
    SecurityType,
    SymbolMappingRevision,
    alpaca_instrument_id,
    benchmark_reference_store,
    build_reference_artifacts,
    build_snapshot_from_backfill_report,
    resolution_report_document,
    snapshot_document,
    verify_reference_artifact,
)

if TYPE_CHECKING:
    from collections.abc import Callable, Mapping

MAX_INT64 = (1 << 63) - 1
BASE_NS = 1_800_000_000_000_000_000
DAY_NS = 86_400_000_000_000


def _instrument(value: int) -> InstrumentId:
    return InstrumentId(Identifier128(0xA600, value))


def _provenance(
    available_at_ns: int = BASE_NS,
    *,
    document_id: str = "fixture-reference-v1",
) -> ReferenceProvenance:
    return ReferenceProvenance(
        provider="aegis-fixture",
        dataset="synthetic-reference",
        document_id=document_id,
        content_sha256=hashlib.sha256(document_id.encode()).hexdigest(),
        observed_at_ns=available_at_ns,
        processed_at_ns=available_at_ns,
        revision_at_ns=available_at_ns,
        source_publication_time_ns=available_at_ns - 1,
        historical_completeness=True,
        limitation="synthetic fixture; not provider data",
    )


def _mapping(  # noqa: PLR0913
    mapping_id: str,
    symbol: str,
    instrument_id: InstrumentId | None,
    *,
    start: int = BASE_NS,
    end: int | None = None,
    known: int = BASE_NS,
    status: MappingStatus = MappingStatus.RESOLVED_CURRENT_ONLY,
    venue: ListingVenue = ListingVenue.NASDAQ,
    version: int = 1,
) -> SymbolMappingRevision:
    return SymbolMappingRevision(
        mapping_id=mapping_id,
        symbol=symbol,
        instrument_id=instrument_id,
        listing_venue=venue,
        status=status,
        effective=BusinessInterval(start, end),
        version=version,
        provenance=_provenance(known, document_id=f"{mapping_id}-v{version}"),
    )


def _calendar_day(
    session_date: date = date(2026, 9, 11),
    *,
    kind: CalendarDayKind = CalendarDayKind.REGULAR_SESSION,
    known: int = BASE_NS,
    calendar_id: str = "XNYS-FIXTURE",
) -> CalendarDayRevision:
    session = kind in {
        CalendarDayKind.REGULAR_SESSION,
        CalendarDayKind.EARLY_CLOSE_SESSION,
    }
    return CalendarDayRevision(
        calendar_id=calendar_id,
        session_date=session_date,
        kind=kind,
        open_exchange_time_ns=BASE_NS if session else None,
        close_exchange_time_ns=BASE_NS
        + (6 if kind is CalendarDayKind.EARLY_CLOSE_SESSION else 7) * 3_600_000_000_000
        if session
        else None,
        version=1,
        provenance=_provenance(
            known, document_id=f"calendar-{calendar_id}-{session_date}"
        ),
    )


def _snapshot(
    *,
    mappings: tuple[SymbolMappingRevision, ...] | None = None,
    instruments: tuple[InstrumentRevision, ...] = (),
    actions: tuple[CorporateActionRevision, ...] = (),
    calendar: tuple[CalendarDayRevision, ...] | None = None,
) -> ReferenceSnapshot:
    return ReferenceSnapshot(
        universe_source_sha256=hashlib.sha256(b"universe").hexdigest(),
        source_report_sha256=hashlib.sha256(b"source").hexdigest(),
        as_of_ns=BASE_NS,
        instruments=instruments,
        mappings=mappings or (_mapping("AAA-current", "AAA", _instrument(1)),),
        corporate_actions=actions,
        calendar_days=calendar or (_calendar_day(),),
        halts=(),
        halt_coverage_complete=False,
    )


def _action(  # noqa: PLR0913
    action_id: str,
    instrument_id: InstrumentId,
    *,
    effective: int,
    known: int,
    ratio: ExactRatio,
    version: int = 1,
) -> CorporateActionRevision:
    return CorporateActionRevision(
        action_id=action_id,
        instrument_id=instrument_id,
        kind=CorporateActionKind.SPLIT,
        effective_time_ns=effective,
        version=version,
        provenance=_provenance(known, document_id=f"{action_id}-v{version}"),
        split_new_shares_per_old=ratio,
    )


def test_ticker_reuse_is_resolved_by_effective_time_not_current_symbol() -> None:
    old = _mapping(
        "reuse-old",
        "REUSE",
        _instrument(10),
        start=BASE_NS,
        end=BASE_NS + 100,
    )
    new = _mapping(
        "reuse-new", "REUSE", _instrument(11), start=BASE_NS + 100, known=BASE_NS + 50
    )
    store = PointInTimeReferenceStore(_snapshot(mappings=(old, new)))

    assert (
        store.symbol_mapping_at(
            "REUSE", effective_at_ns=BASE_NS + 99, known_at_ns=BASE_NS + 200
        )
        == old
    )
    assert (
        store.symbol_mapping_at(
            "REUSE", effective_at_ns=BASE_NS + 100, known_at_ns=BASE_NS + 200
        )
        == new
    )


def test_symbol_change_does_not_implicitly_substitute_new_symbol() -> None:
    instrument = _instrument(20)
    old = _mapping("change-old", "OLD", instrument, start=BASE_NS, end=BASE_NS + 100)
    new = _mapping("change-new", "NEW", instrument, start=BASE_NS + 100)
    store = PointInTimeReferenceStore(_snapshot(mappings=(old, new)))

    assert (
        store.symbol_mapping_at(
            "OLD", effective_at_ns=BASE_NS + 101, known_at_ns=BASE_NS + 200
        )
        is None
    )
    assert (
        store.symbol_mapping_at(
            "NEW", effective_at_ns=BASE_NS + 101, known_at_ns=BASE_NS + 200
        )
        == new
    )


def test_split_and_reverse_split_use_exact_checked_rationals() -> None:
    instrument = _instrument(30)
    split = _action(
        "split-2-for-1",
        instrument,
        effective=BASE_NS + 100,
        known=BASE_NS + 50,
        ratio=ExactRatio(2, 1),
    )
    reverse = _action(
        "reverse-1-for-10",
        instrument,
        effective=BASE_NS + 200,
        known=BASE_NS + 150,
        ratio=ExactRatio(1, 10),
    )
    store = PointInTimeReferenceStore(_snapshot(actions=(split, reverse)))

    assert (
        store.adjust_price_for_splits(
            1_000,
            instrument,
            after_ns=BASE_NS,
            through_ns=BASE_NS + 100,
            known_at_ns=BASE_NS + 200,
        )
        == 500
    )
    assert (
        store.adjust_price_for_splits(
            1_000,
            instrument,
            after_ns=BASE_NS + 100,
            through_ns=BASE_NS + 200,
            known_at_ns=BASE_NS + 200,
        )
        == 10_000
    )


def test_future_known_action_is_neither_visible_nor_applied_early() -> None:
    instrument = _instrument(31)
    action = _action(
        "future-split",
        instrument,
        effective=BASE_NS + 200,
        known=BASE_NS + 100,
        ratio=ExactRatio(2, 1),
    )
    store = PointInTimeReferenceStore(_snapshot(actions=(action,)))

    assert (
        store.corporate_actions_at(
            instrument, effective_at_ns=BASE_NS + 300, known_at_ns=BASE_NS + 99
        )
        == ()
    )
    assert (
        store.corporate_actions_at(
            instrument, effective_at_ns=BASE_NS + 199, known_at_ns=BASE_NS + 300
        )
        == ()
    )
    assert (
        store.adjust_price_for_splits(
            1_000,
            instrument,
            after_ns=BASE_NS,
            through_ns=BASE_NS + 199,
            known_at_ns=BASE_NS + 300,
        )
        == 1_000
    )


def test_dividend_metadata_is_explicit_and_not_applied_as_a_split() -> None:
    instrument = _instrument(32)
    dividend = CorporateActionRevision(
        action_id="cash-dividend",
        instrument_id=instrument,
        kind=CorporateActionKind.CASH_DIVIDEND,
        effective_time_ns=BASE_NS + 100,
        version=1,
        provenance=_provenance(BASE_NS + 50),
        cash_dividend_currency_nanos=250_000_000,
        currency="USD",
    )
    store = PointInTimeReferenceStore(_snapshot(actions=(dividend,)))

    assert store.cash_dividends_at(
        instrument, effective_at_ns=BASE_NS + 100, known_at_ns=BASE_NS + 100
    ) == (dividend,)
    assert (
        store.adjust_price_for_splits(
            10_000_000_000,
            instrument,
            after_ns=BASE_NS,
            through_ns=BASE_NS + 100,
            known_at_ns=BASE_NS + 100,
        )
        == 10_000_000_000
    )


def test_listing_delisting_recent_ipo_and_adr_are_representable() -> None:
    instrument = InstrumentRevision(
        revision_id="adr-revision",
        instrument_id=_instrument(40),
        primary_symbol="ADR",
        listing_venue=ListingVenue.NYSE,
        security_type=SecurityType.ADR,
        listing_date=date(2026, 1, 2),
        delisting_date=date(2026, 8, 31),
        effective=BusinessInterval(BASE_NS, BASE_NS + DAY_NS),
        version=1,
        unresolved_fields=(),
        provenance=_provenance(),
    )
    recent = _mapping(
        "recent-ipo",
        "IPO",
        _instrument(41),
        start=BASE_NS + 100,
        status=MappingStatus.RECENTLY_LISTED,
    )
    delisted = _mapping(
        "delisted",
        "GONE",
        _instrument(42),
        end=BASE_NS + 100,
        status=MappingStatus.DELISTED,
    )
    snapshot = _snapshot(mappings=(recent, delisted), instruments=(instrument,))
    store = PointInTimeReferenceStore(snapshot)

    assert instrument.security_type is SecurityType.ADR
    assert (
        store.instrument_at(
            instrument.instrument_id,
            effective_at_ns=BASE_NS,
            known_at_ns=BASE_NS,
        )
        == instrument
    )
    assert (
        store.instrument_at(
            instrument.instrument_id,
            effective_at_ns=BASE_NS + DAY_NS,
            known_at_ns=BASE_NS + DAY_NS,
        )
        is None
    )
    assert (
        store.symbol_mapping_at(
            "IPO", effective_at_ns=BASE_NS + 99, known_at_ns=BASE_NS + 200
        )
        is None
    )
    assert (
        store.symbol_mapping_at(
            "GONE", effective_at_ns=BASE_NS + 100, known_at_ns=BASE_NS + 200
        )
        is None
    )


def test_otc_identity_is_stable_but_universe_marks_it_unsupported() -> None:
    otc = _mapping(
        "otc",
        "OTCCO",
        _instrument(50),
        status=MappingStatus.UNSUPPORTED_VENUE,
        venue=ListingVenue.OTC,
    )
    store = PointInTimeReferenceStore(_snapshot(mappings=(otc,)))
    resolver = store.as_universe_resolver(effective_at_ns=BASE_NS, known_at_ns=BASE_NS)
    result = resolver.resolve("OTCCO")

    assert otc.instrument_id == _instrument(50)
    assert result.code.name == "UNSUPPORTED"
    assert result.instrument_id is None


def test_weekend_holiday_early_close_and_regular_session_are_explicit() -> None:
    saturday = _calendar_day(date(2026, 9, 12), kind=CalendarDayKind.WEEKEND)
    holiday = _calendar_day(date(2026, 9, 14), kind=CalendarDayKind.HOLIDAY)
    early = _calendar_day(date(2026, 11, 27), kind=CalendarDayKind.EARLY_CLOSE_SESSION)
    regular = _calendar_day(date(2026, 11, 30))
    store = PointInTimeReferenceStore(
        _snapshot(calendar=(saturday, holiday, early, regular))
    )

    assert (
        store.calendar_day_at(
            saturday.session_date, known_at_ns=BASE_NS
        ).open_exchange_time_ns
        is None
    )
    assert (
        store.calendar_day_at(holiday.session_date, known_at_ns=BASE_NS).kind
        is CalendarDayKind.HOLIDAY
    )
    assert (
        store.calendar_day_at(early.session_date, known_at_ns=BASE_NS).kind
        is CalendarDayKind.EARLY_CLOSE_SESSION
    )
    assert (
        store.calendar_day_at(regular.session_date, known_at_ns=BASE_NS).kind
        is CalendarDayKind.REGULAR_SESSION
    )


def test_missing_calendar_fails_closed() -> None:
    store = PointInTimeReferenceStore(_snapshot())
    with pytest.raises(ReferenceDataError) as caught:
        store.calendar_day_at(date(2026, 9, 10), known_at_ns=BASE_NS)
    assert caught.value.code is ReferenceErrorCode.MISSING_CALENDAR


def test_conflicting_symbol_sources_are_rejected_at_lookup() -> None:
    first = _mapping("source-a", "DUAL", _instrument(60))
    second = _mapping("source-b", "DUAL", _instrument(61))
    store = PointInTimeReferenceStore(_snapshot(mappings=(first, second)))
    with pytest.raises(ReferenceDataError) as caught:
        store.symbol_mapping_at("DUAL", effective_at_ns=BASE_NS, known_at_ns=BASE_NS)
    assert caught.value.code is ReferenceErrorCode.CONFLICT


def test_conflicting_calendar_sources_are_rejected_at_lookup() -> None:
    first = _calendar_day(calendar_id="SOURCE-A")
    second = replace(
        _calendar_day(calendar_id="SOURCE-B"),
        kind=CalendarDayKind.EARLY_CLOSE_SESSION,
        close_exchange_time_ns=BASE_NS + 4 * 3_600_000_000_000,
    )
    store = PointInTimeReferenceStore(_snapshot(calendar=(first, second)))
    with pytest.raises(ReferenceDataError) as caught:
        store.calendar_day_at(first.session_date, known_at_ns=BASE_NS)
    assert caught.value.code is ReferenceErrorCode.CONFLICT


def test_revision_lineage_requires_contiguous_versions_and_increasing_time() -> None:
    first = _mapping("revision", "REV", _instrument(70), version=1)
    third = _mapping("revision", "REV", _instrument(70), version=3, known=BASE_NS + 100)
    with pytest.raises(ReferenceDataError) as caught:
        PointInTimeReferenceStore(_snapshot(mappings=(first, third)))
    assert caught.value.code is ReferenceErrorCode.CONFLICT


@pytest.mark.parametrize(
    ("ratio", "value", "rounding", "code"),
    [
        (
            ExactRatio(1, 3),
            1,
            RoundingPolicy.REJECT_INEXACT,
            ReferenceErrorCode.INEXACT_ARITHMETIC,
        ),
        (
            ExactRatio(MAX_INT64, 1),
            2,
            RoundingPolicy.FLOOR,
            ReferenceErrorCode.INTEGER_OVERFLOW,
        ),
    ],
)
def test_rational_arithmetic_rejects_inexact_and_overflow(
    ratio: ExactRatio,
    value: int,
    rounding: RoundingPolicy,
    code: ReferenceErrorCode,
) -> None:
    with pytest.raises(ReferenceDataError) as caught:
        ratio.apply(value, rounding)
    assert caught.value.code is code


def test_half_even_and_floor_rounding_are_explicit() -> None:
    ratio = ExactRatio(1, 2)
    assert ratio.apply(5, RoundingPolicy.FLOOR) == 2
    assert ratio.apply(5, RoundingPolicy.HALF_EVEN) == 2
    assert ratio.apply(7, RoundingPolicy.HALF_EVEN) == 4


def test_records_are_immutable_and_snapshot_hash_is_reproducible() -> None:
    snapshot = _snapshot()
    with pytest.raises(FrozenInstanceError):
        snapshot.as_of_ns = BASE_NS + 1  # type: ignore[misc]
    first = snapshot_document(snapshot)
    second = snapshot_document(snapshot)
    assert first == second
    assert first["document_sha256"] == second["document_sha256"]


def _hashed_document(payload: Mapping[str, object]) -> dict[str, object]:
    body = dict(payload)
    encoded = json.dumps(
        body, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode("ascii")
    return {**body, "document_sha256": hashlib.sha256(encoded).hexdigest()}


def _write_backfill_fixture(path: Path) -> tuple[str, ...]:
    symbols = tuple(
        line
        for line in Path("ticker.txt").read_text(encoding="ascii").splitlines()
        if line
    )
    assets = {
        symbol: {
            "asset_class": "us_equity",
            "asset_id": f"fixture-{index:04d}",
            "exchange": "OTC" if symbol == "KRKNF" else "NASDAQ",
            "reason": "UNSUPPORTED_ASSET" if symbol == "KRKNF" else "SUPPORTED",
            "status": "UNSUPPORTED" if symbol == "KRKNF" else "RESOLVED",
            "symbol": symbol,
        }
        for index, symbol in enumerate(symbols)
    }
    payload: dict[str, object] = {
        "assets": assets,
        "calendar_source_response_sha256": hashlib.sha256(b"calendar").hexdigest(),
        "end_date": "2026-09-14",
        "finished_at_utc": "2026-09-15T01:02:03Z",
        "mode": "BACKFILL",
        "schema_version": "1.0.0",
        "sessions": [
            {
                "close_utc": "2026-09-11T20:00:00Z",
                "date": "2026-09-11",
                "open_utc": "2026-09-11T13:30:00Z",
            },
            {
                "close_utc": "2026-09-14T17:00:00Z",
                "date": "2026-09-14",
                "open_utc": "2026-09-14T13:30:00Z",
            },
        ],
        "start_date": "2026-09-11",
        "status": "COMPLETED",
        "ticker_source_sha256": hashlib.sha256(
            Path("ticker.txt").read_bytes()
        ).hexdigest(),
    }
    path.write_text(
        json.dumps(_hashed_document(payload), sort_keys=True, separators=(",", ":"))
        + "\n",
        encoding="ascii",
    )
    return symbols


def test_offline_report_builder_covers_every_ticker_without_backdating(
    tmp_path: Path,
) -> None:
    source = tmp_path / "backfill.json"
    symbols = _write_backfill_fixture(source)
    snapshot = build_snapshot_from_backfill_report(source)

    assert len(snapshot.mappings) == len(symbols) == 79
    assert {item.symbol for item in snapshot.mappings} == set(symbols)
    assert len(snapshot.instruments) == 79
    assert snapshot.corporate_actions == ()
    assert snapshot.halts == ()
    assert not snapshot.halt_coverage_complete
    assert all(
        item.effective.effective_from_ns == snapshot.as_of_ns
        for item in snapshot.mappings
    )
    assert (
        sum(
            item.status is MappingStatus.UNSUPPORTED_VENUE for item in snapshot.mappings
        )
        == 1
    )
    assert any(item.kind is CalendarDayKind.WEEKEND for item in snapshot.calendar_days)
    assert any(
        item.kind is CalendarDayKind.EARLY_CLOSE_SESSION
        for item in snapshot.calendar_days
    )


def test_artifact_build_verify_tamper_and_report_contract(tmp_path: Path) -> None:
    source = tmp_path / "backfill.json"
    _write_backfill_fixture(source)
    output = tmp_path / "output"
    summary = build_reference_artifacts(source, output)

    snapshot_path = Path(cast("str", summary["reference_snapshot"]))
    report_path = Path(cast("str", summary["instrument_resolution_report"]))
    assert (
        verify_reference_artifact(
            snapshot_path, expected_schema=REFERENCE_SCHEMA_VERSION
        )
        == summary["reference_snapshot_file_sha256"]
    )
    assert (
        verify_reference_artifact(
            report_path, expected_schema=REFERENCE_REPORT_SCHEMA_VERSION
        )
        == summary["instrument_resolution_report_file_sha256"]
    )
    report = json.loads(report_path.read_bytes())
    assert report["counts"]["requested"] == 79
    assert len(report["entries"]) == 79
    assert report["status"] == "PARTIAL_REFERENCE_COVERAGE"
    assert not report["safe_for_historical_training"]
    assert not report["live_trading_capable"]
    assert (output / "instrument-resolution-report.md").stat().st_mode & 0o777 == 0o600

    tampered = bytearray(snapshot_path.read_bytes())
    tampered[10] ^= 1
    snapshot_path.write_bytes(tampered)
    with pytest.raises(ReferenceDataError):
        verify_reference_artifact(
            snapshot_path, expected_schema=REFERENCE_SCHEMA_VERSION
        )


def test_report_is_deterministic_for_identical_snapshot() -> None:
    snapshot = _snapshot()
    snapshot_value = snapshot_document(snapshot)
    first = resolution_report_document(
        snapshot, cast("str", snapshot_value["document_sha256"])
    )
    second = resolution_report_document(
        snapshot, cast("str", snapshot_value["document_sha256"])
    )
    assert first == second


def test_alpaca_stable_identifier_matches_existing_minute_contract() -> None:
    asset_id = "b0b6dd9d-8b9b-48a9-ba46-b9d54906e415"
    expected = hashlib.sha256(f"ALPACA-ASSET-V1:{asset_id}".encode()).digest()[:16]
    assert alpaca_instrument_id(asset_id).hex() == expected.hex()
    with pytest.raises(ReferenceDataError):
        alpaca_instrument_id("non-ascii-é")


@pytest.mark.parametrize(
    "mutation",
    [b"", b"{}", b"[]", b"not-json", bytes(range(256)), b'{"schema_version":2}'],
)
def test_reference_artifact_deserialization_fuzz_smoke(
    mutation: bytes, tmp_path: Path
) -> None:
    path = tmp_path / "fuzz.json"
    path.write_bytes(mutation)
    with pytest.raises(ReferenceDataError):
        verify_reference_artifact(path, expected_schema=REFERENCE_SCHEMA_VERSION)


def test_no_network_primitive_or_trading_dependency_is_present() -> None:
    source = Path(reference_data.__file__).read_text(encoding="utf-8")
    imported: set[str] = set()
    for node in ast.walk(ast.parse(source)):
        if isinstance(node, ast.Import):
            imported.update(alias.name for alias in node.names)
        elif isinstance(node, ast.ImportFrom) and node.module:
            imported.add(node.module)
    assert not any(
        name == forbidden or name.startswith(f"{forbidden}.")
        for name in imported
        for forbidden in ("urllib", "requests", "socket", "aegis.gateway", "aegis.oms")
    )


@pytest.mark.parametrize("value", ["", "bad\x00text", "x" * 513])
def test_provenance_rejects_invalid_bounded_text(value: str) -> None:
    with pytest.raises(ReferenceDataError):
        replace(_provenance(), limitation=value)


@pytest.mark.parametrize(
    "changes",
    [
        {"content_sha256": "bad"},
        {"observed_at_ns": 0},
        {"processed_at_ns": BASE_NS - 2},
        {"source_publication_time_ns": BASE_NS + 1},
        {"revision_at_ns": BASE_NS - 2, "source_publication_time_ns": BASE_NS - 1},
    ],
)
def test_provenance_rejects_invalid_digest_or_temporal_order(
    changes: Mapping[str, object],
) -> None:
    with pytest.raises(ReferenceDataError):
        replace(_provenance(), **changes)  # type: ignore[arg-type]


@pytest.mark.parametrize(
    "factory",
    [
        lambda: ExactRatio(0, 1),
        lambda: ExactRatio(2, 2),
        lambda: ExactRatio.reduced(0, 1),
        lambda: ExactRatio(1, 1).apply(-1, RoundingPolicy.FLOOR),
        lambda: BusinessInterval(0),
        lambda: BusinessInterval(BASE_NS, BASE_NS),
    ],
)
def test_ratio_and_interval_invalid_states_fail_closed(
    factory: Callable[[], object],
) -> None:
    with pytest.raises(ReferenceDataError):
        factory()


def test_ratio_reduction_cross_cancel_and_interval_boundaries() -> None:
    assert ExactRatio.reduced(6, 8) == ExactRatio(3, 4)
    assert ExactRatio(2, 3).multiply(ExactRatio(3, 4)) == ExactRatio(1, 2)
    interval = BusinessInterval(BASE_NS, BASE_NS + 10)
    assert interval.contains(BASE_NS)
    assert not interval.contains(BASE_NS - 1)
    assert not interval.contains(BASE_NS + 10)


@pytest.mark.parametrize(
    "changes",
    [
        {"primary_symbol": "bad"},
        {"version": 0},
        {"listing_date": date(2026, 2, 1), "delisting_date": date(2026, 1, 1)},
        {"unresolved_fields": ("listing_date", "listing_date")},
    ],
)
def test_instrument_revision_rejects_invalid_states(
    changes: Mapping[str, object],
) -> None:
    valid = InstrumentRevision(
        revision_id="instrument",
        instrument_id=_instrument(80),
        primary_symbol="VALID",
        listing_venue=ListingVenue.NYSE,
        security_type=SecurityType.COMMON_STOCK,
        listing_date=None,
        delisting_date=None,
        effective=BusinessInterval(BASE_NS),
        version=1,
        unresolved_fields=(),
        provenance=_provenance(),
    )
    with pytest.raises(ReferenceDataError):
        replace(valid, **changes)  # type: ignore[arg-type]


@pytest.mark.parametrize(
    "changes",
    [
        {"symbol": "bad"},
        {"version": 0},
        {"instrument_id": None},
        {"status": MappingStatus.AMBIGUOUS},
    ],
)
def test_symbol_mapping_rejects_invalid_states(changes: Mapping[str, object]) -> None:
    valid = _mapping("valid", "VALID", _instrument(81))
    with pytest.raises(ReferenceDataError):
        replace(valid, **changes)  # type: ignore[arg-type]


def test_ambiguous_and_missing_mappings_require_no_instrument_id() -> None:
    for status in (MappingStatus.AMBIGUOUS, MappingStatus.MISSING_REFERENCE):
        assert (
            _mapping(
                f"no-id-{status.value}",
                "NONE",
                None,
                status=status,
                venue=ListingVenue.UNKNOWN,
            ).instrument_id
            is None
        )


@pytest.mark.parametrize(
    "action",
    [
        CorporateActionRevision(
            "valid-split",
            _instrument(82),
            CorporateActionKind.SPLIT,
            BASE_NS,
            1,
            _provenance(),
            ExactRatio(2, 1),
        ),
        CorporateActionRevision(
            "valid-dividend",
            _instrument(82),
            CorporateActionKind.CASH_DIVIDEND,
            BASE_NS,
            1,
            _provenance(),
            cash_dividend_currency_nanos=1,
            currency="USD",
        ),
    ],
)
def test_action_serialization_covers_splits_and_dividends(
    action: CorporateActionRevision,
) -> None:
    value = snapshot_document(_snapshot(actions=(action,)))
    assert (
        cast("list[dict[str, object]]", value["corporate_actions"])[0]["kind"]
        == action.kind.value
    )


@pytest.mark.parametrize(
    "changes",
    [
        {"version": 0},
        {"split_new_shares_per_old": None},
        {"cash_dividend_currency_nanos": 1},
    ],
)
def test_split_action_rejects_invalid_payload(changes: Mapping[str, object]) -> None:
    valid = _action(
        "split-valid",
        _instrument(83),
        effective=BASE_NS,
        known=BASE_NS,
        ratio=ExactRatio(2, 1),
    )
    with pytest.raises(ReferenceDataError):
        replace(valid, **changes)  # type: ignore[arg-type]


@pytest.mark.parametrize(
    "changes",
    [
        {"cash_dividend_currency_nanos": None},
        {"cash_dividend_currency_nanos": -1},
        {"currency": None},
        {"split_new_shares_per_old": ExactRatio(2, 1)},
    ],
)
def test_dividend_action_rejects_invalid_payload(
    changes: Mapping[str, object],
) -> None:
    valid = CorporateActionRevision(
        action_id="dividend-valid",
        instrument_id=_instrument(84),
        kind=CorporateActionKind.CASH_DIVIDEND,
        effective_time_ns=BASE_NS,
        version=1,
        provenance=_provenance(),
        cash_dividend_currency_nanos=1,
        currency="USD",
    )
    with pytest.raises(ReferenceDataError):
        replace(valid, **changes)  # type: ignore[arg-type]


@pytest.mark.parametrize(
    "changes",
    [
        {"version": 0},
        {"open_exchange_time_ns": None},
        {"close_exchange_time_ns": BASE_NS},
    ],
)
def test_session_calendar_rejects_invalid_payload(
    changes: Mapping[str, object],
) -> None:
    with pytest.raises(ReferenceDataError):
        replace(_calendar_day(), **changes)  # type: ignore[arg-type]


def test_closed_calendar_day_rejects_session_timestamps() -> None:
    with pytest.raises(ReferenceDataError):
        replace(_calendar_day(), kind=CalendarDayKind.HOLIDAY)


def test_halt_validation_identity_and_serialization() -> None:
    halt = HaltRevision(
        halt_id="halt-1",
        instrument_id=_instrument(85),
        start_exchange_time_ns=BASE_NS,
        end_exchange_time_ns=BASE_NS + 1,
        version=1,
        provenance=_provenance(),
    )
    halt_snapshot = replace(_snapshot(), halts=(halt,))
    store = PointInTimeReferenceStore(halt_snapshot)
    assert store.halts_at(
        halt.instrument_id,
        start_exchange_time_ns=BASE_NS,
        end_exchange_time_ns=BASE_NS + 1,
        known_at_ns=BASE_NS,
    ) == (halt,)
    assert (
        store.halts_at(
            halt.instrument_id,
            start_exchange_time_ns=BASE_NS + 1,
            end_exchange_time_ns=BASE_NS + 2,
            known_at_ns=BASE_NS,
        )
        == ()
    )
    with pytest.raises(ReferenceDataError):
        store.halts_at(
            halt.instrument_id,
            start_exchange_time_ns=BASE_NS,
            end_exchange_time_ns=BASE_NS,
            known_at_ns=BASE_NS,
        )
    value = snapshot_document(halt_snapshot)
    assert cast("list[dict[str, object]]", value["halts"])[0]["halt_id"] == "halt-1"
    for changes in ({"halt_id": ""}, {"end_exchange_time_ns": BASE_NS}, {"version": 0}):
        with pytest.raises(ReferenceDataError):
            replace(halt, **changes)


def test_conflicting_instrument_revisions_fail_closed() -> None:
    instrument_id = _instrument(851)

    def instrument(revision_id: str, symbol: str) -> InstrumentRevision:
        return InstrumentRevision(
            revision_id=revision_id,
            instrument_id=instrument_id,
            primary_symbol=symbol,
            listing_venue=ListingVenue.NASDAQ,
            security_type=SecurityType.COMMON_STOCK,
            listing_date=None,
            delisting_date=None,
            effective=BusinessInterval(BASE_NS),
            version=1,
            unresolved_fields=(),
            provenance=_provenance(document_id=revision_id),
        )

    store = PointInTimeReferenceStore(
        _snapshot(
            instruments=(instrument("first", "FIRST"), instrument("second", "SECOND"))
        )
    )
    with pytest.raises(ReferenceDataError) as caught:
        store.instrument_at(instrument_id, effective_at_ns=BASE_NS, known_at_ns=BASE_NS)
    assert caught.value.code is ReferenceErrorCode.CONFLICT


@pytest.mark.parametrize(
    "changes",
    [
        {"schema_version": "2.0.0"},
        {"source_report_sha256": "bad"},
        {"as_of_ns": 0},
        {"mappings": ()},
        {"calendar_days": ()},
    ],
)
def test_reference_snapshot_rejects_invalid_envelope(
    changes: Mapping[str, object],
) -> None:
    with pytest.raises(ReferenceDataError):
        replace(_snapshot(), **changes)  # type: ignore[arg-type]


def test_revision_lineage_rejects_nonincreasing_revision_time() -> None:
    first = _mapping("same", "SAME", _instrument(86), version=1)
    second = _mapping("same", "SAME", _instrument(86), version=2)
    with pytest.raises(ReferenceDataError) as caught:
        PointInTimeReferenceStore(_snapshot(mappings=(first, second)))
    assert caught.value.code is ReferenceErrorCode.CONFLICT


@pytest.mark.parametrize(
    ("status", "expected"),
    [
        (MappingStatus.RESOLVED_CURRENT_ONLY, "RESOLVED"),
        (MappingStatus.RECENTLY_LISTED, "RECENTLY_LISTED"),
        (MappingStatus.DELISTED, "DELISTED"),
        (MappingStatus.UNSUPPORTED_VENUE, "UNSUPPORTED"),
        (MappingStatus.AMBIGUOUS, "AMBIGUOUS_REFERENCE"),
        (MappingStatus.MISSING_REFERENCE, "MISSING_REFERENCE_DATA"),
    ],
)
def test_all_mapping_states_convert_to_fail_closed_universe_results(
    status: MappingStatus, expected: str
) -> None:
    needs_id = status not in {MappingStatus.AMBIGUOUS, MappingStatus.MISSING_REFERENCE}
    mapping = _mapping(
        f"status-{status.value}",
        "STATE",
        _instrument(87) if needs_id else None,
        status=status,
        venue=(
            ListingVenue.OTC
            if status is MappingStatus.UNSUPPORTED_VENUE
            else ListingVenue.NASDAQ
        ),
    )
    store = PointInTimeReferenceStore(_snapshot(mappings=(mapping,)))
    result = store.as_universe_resolver(
        effective_at_ns=BASE_NS, known_at_ns=BASE_NS
    ).resolve("STATE")
    assert result.code.name == expected
    assert (result.instrument_id is not None) == (expected == "RESOLVED")


def test_missing_mapping_converts_to_missing_reference() -> None:
    store = PointInTimeReferenceStore(_snapshot())
    result = store.as_universe_resolver(
        effective_at_ns=BASE_NS, known_at_ns=BASE_NS
    ).resolve("ABSENT")
    assert result.code.name == "MISSING_REFERENCE_DATA"


def test_reversed_adjustment_interval_and_invalid_query_time_reject() -> None:
    store = PointInTimeReferenceStore(_snapshot())
    with pytest.raises(ReferenceDataError):
        store.adjust_price_for_splits(
            1,
            _instrument(1),
            after_ns=BASE_NS + 1,
            through_ns=BASE_NS,
            known_at_ns=BASE_NS,
        )
    with pytest.raises(ReferenceDataError):
        store.symbol_mapping_at("AAA", effective_at_ns=BASE_NS, known_at_ns=0)


@pytest.mark.parametrize("value", ["2026-09-11", "badZ", "1969-01-01T00:00:00Z"])
def test_utc_timestamp_parser_rejects_ambiguous_malformed_or_nonpositive(
    value: str,
) -> None:
    with pytest.raises(ReferenceDataError):
        reference_data._utc_ns(value)  # noqa: SLF001


def _mutate_hashed_fixture(
    path: Path, mutation: Callable[[dict[str, object]], None]
) -> None:
    value = cast("dict[str, object]", json.loads(path.read_bytes()))
    value.pop("document_sha256")
    mutation(value)
    path.write_text(
        json.dumps(_hashed_document(value), sort_keys=True, separators=(",", ":"))
        + "\n",
        encoding="ascii",
    )


def test_backfill_builder_rejects_unreadable_empty_malformed_and_oversized(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    with pytest.raises(ReferenceDataError):
        build_snapshot_from_backfill_report(tmp_path / "absent")
    empty = tmp_path / "empty"
    empty.write_bytes(b"")
    with pytest.raises(ReferenceDataError):
        build_snapshot_from_backfill_report(empty)
    empty.write_bytes(b"not-json")
    with pytest.raises(ReferenceDataError):
        build_snapshot_from_backfill_report(empty)
    _write_backfill_fixture(empty)
    monkeypatch.setattr(reference_data, "MAX_INPUT_BYTES", 1)
    with pytest.raises(ReferenceDataError):
        build_snapshot_from_backfill_report(empty)


@pytest.mark.parametrize(
    ("mutation", "retained_status"),
    [
        (lambda value: value.update(status="FAILED"), None),
        (lambda value: value.update(ticker_source_sha256="0" * 64), None),
        (lambda value: value.update(assets={}), None),
        (lambda value: value.update(sessions=[]), None),
        (lambda value: value["assets"].update(NVDA="bad"), None),
        (
            lambda value: value["assets"]["NVDA"].update(asset_id=None),
            MappingStatus.MISSING_REFERENCE,
        ),
        (lambda value: value.update(sessions=["bad"]), None),
        (lambda value: value["sessions"][0].update(open_utc="bad"), None),
        (lambda value: value["sessions"].append(value["sessions"][0]), None),
        (lambda value: value.update(start_date="2026-09-10"), None),
    ],
)
def test_backfill_builder_rejects_or_explicitly_retains_incomplete_inputs(
    mutation: Callable[[dict[str, object]], None],
    retained_status: MappingStatus | None,
    tmp_path: Path,
) -> None:
    path = tmp_path / "backfill.json"
    _write_backfill_fixture(path)
    _mutate_hashed_fixture(path, mutation)
    if retained_status is not None:
        snapshot = build_snapshot_from_backfill_report(path)
        mapping = next(item for item in snapshot.mappings if item.symbol == "NVDA")
        assert mapping.status is retained_status
        assert mapping.instrument_id is None
    else:
        with pytest.raises(ReferenceDataError):
            build_snapshot_from_backfill_report(path)


def test_backfill_builder_preserves_unknown_venue_and_security_type(
    tmp_path: Path,
) -> None:
    path = tmp_path / "backfill.json"
    _write_backfill_fixture(path)

    def mutate_asset(value: dict[str, object]) -> None:
        assets = cast("dict[str, dict[str, object]]", value["assets"])
        assets["NVDA"].update(exchange="UNMAPPED", asset_class="unknown")

    _mutate_hashed_fixture(path, mutate_asset)
    snapshot = build_snapshot_from_backfill_report(path)
    instrument = next(
        item for item in snapshot.instruments if item.primary_symbol == "NVDA"
    )
    assert instrument.listing_venue is ListingVenue.UNKNOWN
    assert instrument.security_type is SecurityType.UNKNOWN


def test_verify_rejects_absent_empty_noncanonical_and_wrong_schema(
    tmp_path: Path,
) -> None:
    absent = tmp_path / "absent"
    with pytest.raises(ReferenceDataError):
        verify_reference_artifact(absent, expected_schema=REFERENCE_SCHEMA_VERSION)
    empty = tmp_path / "empty"
    empty.write_bytes(b"")
    with pytest.raises(ReferenceDataError):
        verify_reference_artifact(empty, expected_schema=REFERENCE_SCHEMA_VERSION)
    valid = _document_for_test({"schema_version": "1.0.0"})
    empty.write_text(json.dumps(valid, indent=2), encoding="ascii")
    with pytest.raises(ReferenceDataError):
        verify_reference_artifact(empty, expected_schema=REFERENCE_SCHEMA_VERSION)
    canonical = json.dumps(valid, sort_keys=True, separators=(",", ":")) + "\n"
    empty.write_text(canonical, encoding="ascii")
    with pytest.raises(ReferenceDataError) as caught:
        verify_reference_artifact(empty, expected_schema="2.0.0")
    assert caught.value.code is ReferenceErrorCode.UNSUPPORTED_SCHEMA


def _document_for_test(payload: Mapping[str, object]) -> dict[str, object]:
    return _hashed_document(payload)


def test_atomic_write_cleans_temporary_file_on_failure(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    output = tmp_path / "output.json"

    def fail_fsync(_descriptor: int) -> None:
        message = "synthetic fsync failure"
        raise OSError(message)

    monkeypatch.setattr("aegis_mx_research.reference_data.os.fsync", fail_fsync)
    with pytest.raises(OSError, match="synthetic fsync"):
        reference_data._atomic_write(output, b"payload")  # noqa: SLF001
    assert not output.exists()
    assert list(tmp_path.iterdir()) == []


def test_atomic_publication_is_idempotent_and_rejects_replacement(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    output = tmp_path / "immutable.json"
    reference_data._atomic_write(output, b"same")  # noqa: SLF001
    reference_data._atomic_write(output, b"same")  # noqa: SLF001
    with pytest.raises(ReferenceDataError) as caught:
        reference_data._atomic_write(output, b"different")  # noqa: SLF001
    assert caught.value.code is ReferenceErrorCode.CONFLICT
    assert output.read_bytes() == b"same"

    def fail_read() -> bytes:
        message = "synthetic read failure"
        raise OSError(message)

    monkeypatch.setattr(Path, "read_bytes", lambda _path: fail_read())
    with pytest.raises(ReferenceDataError) as unreadable:
        reference_data._atomic_write(output, b"same")  # noqa: SLF001
    assert unreadable.value.code is ReferenceErrorCode.INTEGRITY_FAILURE


def test_cli_build_verify_and_console_entry(
    tmp_path: Path, capsys: pytest.CaptureFixture[str], monkeypatch: pytest.MonkeyPatch
) -> None:
    source = tmp_path / "backfill.json"
    _write_backfill_fixture(source)
    output = tmp_path / "out"
    assert (
        reference_data.main(
            ["build", "--source-report", str(source), "--output-directory", str(output)]
        )
        == 0
    )
    build_result = json.loads(capsys.readouterr().out)
    assert build_result["requested_symbols"] == 79
    snapshot = output / "reference-snapshot-v1.json"
    assert reference_data.main(["verify", str(snapshot)]) == 0
    assert json.loads(capsys.readouterr().out)["status"] == "VERIFIED"

    monkeypatch.setattr(sys, "argv", ["aegis-reference", "verify", str(snapshot)])
    assert reference_data.cli_main() == 0
    assert json.loads(capsys.readouterr().out)["status"] == "VERIFIED"

    benchmark_path = output / "benchmark.json"
    assert (
        reference_data.main(
            [
                "benchmark",
                "--source-report",
                str(source),
                "--iterations",
                "2",
                "--output",
                str(benchmark_path),
            ]
        )
        == 0
    )
    benchmark = json.loads(capsys.readouterr().out)
    assert benchmark["benchmark_kind"] == "POINT_IN_TIME_REFERENCE"
    assert benchmark_path.exists()


def test_benchmark_is_bounded_and_reports_all_categories() -> None:
    result = benchmark_reference_store(_snapshot(), iterations=2)
    assert result["iterations"] == 2
    for category in (
        "calendar_lookup",
        "instrument_adjustment",
        "mapping_lookup",
        "universe_resolution",
    ):
        metrics = cast("dict[str, object]", result[category])
        assert metrics["operations"] == 2
        assert len(cast("list[int]", metrics["raw_batch_samples_ns"])) == 2
    with pytest.raises(ReferenceDataError):
        benchmark_reference_store(_snapshot(), iterations=0)
    with pytest.raises(ReferenceDataError):
        reference_data._latency_summary([], 1)  # noqa: SLF001
    with pytest.raises(ReferenceDataError):
        reference_data._latency_summary([1], 0)  # noqa: SLF001


def test_closed_json_schemas_match_generated_reference_documents(
    tmp_path: Path,
) -> None:
    source = tmp_path / "backfill.json"
    _write_backfill_fixture(source)
    output = tmp_path / "artifacts"
    build_reference_artifacts(source, output)
    pairs = (
        (
            Path("schemas/instrument-reference-snapshot-v1.schema.json"),
            output / "reference-snapshot-v1.json",
        ),
        (
            Path("schemas/instrument-resolution-report-v1.schema.json"),
            output / "instrument-resolution-report-v1.json",
        ),
    )
    for schema_path, document_path in pairs:
        schema = json.loads(schema_path.read_bytes())
        document = json.loads(document_path.read_bytes())
        assert schema["additionalProperties"] is False
        assert set(schema["required"]) == set(schema["properties"])
        assert set(document) == set(schema["required"])

    snapshot_schema = json.loads(pairs[0][0].read_bytes())
    snapshot = json.loads(pairs[0][1].read_bytes())
    collection_defs = {
        "calendar_days": "calendarDay",
        "corporate_actions": "action",
        "halts": "halt",
        "instruments": "instrument",
        "mappings": "mapping",
    }
    for collection, definition in collection_defs.items():
        expected = set(snapshot_schema["$defs"][definition]["required"])
        assert snapshot_schema["$defs"][definition]["additionalProperties"] is False
        assert all(set(record) == expected for record in snapshot[collection])
