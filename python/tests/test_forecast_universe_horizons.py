"""Deterministic contracts for the bounded forecasting POC universe and horizons."""

from __future__ import annotations

import hashlib
import json
from dataclasses import replace
from typing import TYPE_CHECKING, cast

import pytest
from aegis_mx_intelligence import ConfigurationVersion, Identifier128, InstrumentId
from aegis_mx_research import (
    REQUIRED_HORIZON_LABELS,
    ExchangeCalendar,
    ForecastHorizonUnit,
    HaltInterval,
    HorizonContractError,
    HorizonErrorCode,
    HorizonHaltPolicy,
    HorizonSessionEndpoint,
    HorizonSpec,
    HorizonTarget,
    InstrumentResolution,
    InstrumentResolutionCode,
    StaticInstrumentResolver,
    TradingSession,
    UniverseContractError,
    UniverseEntry,
    UniverseErrorCode,
    UnresolvedInstrumentResolver,
    decode_universe_snapshot,
    encode_universe_snapshot,
    forecast_contracts,
    load_ticker_universe,
    parse_ticker_universe,
    required_horizon_specs,
    resolve_horizon,
)

if TYPE_CHECKING:
    from collections.abc import Mapping
    from pathlib import Path

MINUTE_NS = 60_000_000_000
DAY_NS = 86_400_000_000_000
BASE_NS = 1_800_000_000_000_000_000
MAX_INT64 = (1 << 63) - 1


def _identifier(value: int) -> Identifier128:
    return Identifier128(0xA600, value)


def _instrument(value: int) -> InstrumentId:
    return InstrumentId(_identifier(value))


def _configuration(value: int = 1) -> ConfigurationVersion:
    return ConfigurationVersion(Identifier128(0xCA1E, value))


def _resolver(
    symbols: tuple[str, ...],
    unresolved: Mapping[str, InstrumentResolutionCode] | None = None,
) -> StaticInstrumentResolver:
    return StaticInstrumentResolver(
        {symbol: _instrument(index + 1) for index, symbol in enumerate(symbols)},
        {} if unresolved is None else unresolved,
    )


def _session(
    ordinal: int,
    *,
    day_offset: int | None = None,
    minutes: int = 390,
    halts: tuple[HaltInterval, ...] = (),
) -> TradingSession:
    day = ordinal if day_offset is None else day_offset
    open_ns = BASE_NS + (day * DAY_NS)
    return TradingSession(
        session_id=f"XNYS-TEST-{ordinal:02d}",
        open_exchange_event_time_ns=open_ns,
        close_exchange_event_time_ns=open_ns + (minutes * MINUTE_NS),
        halt_intervals=halts,
    )


def _calendar(*sessions: TradingSession) -> ExchangeCalendar:
    return ExchangeCalendar(
        calendar_id="XNYS-TEST",
        calendar_version=_configuration(),
        sessions=tuple(sessions),
    )


def test_universe_preserves_source_order_hashes_sorted_canonical_and_roundtrip() -> (
    None
):
    source = b"MSFT\n\nAAPL"
    snapshot = parse_ticker_universe(source, _resolver(("MSFT", "AAPL")))

    assert tuple(entry.symbol for entry in snapshot.ordered_entries) == ("MSFT", "AAPL")
    assert snapshot.canonical_symbols == ("AAPL", "MSFT")
    assert snapshot.source_file_sha256 == hashlib.sha256(source).digest()
    assert all(
        entry.resolution_code is InstrumentResolutionCode.RESOLVED
        and entry.instrument_id is not None
        for entry in snapshot.ordered_entries
    )
    encoded = encode_universe_snapshot(snapshot)
    assert decode_universe_snapshot(encoded) == snapshot
    assert encode_universe_snapshot(decode_universe_snapshot(encoded)) == encoded

    reordered = parse_ticker_universe(b"AAPL\nMSFT\n", _resolver(("AAPL", "MSFT")))
    assert reordered.canonical_symbols == snapshot.canonical_symbols
    assert reordered.source_file_sha256 != snapshot.source_file_sha256
    assert reordered.universe_snapshot_sha256 != snapshot.universe_snapshot_sha256


def test_operational_loader_reads_fixed_ticker_file_and_retains_unresolved() -> None:
    snapshot = load_ticker_universe(UnresolvedInstrumentResolver())

    assert snapshot.ordered_entries
    assert snapshot.source_path == "/scratch/djy8hg/xtrade/ticker.txt"
    assert len(snapshot.canonical_symbols) == len(snapshot.ordered_entries)
    assert all(
        entry.resolution_code is InstrumentResolutionCode.MISSING_REFERENCE_DATA
        and entry.instrument_id is None
        for entry in snapshot.ordered_entries
    )


@pytest.mark.parametrize(
    ("source", "code"),
    [
        (b"", UniverseErrorCode.EMPTY_UNIVERSE),
        (b"aapl\n", UniverseErrorCode.MALFORMED_SYMBOL),
        (b"AAPL MSFT\n", UniverseErrorCode.AMBIGUOUS_SYMBOL),
        (b"NASDAQ:AAPL\n", UniverseErrorCode.AMBIGUOUS_SYMBOL),
        (b"A/B\n", UniverseErrorCode.AMBIGUOUS_SYMBOL),
        (b"1AAPL\n", UniverseErrorCode.MALFORMED_SYMBOL),
        (b"AAPL..B\n", UniverseErrorCode.MALFORMED_SYMBOL),
        (b"AAPL \n", UniverseErrorCode.AMBIGUOUS_SYMBOL),
        (b"ABCDEFGHIJKLMNOPQ\n", UniverseErrorCode.SYMBOL_TOO_LONG),
        (b"AAPL\nAAPL\n", UniverseErrorCode.DUPLICATE_SYMBOL),
        (b"AAPL\x00\n", UniverseErrorCode.MALFORMED_SYMBOL),
        (b"\xff\n", UniverseErrorCode.INVALID_ENCODING),
    ],
)
def test_universe_rejects_malformed_duplicate_overlength_and_ambiguous_input(
    source: bytes, code: UniverseErrorCode
) -> None:
    with pytest.raises(UniverseContractError) as caught:
        parse_ticker_universe(source, UnresolvedInstrumentResolver())
    assert caught.value.code is code


def test_universe_retains_explicit_unresolved_reasons() -> None:
    resolver = StaticInstrumentResolver(
        {"AAPL": _instrument(1)},
        {
            "NEWCO": InstrumentResolutionCode.RECENTLY_LISTED,
            "OTCCO": InstrumentResolutionCode.UNSUPPORTED,
            "OLDCO": InstrumentResolutionCode.DELISTED,
            "DUAL": InstrumentResolutionCode.AMBIGUOUS_REFERENCE,
        },
    )
    snapshot = parse_ticker_universe(
        b"AAPL\nNEWCO\nOTCCO\nOLDCO\nDUAL\nMISSING\n", resolver
    )

    assert tuple(entry.resolution_code for entry in snapshot.ordered_entries) == (
        InstrumentResolutionCode.RESOLVED,
        InstrumentResolutionCode.RECENTLY_LISTED,
        InstrumentResolutionCode.UNSUPPORTED,
        InstrumentResolutionCode.DELISTED,
        InstrumentResolutionCode.AMBIGUOUS_REFERENCE,
        InstrumentResolutionCode.MISSING_REFERENCE_DATA,
    )
    assert len(snapshot.ordered_entries) == 6


def test_universe_rejects_duplicate_resolved_instrument_identity() -> None:
    repeated = _instrument(7)
    resolver = StaticInstrumentResolver({"AAA": repeated, "BBB": repeated})
    with pytest.raises(UniverseContractError) as caught:
        parse_ticker_universe(b"AAA\nBBB\n", resolver)
    assert caught.value.code is UniverseErrorCode.AMBIGUOUS_INSTRUMENT


@pytest.mark.parametrize(
    "mutation",
    [
        b"",
        b"{}",
        b"[]",
        b'{"schema_version":1}',
        bytes(range(256)),
        b"\xff" * 32,
    ],
)
def test_universe_snapshot_deserialization_fuzz_smoke_rejects_bytes(
    mutation: bytes,
) -> None:
    with pytest.raises(UniverseContractError):
        decode_universe_snapshot(mutation)


def test_universe_snapshot_deserialization_mutation_fuzz_is_deterministic() -> None:
    encoded = bytearray(
        encode_universe_snapshot(parse_ticker_universe(b"AAPL\n", _resolver(("AAPL",))))
    )
    outcomes: list[tuple[int, UniverseErrorCode]] = []
    for index in range(0, len(encoded), max(1, len(encoded) // 31)):
        mutated = encoded.copy()
        mutated[index] ^= 0x5A
        try:
            decode_universe_snapshot(bytes(mutated))
        except UniverseContractError as error:
            outcomes.append((index, error.code))
    assert outcomes
    assert outcomes == sorted(outcomes, key=lambda item: item[0])


def test_required_horizons_cover_exact_labels_and_semantics() -> None:
    specs = required_horizon_specs(_configuration())

    assert tuple(spec.label for spec in specs) == REQUIRED_HORIZON_LABELS
    assert tuple((spec.unit, spec.value, spec.session_endpoint) for spec in specs) == (
        *(
            (
                ForecastHorizonUnit.TRADING_MINUTES,
                value,
                HorizonSessionEndpoint.NOT_APPLICABLE,
            )
            for value in (5, 10, 15, 30, 60, 120, 300)
        ),
        *(
            (
                ForecastHorizonUnit.TRADING_SESSIONS,
                value,
                HorizonSessionEndpoint.REGULAR_SESSION_CLOSE,
            )
            for value in (1, 5, 10, 21, 42)
        ),
    )
    assert all(spec.calendar_version == _configuration() for spec in specs)


def test_trading_minutes_skip_weekend_holiday_and_closed_periods() -> None:
    friday = _session(0, day_offset=0, minutes=10)
    monday = _session(1, day_offset=3, minutes=10)
    tuesday = _session(2, day_offset=4, minutes=10)
    calendar = _calendar(friday, monday, tuesday)
    spec = HorizonSpec.trading_minutes("5m", 5, calendar.calendar_version)
    as_of = friday.close_exchange_event_time_ns - (2 * MINUTE_NS)

    target = resolve_horizon(spec, as_of, calendar)

    assert target.target_exchange_event_time_ns == (
        monday.open_exchange_event_time_ns + (3 * MINUTE_NS)
    )
    assert target.elapsed_ns == target.target_exchange_event_time_ns - as_of


def test_session_horizon_uses_future_early_close_endpoint() -> None:
    normal = _session(0, minutes=390)
    early_close = _session(1, minutes=210)
    later = _session(2, minutes=390)
    calendar = _calendar(normal, early_close, later)
    spec = HorizonSpec.trading_sessions("1d", 1, calendar.calendar_version)

    target = resolve_horizon(
        spec, normal.open_exchange_event_time_ns + MINUTE_NS, calendar
    )

    assert (
        target.target_exchange_event_time_ns == early_close.close_exchange_event_time_ns
    )
    assert target.target_exchange_event_time_ns != (
        early_close.open_exchange_event_time_ns + (390 * MINUTE_NS)
    )


def test_minute_halt_policy_is_explicit_count_pause_or_reject() -> None:
    open_ns = BASE_NS
    halt = HaltInterval(open_ns + (3 * MINUTE_NS), open_ns + (5 * MINUTE_NS))
    session = _session(0, minutes=10, halts=(halt,))
    calendar = _calendar(session)
    as_of = open_ns + MINUTE_NS
    base = HorizonSpec.trading_minutes("5m", 5, calendar.calendar_version)

    counted = resolve_horizon(
        replace(base, halt_policy=HorizonHaltPolicy.COUNT_SCHEDULED),
        as_of,
        calendar,
    )
    paused = resolve_horizon(base, as_of, calendar)

    assert counted.target_exchange_event_time_ns == open_ns + (6 * MINUTE_NS)
    assert paused.target_exchange_event_time_ns == open_ns + (8 * MINUTE_NS)
    with pytest.raises(HorizonContractError) as caught:
        resolve_horizon(
            replace(base, halt_policy=HorizonHaltPolicy.REJECT), as_of, calendar
        )
    assert caught.value.code is HorizonErrorCode.HALT_ENCOUNTERED


def test_session_halt_at_close_is_counted_skipped_or_rejected_by_policy() -> None:
    first = _session(0, minutes=10)
    second_open = BASE_NS + DAY_NS
    close_halt = HaltInterval(
        second_open + (9 * MINUTE_NS), second_open + (10 * MINUTE_NS)
    )
    second = _session(1, minutes=10, halts=(close_halt,))
    third = _session(2, minutes=10)
    calendar = _calendar(first, second, third)
    as_of = first.open_exchange_event_time_ns + MINUTE_NS
    base = HorizonSpec.trading_sessions("1d", 1, calendar.calendar_version)

    counted = resolve_horizon(
        replace(base, halt_policy=HorizonHaltPolicy.COUNT_SCHEDULED),
        as_of,
        calendar,
    )
    paused = resolve_horizon(base, as_of, calendar)

    assert counted.target_exchange_event_time_ns == second.close_exchange_event_time_ns
    assert paused.target_exchange_event_time_ns == third.close_exchange_event_time_ns
    with pytest.raises(HorizonContractError) as caught:
        resolve_horizon(
            replace(base, halt_policy=HorizonHaltPolicy.REJECT), as_of, calendar
        )
    assert caught.value.code is HorizonErrorCode.HALT_ENCOUNTERED


def test_calendar_and_horizon_fail_closed_on_mismatch_or_insufficient_range() -> None:
    first = _session(0, minutes=10)
    second = _session(1, minutes=10)
    calendar = _calendar(first, second)
    as_of = first.open_exchange_event_time_ns + MINUTE_NS

    with pytest.raises(HorizonContractError) as mismatch:
        resolve_horizon(
            HorizonSpec.trading_sessions("1d", 1, _configuration(99)),
            as_of,
            calendar,
        )
    assert mismatch.value.code is HorizonErrorCode.CALENDAR_VERSION_MISMATCH

    with pytest.raises(HorizonContractError) as insufficient:
        resolve_horizon(
            HorizonSpec.trading_sessions("5d", 5, calendar.calendar_version),
            as_of,
            calendar,
        )
    assert insufficient.value.code is HorizonErrorCode.INSUFFICIENT_CALENDAR


@pytest.mark.parametrize(
    ("unit", "value", "endpoint"),
    [
        (ForecastHorizonUnit.TRADING_MINUTES, 0, HorizonSessionEndpoint.NOT_APPLICABLE),
        (
            ForecastHorizonUnit.TRADING_MINUTES,
            5,
            HorizonSessionEndpoint.REGULAR_SESSION_CLOSE,
        ),
        (
            ForecastHorizonUnit.TRADING_SESSIONS,
            1,
            HorizonSessionEndpoint.NOT_APPLICABLE,
        ),
    ],
)
def test_horizon_spec_rejects_invalid_values(
    unit: ForecastHorizonUnit,
    value: int,
    endpoint: HorizonSessionEndpoint,
) -> None:
    with pytest.raises(HorizonContractError) as caught:
        HorizonSpec(
            label="bad",
            unit=unit,
            value=value,
            halt_policy=HorizonHaltPolicy.PAUSE,
            session_endpoint=endpoint,
            calendar_version=_configuration(),
        )
    assert caught.value.code is HorizonErrorCode.INVALID_SPEC


def test_timestamp_overflow_and_malformed_calendar_are_rejected() -> None:
    with pytest.raises(HorizonContractError) as overflow:
        TradingSession("overflow", MAX_INT64 - MINUTE_NS, MAX_INT64 + 1)
    assert overflow.value.code is HorizonErrorCode.TIMESTAMP_OVERFLOW

    with pytest.raises(HorizonContractError) as alignment:
        TradingSession("unaligned", BASE_NS, BASE_NS + MINUTE_NS + 1)
    assert alignment.value.code is HorizonErrorCode.INVALID_SESSION

    first = _session(0, minutes=10)
    with pytest.raises(HorizonContractError) as overlap:
        _calendar(first, replace(first, session_id="overlap"))
    assert overlap.value.code is HorizonErrorCode.INVALID_CALENDAR


def test_resolution_requires_eligible_as_of_minute() -> None:
    session = _session(0, minutes=10)
    calendar = _calendar(session)
    spec = HorizonSpec.trading_minutes("5m", 5, calendar.calendar_version)

    for as_of in (
        session.open_exchange_event_time_ns,
        session.open_exchange_event_time_ns + 1,
        session.close_exchange_event_time_ns + MINUTE_NS,
    ):
        with pytest.raises(HorizonContractError) as caught:
            resolve_horizon(spec, as_of, calendar)
        assert caught.value.code is HorizonErrorCode.INVALID_AS_OF_TIME


def test_instrument_resolution_contract_rejects_inconsistent_state() -> None:
    with pytest.raises(UniverseContractError):
        InstrumentResolution(InstrumentResolutionCode.RESOLVED, None)
    with pytest.raises(UniverseContractError):
        InstrumentResolution(
            InstrumentResolutionCode.UNSUPPORTED,
            _instrument(1),
        )


def _snapshot_payload() -> dict[str, object]:
    encoded = encode_universe_snapshot(
        parse_ticker_universe(b"AAPL\nMSFT\n", _resolver(("AAPL", "MSFT")))
    )
    return cast("dict[str, object]", json.loads(encoded))


def _json_bytes(payload: object) -> bytes:
    return json.dumps(
        payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("ascii")


def test_resolver_and_snapshot_constructors_reject_inconsistent_state() -> None:
    with pytest.raises(UniverseContractError):
        StaticInstrumentResolver(
            {"AAPL": _instrument(1)},
            {"AAPL": InstrumentResolutionCode.UNSUPPORTED},
        )
    with pytest.raises(UniverseContractError):
        StaticInstrumentResolver({}, {"AAPL": InstrumentResolutionCode.RESOLVED})
    with pytest.raises(UniverseContractError):
        UniverseEntry(-1, "AAPL", InstrumentResolutionCode.UNSUPPORTED, None)
    with pytest.raises(UniverseContractError):
        UniverseEntry(0, "É", InstrumentResolutionCode.UNSUPPORTED, None)

    unresolved = parse_ticker_universe(b"AAPL\n", UnresolvedInstrumentResolver())
    assert (
        decode_universe_snapshot(encode_universe_snapshot(unresolved))
        .ordered_entries[0]
        .instrument_id
        is None
    )

    valid = parse_ticker_universe(b"AAPL\n", _resolver(("AAPL",)))
    invalid_changes = (
        {"source_path": "wrong-authority"},
        {"schema_version": "9.0.0"},
        {"source_file_sha256": b"short"},
        {"universe_snapshot_sha256": b"short"},
        {"ordered_entries": ()},
        {"ordered_entries": (replace(valid.ordered_entries[0], source_ordinal=1),)},
        {"canonical_symbols": ("WRONG",)},
    )
    for changes in invalid_changes:
        with pytest.raises(UniverseContractError):
            replace(valid, **changes)


def test_universe_parser_enforces_type_size_count_and_resolver_contract(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    with pytest.raises(UniverseContractError) as nonbytes:
        parse_ticker_universe("AAPL", UnresolvedInstrumentResolver())  # type: ignore[arg-type]
    assert nonbytes.value.code is UniverseErrorCode.INVALID_ENCODING

    with pytest.raises(UniverseContractError) as oversized:
        parse_ticker_universe(
            b"A" * (forecast_contracts.MAX_UNIVERSE_BYTES + 1),
            UnresolvedInstrumentResolver(),
        )
    assert oversized.value.code is UniverseErrorCode.SOURCE_TOO_LARGE

    monkeypatch.setattr(forecast_contracts, "MAX_UNIVERSE_SYMBOLS", 1)
    with pytest.raises(UniverseContractError) as too_many:
        parse_ticker_universe(b"AAPL\nMSFT\n", UnresolvedInstrumentResolver())
    assert too_many.value.code is UniverseErrorCode.SOURCE_TOO_LARGE

    class InvalidResolver:
        def resolve(self, symbol: str) -> object:
            return symbol

    monkeypatch.setattr(forecast_contracts, "MAX_UNIVERSE_SYMBOLS", 10_000)
    with pytest.raises(UniverseContractError) as invalid_resolution:
        parse_ticker_universe(b"AAPL\n", InvalidResolver())  # type: ignore[arg-type]
    assert invalid_resolution.value.code is UniverseErrorCode.INVALID_RESOLUTION


def test_universe_loader_and_encoder_fail_closed(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    original_path = forecast_contracts.TICKER_UNIVERSE_PATH
    monkeypatch.setattr(
        forecast_contracts,
        "TICKER_UNIVERSE_PATH",
        tmp_path / "missing-ticker.txt",
    )
    with pytest.raises(UniverseContractError) as io_error:
        load_ticker_universe(UnresolvedInstrumentResolver())
    assert io_error.value.code is UniverseErrorCode.IO_ERROR

    monkeypatch.setattr(forecast_contracts, "TICKER_UNIVERSE_PATH", original_path)
    snapshot = parse_ticker_universe(b"AAPL\n", _resolver(("AAPL",)))
    with pytest.raises(UniverseContractError) as hash_error:
        encode_universe_snapshot(
            replace(snapshot, universe_snapshot_sha256=bytes.fromhex("01" * 32))
        )
    assert hash_error.value.code is UniverseErrorCode.HASH_MISMATCH

    monkeypatch.setattr(forecast_contracts, "MAX_UNIVERSE_SNAPSHOT_BYTES", 1)
    with pytest.raises(UniverseContractError) as size_error:
        encode_universe_snapshot(snapshot)
    assert size_error.value.code is UniverseErrorCode.SOURCE_TOO_LARGE


@pytest.mark.parametrize(
    ("mutation", "code"),
    [
        ({"schema_version": "9.0.0"}, UniverseErrorCode.INVALID_SNAPSHOT),
        ({"source_path": "wrong-authority"}, UniverseErrorCode.INVALID_SNAPSHOT),
        ({"source_file_sha256": "A" * 64}, UniverseErrorCode.INVALID_SNAPSHOT),
        ({"source_file_sha256": "z" * 64}, UniverseErrorCode.INVALID_SNAPSHOT),
        ({"universe_snapshot_sha256": "0" * 64}, UniverseErrorCode.HASH_MISMATCH),
        ({"canonical_entries": []}, UniverseErrorCode.INVALID_SNAPSHOT),
        ({"canonical_entries": "bad"}, UniverseErrorCode.INVALID_SNAPSHOT),
    ],
)
def test_universe_decoder_rejects_invalid_top_level_fields(
    mutation: dict[str, object], code: UniverseErrorCode
) -> None:
    payload = _snapshot_payload()
    payload.update(mutation)
    with pytest.raises(UniverseContractError) as caught:
        decode_universe_snapshot(_json_bytes(payload))
    assert caught.value.code is code


def test_universe_decoder_rejects_untrusted_entry_shapes(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    baseline = _snapshot_payload()
    bad_entries: tuple[object, ...] = (
        "bad",
        {"symbol": "AAPL"},
        {
            "instrument_id": None,
            "resolution_code": True,
            "source_ordinal": 0,
            "symbol": "AAPL",
        },
        {
            "instrument_id": None,
            "resolution_code": 255,
            "source_ordinal": 0,
            "symbol": "AAPL",
        },
        {
            "instrument_id": 7,
            "resolution_code": int(InstrumentResolutionCode.RESOLVED),
            "source_ordinal": 0,
            "symbol": "AAPL",
        },
        {
            "instrument_id": "bad",
            "resolution_code": int(InstrumentResolutionCode.RESOLVED),
            "source_ordinal": 0,
            "symbol": "AAPL",
        },
        {
            "instrument_id": "z" * 32,
            "resolution_code": int(InstrumentResolutionCode.RESOLVED),
            "source_ordinal": 0,
            "symbol": "AAPL",
        },
        {
            "instrument_id": None,
            "resolution_code": int(InstrumentResolutionCode.UNSUPPORTED),
            "source_ordinal": "zero",
            "symbol": "AAPL",
        },
        {
            "instrument_id": None,
            "resolution_code": int(InstrumentResolutionCode.UNSUPPORTED),
            "source_ordinal": 0,
            "symbol": 7,
        },
    )
    for bad_entry in bad_entries:
        payload = dict(baseline)
        payload["canonical_entries"] = [bad_entry]
        with pytest.raises(UniverseContractError):
            decode_universe_snapshot(_json_bytes(payload))

    unsorted = dict(baseline)
    entries = cast("list[object]", unsorted["canonical_entries"])
    unsorted["canonical_entries"] = list(reversed(entries))
    with pytest.raises(UniverseContractError):
        decode_universe_snapshot(_json_bytes(unsorted))

    monkeypatch.setattr(forecast_contracts, "MAX_UNIVERSE_SYMBOLS", 1)
    with pytest.raises(UniverseContractError) as too_many:
        decode_universe_snapshot(_json_bytes(baseline))
    assert too_many.value.code is UniverseErrorCode.SOURCE_TOO_LARGE


def test_universe_decoder_requires_exact_fields_size_and_canonical_bytes(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    payload = _snapshot_payload()
    missing = dict(payload)
    missing.pop("source_path")
    with pytest.raises(UniverseContractError):
        decode_universe_snapshot(_json_bytes(missing))
    with pytest.raises(UniverseContractError):
        decode_universe_snapshot(_json_bytes([payload]))

    pretty = json.dumps(payload, indent=2, sort_keys=True).encode("ascii")
    with pytest.raises(UniverseContractError):
        decode_universe_snapshot(pretty)

    monkeypatch.setattr(forecast_contracts, "MAX_UNIVERSE_SNAPSHOT_BYTES", 1)
    with pytest.raises(UniverseContractError):
        decode_universe_snapshot(_json_bytes(payload))


def test_horizon_spec_and_text_bounds_fail_closed() -> None:
    calendar_version = _configuration()
    invalid_specs = (
        {"label": ""},
        {"label": "é"},
        {"label": "BAD"},
        {"schema_version": "9.0.0"},
        {"unit": 255},
        {"halt_policy": 255},
        {"session_endpoint": 255},
        {"value": 10_001},
        {"calendar_version": None},
        {"halt_policy": HorizonHaltPolicy.NOT_APPLICABLE},
    )
    valid = HorizonSpec.trading_minutes("5m", 5, calendar_version)
    for changes in invalid_specs:
        with pytest.raises(HorizonContractError):
            replace(valid, **changes)

    elapsed = HorizonSpec(
        "legacy",
        ForecastHorizonUnit.ELAPSED_NANOSECONDS,
        MINUTE_NS,
        HorizonHaltPolicy.NOT_APPLICABLE,
        HorizonSessionEndpoint.NOT_APPLICABLE,
        None,
    )
    assert elapsed.value == MINUTE_NS
    with pytest.raises(HorizonContractError):
        replace(elapsed, calendar_version=calendar_version)


def test_calendar_halt_and_target_constructors_reject_all_invalid_bounds(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    with pytest.raises(HorizonContractError):
        HaltInterval(0, MINUTE_NS)
    with pytest.raises(HorizonContractError):
        HaltInterval(BASE_NS, BASE_NS)
    with pytest.raises(HorizonContractError):
        TradingSession("", BASE_NS, BASE_NS + MINUTE_NS)
    with pytest.raises(HorizonContractError):
        TradingSession("bad", 0, MINUTE_NS)

    before = HaltInterval(BASE_NS - MINUTE_NS, BASE_NS)
    after = HaltInterval(BASE_NS + MINUTE_NS, BASE_NS + (3 * MINUTE_NS))
    overlap_a = HaltInterval(BASE_NS, BASE_NS + (2 * MINUTE_NS))
    overlap_b = HaltInterval(BASE_NS + MINUTE_NS, BASE_NS + (2 * MINUTE_NS))
    for halts in ((before,), (after,), (overlap_a, overlap_b)):
        with pytest.raises(HorizonContractError):
            TradingSession("bad-halt", BASE_NS, BASE_NS + (2 * MINUTE_NS), halts)

    with pytest.raises(HorizonContractError):
        ExchangeCalendar("XNYS", _configuration(), ())
    monkeypatch.setattr(forecast_contracts, "MAX_CALENDAR_SESSIONS", 1)
    with pytest.raises(HorizonContractError):
        _calendar(_session(0), _session(1))

    spec = HorizonSpec.trading_minutes("5m", 5, _configuration())
    with pytest.raises(HorizonContractError):
        HorizonTarget(spec, BASE_NS, BASE_NS, 0)
    with pytest.raises(HorizonContractError):
        HorizonTarget(spec, 0, BASE_NS, BASE_NS)


def test_horizon_resolution_elapsed_insufficient_and_halted_as_of() -> None:
    elapsed = HorizonSpec(
        "legacy",
        ForecastHorizonUnit.ELAPSED_NANOSECONDS,
        MINUTE_NS,
        HorizonHaltPolicy.NOT_APPLICABLE,
        HorizonSessionEndpoint.NOT_APPLICABLE,
        None,
    )
    assert (
        resolve_horizon(elapsed, BASE_NS, _calendar(_session(0))).elapsed_ns
        == MINUTE_NS
    )
    with pytest.raises(HorizonContractError) as overflow:
        resolve_horizon(elapsed, MAX_INT64, _calendar(_session(0)))
    assert overflow.value.code is HorizonErrorCode.TIMESTAMP_OVERFLOW

    short = _calendar(_session(0, minutes=2))
    with pytest.raises(HorizonContractError) as insufficient:
        resolve_horizon(
            HorizonSpec.trading_minutes("5m", 5, short.calendar_version),
            short.sessions[0].open_exchange_event_time_ns + MINUTE_NS,
            short,
        )
    assert insufficient.value.code is HorizonErrorCode.INSUFFICIENT_CALENDAR

    open_ns = BASE_NS
    halted = _calendar(
        _session(
            0,
            minutes=10,
            halts=(HaltInterval(open_ns, open_ns + (2 * MINUTE_NS)),),
        )
    )
    with pytest.raises(HorizonContractError) as invalid_as_of:
        resolve_horizon(
            HorizonSpec.trading_minutes("5m", 5, halted.calendar_version),
            open_ns + MINUTE_NS,
            halted,
        )
    assert invalid_as_of.value.code is HorizonErrorCode.INVALID_AS_OF_TIME
