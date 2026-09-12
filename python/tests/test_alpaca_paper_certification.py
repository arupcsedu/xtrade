"""Deterministic safety tests for the Alpaca PAPER certification boundary."""

from __future__ import annotations

import json
from dataclasses import replace
from datetime import UTC, date, datetime
from pathlib import Path
from typing import TYPE_CHECKING, cast

import pytest

from tools.alpaca_paper_certification import (
    DATA_ORIGIN,
    PAPER_ORIGIN,
    AlpacaPaperClient,
    CertificationError,
    CertificationErrorCode,
    CppSystemPathRunner,
    FixedHttpsTransport,
    HttpResponse,
    PaperAuthorization,
    PaperCredentials,
    SecretText,
    SystemPathExecution,
    _certification_instrument_id,
    _system_path_stable_hash,
    load_authorization,
    load_credentials,
    main,
    prepare_authorization,
    run_certification,
    run_preflight,
    write_json_atomic,
)

if TYPE_CHECKING:
    from collections.abc import Mapping

SESSION_DATE = date(2026, 9, 11)
NOW = datetime(2026, 9, 11, 14, 0, tzinfo=UTC)
ORDER_ID = "11111111-2222-4333-8444-555555555555"


class FakeTransport:
    """Stateful Alpaca fixture that records routes without using a network."""

    def __init__(
        self,
        *,
        market_open: bool = True,
        terminal_status: str = "canceled",
        fail_post: bool = False,
        fail_lookup: bool = False,
    ) -> None:
        """Configure deterministic market and ambiguity outcomes."""
        self.market_open = market_open
        self.terminal_status = terminal_status
        self.fail_post = fail_post
        self.fail_lookup = fail_lookup
        self.requests: list[tuple[str, str, str, bytes | None]] = []
        self.order: dict[str, object] | None = None

    @staticmethod
    def _json(value: object, status: int = 200) -> HttpResponse:
        return HttpResponse(status, json.dumps(value, sort_keys=True).encode("ascii"))

    def _make_order(self, payload: Mapping[str, object]) -> dict[str, object]:
        return {
            "asset_class": "us_equity",
            "client_order_id": payload["client_order_id"],
            "extended_hours": payload["extended_hours"],
            "filled_qty": "0",
            "id": ORDER_ID,
            "limit_price": payload["limit_price"],
            "qty": payload["qty"],
            "side": payload["side"],
            "status": "new",
            "symbol": payload["symbol"],
            "time_in_force": payload["time_in_force"],
            "type": payload["type"],
        }

    def request(
        self,
        *,
        method: str,
        origin: str,
        path: str,
        headers: Mapping[str, str],
        body: bytes | None,
        timeout_seconds: float,
    ) -> HttpResponse:
        """Return one documented shape or fail on an unexpected route."""
        assert headers["APCA-API-KEY-ID"] == "paper-key"
        assert headers["APCA-API-SECRET-KEY"] == "paper-secret"
        assert timeout_seconds == 10.0
        self.requests.append((method, origin, path, body))
        if method == "GET" and path == "/v2/account":
            return self._json(
                {
                    "account_blocked": False,
                    "id": "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
                    "status": "ACTIVE",
                    "trade_suspended_by_user": False,
                    "trading_blocked": False,
                }
            )
        if method == "GET" and path == "/v2/clock":
            return self._json(
                {
                    "is_open": self.market_open,
                    "next_close": "2026-09-11T16:00:00-04:00",
                    "next_open": "2026-09-14T09:30:00-04:00",
                    "timestamp": "2026-09-11T10:00:00-04:00",
                }
            )
        if method == "GET" and path == "/v2/assets/NVDA":
            return self._json(
                {
                    "class": "us_equity",
                    "status": "active",
                    "symbol": "NVDA",
                    "tradable": True,
                }
            )
        if method == "GET" and path.startswith("/v2/orders?"):
            return self._json([])
        if method == "GET" and path == "/v2/positions":
            return self._json([])
        if origin == DATA_ORIGIN and method == "GET":
            assert path == "/v2/stocks/NVDA/quotes/latest?feed=iex"
            return self._json({"quote": {"bp": 100}})
        if method == "POST" and path == "/v2/orders":
            payload = cast("dict[str, object]", json.loads(body or b"{}"))
            assert payload == {
                "client_order_id": payload["client_order_id"],
                "extended_hours": False,
                "limit_price": "0.01",
                "order_class": "simple",
                "position_intent": "buy_to_open",
                "qty": "1",
                "side": "buy",
                "symbol": "NVDA",
                "time_in_force": "day",
                "type": "limit",
            }
            self.order = self._make_order(payload)
            if self.fail_post:
                raise CertificationError(
                    CertificationErrorCode.NETWORK_FAILURE,
                    "synthetic timeout",
                )
            return self._json(self.order)
        if method == "GET" and path.startswith("/v2/orders:by_client_order_id?"):
            if self.fail_lookup or self.order is None:
                raise CertificationError(
                    CertificationErrorCode.PROVIDER_REJECTED,
                    "synthetic lookup failure",
                )
            return self._json(self.order)
        if method == "DELETE" and path == f"/v2/orders/{ORDER_ID}":
            return HttpResponse(204, b"")
        if method == "GET" and path == f"/v2/orders/{ORDER_ID}":
            assert self.order is not None
            self.order["status"] = self.terminal_status
            if self.terminal_status == "filled":
                self.order["filled_qty"] = "1"
            return self._json(self.order)
        message = f"unexpected route: {method} {origin} {path}"
        raise AssertionError(message)


def _system_path_evidence() -> dict[str, object]:
    instrument_id_high, instrument_id_low = _certification_instrument_id("NVDA")
    evidence: dict[str, object] = {
        "build_version": "0.1.0",
        "buy_only": True,
        "client_order_id": "34ded3bbf147dbc4f882c0a67ddd43eb",
        "fresh_pretrade_risk_required": True,
        "gateway_audit_hash": 8_985_863_997_124_911_565,
        "gateway_audit_sequence": 2,
        "gateway_configuration_hash": 11_008_645_150_851_783_344,
        "gateway_final_gate_accepted": True,
        "gateway_outbound_sequence": 1,
        "gateway_request_hash": 17_762_356_302_311_455_241,
        "instrument_id_high": instrument_id_high,
        "instrument_id_low": instrument_id_low,
        "live_trading_compiled": False,
        "mode": "PAPER",
        "oms_command_hash": 2_837_366_667_254_595_967,
        "oms_command_valid": True,
        "oms_configuration_hash": 4_576_606_819_846_895_873,
        "oms_journal_sequence": 3,
        "paper_mode": True,
        "price_increment_currency_nanos": 10_000_000,
        "price_ticks": 1,
        "quantity_units": 1,
        "regular_hours_only": True,
        "risk_approved": True,
        "risk_context_hash": 13_954_000_761_705_928_018,
        "risk_decision_hash": 15_201_679_416_317_658_683,
        "risk_journal_sequence": 1,
        "risk_snapshot_hash": 7_412_988_937_920_903_764,
        "router_configuration_hash": 16_250_863_630_627_244_289,
        "routing_decision_hash": 17_625_894_669_616_623_771,
        "routing_request_hash": 9_955_101_511_447_499_731,
        "schema_version": "1.0.0",
        "stable_hash": 0,
        "stage": "COMPLETE",
        "symbol": "NVDA",
    }
    evidence["stable_hash"] = _system_path_stable_hash(evidence)
    return evidence


class FakeSystemPathRunner:
    """Return C++-shaped evidence without starting a subprocess."""

    def __init__(self, evidence: dict[str, object] | None = None) -> None:
        """Create a runner with valid evidence unless explicitly overridden."""
        self.evidence = _system_path_evidence() if evidence is None else evidence
        self.calls: list[tuple[str, int]] = []

    def run(self, *, symbol: str, seed: int) -> SystemPathExecution:
        """Record the request and return the configured evidence."""
        self.calls.append((symbol, seed))
        return SystemPathExecution(self.evidence, "b" * 64)


def _credentials(path: Path) -> PaperCredentials:
    return PaperCredentials(SecretText("paper-key"), SecretText("paper-secret"), path)


def _client(tmp_path: Path, transport: FakeTransport) -> AlpacaPaperClient:
    return AlpacaPaperClient(_credentials(tmp_path / ".keys"), transport)


def _authorization() -> PaperAuthorization:
    return prepare_authorization(
        authorization_id="operator-paper-certification-20260911",
        expected_session_date=SESSION_DATE,
        issued_at_utc=datetime(2026, 9, 10, 20, 0, tzinfo=UTC),
        expires_at_utc=datetime(2026, 9, 11, 21, 0, tzinfo=UTC),
    )


def _write_keys(path: Path, endpoint: str = f"{PAPER_ORIGIN}/v2") -> None:
    path.write_text(
        "\n".join(
            (
                f"AEGIS_ALPACA_PAPER_ENDPOINT={endpoint}",
                "AEGIS_ALPACA_PAPER_KEY_ID=paper-key",
                "AEGIS_ALPACA_PAPER_SECRET_KEY=paper-secret",
            )
        ),
        encoding="utf-8",
    )
    path.chmod(0o600)


def test_credentials_are_fixed_to_paper_origin_and_never_printable(
    tmp_path: Path,
) -> None:
    path = tmp_path / ".keys"
    _write_keys(path)
    credentials = load_credentials(path)
    assert credentials.origin == PAPER_ORIGIN
    assert str(credentials.key_id) == "[REDACTED]"
    assert repr(credentials.secret_key) == "[REDACTED]"
    assert "paper-key" not in repr(credentials)
    assert "paper-secret" not in repr(credentials)

    _write_keys(path, "https://api.alpaca.markets/v2")
    with pytest.raises(CertificationError) as rejected:
        load_credentials(path)
    assert rejected.value.code is CertificationErrorCode.NON_PAPER_ENDPOINT

    with pytest.raises(CertificationError) as live_origin:
        FixedHttpsTransport().request(
            method="GET",
            origin="https://api.alpaca.markets",
            path="/v2/account",
            headers={},
            body=None,
            timeout_seconds=1,
        )
    assert live_origin.value.code is CertificationErrorCode.NON_PAPER_ENDPOINT


def test_credential_loader_rejects_unsafe_or_ambiguous_files(tmp_path: Path) -> None:
    path = tmp_path / ".keys"
    _write_keys(path)
    path.chmod(0o644)
    with pytest.raises(CertificationError) as insecure:
        load_credentials(path)
    assert insecure.value.code is CertificationErrorCode.SECRET_FILE_UNSAFE

    path.unlink()
    target = tmp_path / "target"
    _write_keys(target)
    path.symlink_to(target)
    with pytest.raises(CertificationError) as linked:
        load_credentials(path)
    assert linked.value.code is CertificationErrorCode.SECRET_FILE_UNSAFE

    path.unlink()
    _write_keys(path)
    with path.open("a", encoding="utf-8") as output:
        output.write("\nUNKNOWN=value\n")
    with pytest.raises(CertificationError) as unknown:
        load_credentials(path)
    assert unknown.value.code is CertificationErrorCode.CREDENTIAL_MALFORMED


def test_read_only_preflight_uses_only_fixed_documented_routes(tmp_path: Path) -> None:
    transport = FakeTransport()
    report = run_preflight(
        _client(tmp_path, transport), symbol="NVDA", observed_at_utc=NOW
    )
    assert report["status"] == "READY"
    assert report["mode"] == "PAPER"
    assert report["live_trading_enabled"] is False
    assert report["system_order_path_certified"] is False
    assert report["mutation_count"] == 0
    assert report["ticker_count"] == 79
    assert all(method == "GET" for method, _origin, _path, _body in transport.requests)
    assert all(
        origin == PAPER_ORIGIN for _method, origin, _path, _body in transport.requests
    )


def test_certification_submits_once_cancels_only_created_order_and_preserves_state(
    tmp_path: Path,
) -> None:
    transport = FakeTransport()
    client = _client(tmp_path, transport)
    report = run_certification(
        client,
        symbol="NVDA",
        authorization=_authorization(),
        authorization_sha256="a" * 64,
        execute_paper=True,
        observed_at_utc=NOW,
        poll_attempts=2,
        poll_interval_seconds=0,
        sleeper=lambda _seconds: None,
    )
    assert report["status"] == "PASSED"
    assert report["cancel_confirmed"] is True
    assert report["existing_state_preserved"] is True
    assert report["order_quantity"] == 1
    assert report["order_limit_price"] == "0.01"
    assert report["mutation_count"] == 2
    mutations = [item for item in transport.requests if item[0] != "GET"]
    assert [(item[0], item[2]) for item in mutations] == [
        ("POST", "/v2/orders"),
        ("DELETE", f"/v2/orders/{ORDER_ID}"),
    ]


def test_system_path_certification_binds_router_risk_oms_and_gateway_evidence(
    tmp_path: Path,
) -> None:
    transport = FakeTransport()
    runner = FakeSystemPathRunner()
    report = run_certification(
        _client(tmp_path, transport),
        symbol="NVDA",
        authorization=_authorization(),
        authorization_sha256="9" * 64,
        execute_paper=True,
        observed_at_utc=NOW,
        poll_attempts=1,
        poll_interval_seconds=0,
        sleeper=lambda _seconds: None,
        system_path_runner=runner,
        system_path_seed=20_260_911,
    )
    assert runner.calls == [("NVDA", 20_260_911)]
    assert report["status"] == "PASSED"
    assert report["system_order_path_certified"] is True
    assert report["broker_response_roundtrip_certified"] is False
    assert report["certification_kind"] == "alpaca-paper-system-command-path"
    assert report["system_path_binary_sha256"] == "b" * 64
    assert isinstance(report["system_path_evidence_sha256"], str)
    posted = next(item for item in transport.requests if item[0] == "POST")
    payload = cast("dict[str, object]", json.loads(posted[3] or b"{}"))
    assert payload["client_order_id"] == runner.evidence["client_order_id"]


def test_tampered_system_path_evidence_blocks_before_order_submission(
    tmp_path: Path,
) -> None:
    evidence = _system_path_evidence()
    evidence["quantity_units"] = 2
    runner = FakeSystemPathRunner(evidence)
    transport = FakeTransport()
    with pytest.raises(CertificationError) as rejected:
        run_certification(
            _client(tmp_path, transport),
            symbol="NVDA",
            authorization=_authorization(),
            authorization_sha256="8" * 64,
            execute_paper=True,
            observed_at_utc=NOW,
            system_path_runner=runner,
        )
    assert rejected.value.code is CertificationErrorCode.SYSTEM_PATH_INVALID
    assert all(item[0] == "GET" for item in transport.requests)


def test_system_path_runner_rejects_symlink_and_writable_executable(
    tmp_path: Path,
) -> None:
    target = tmp_path / "path-binary"
    target.write_bytes(b"not an executable")
    target.chmod(0o755)
    linked = tmp_path / "linked-binary"
    linked.symlink_to(target)
    with pytest.raises(CertificationError) as symlinked:
        CppSystemPathRunner(linked).run(symbol="NVDA", seed=20_260_911)
    assert symlinked.value.code is CertificationErrorCode.SYSTEM_PATH_FAILED

    target.chmod(0o775)
    with pytest.raises(CertificationError) as writable:
        CppSystemPathRunner(target).run(symbol="NVDA", seed=20_260_911)
    assert writable.value.code is CertificationErrorCode.SYSTEM_PATH_FAILED


def test_certification_never_retries_an_ambiguous_submission(tmp_path: Path) -> None:
    transport = FakeTransport(fail_post=True)
    report = run_certification(
        _client(tmp_path, transport),
        symbol="NVDA",
        authorization=_authorization(),
        authorization_sha256="b" * 64,
        execute_paper=True,
        observed_at_utc=NOW,
        poll_attempts=1,
        poll_interval_seconds=0,
        sleeper=lambda _seconds: None,
    )
    assert report["status"] == "PASSED"
    assert sum(item[0] == "POST" for item in transport.requests) == 1

    unresolved = FakeTransport(fail_post=True, fail_lookup=True)
    with pytest.raises(CertificationError) as ambiguous:
        run_certification(
            _client(tmp_path, unresolved),
            symbol="NVDA",
            authorization=_authorization(),
            authorization_sha256="c" * 64,
            execute_paper=True,
            observed_at_utc=NOW,
            poll_attempts=1,
            poll_interval_seconds=0,
            sleeper=lambda _seconds: None,
        )
    assert ambiguous.value.code is CertificationErrorCode.ORDER_AMBIGUOUS
    assert sum(item[0] == "POST" for item in unresolved.requests) == 1


def test_certification_fails_closed_before_or_after_mutation(tmp_path: Path) -> None:
    with pytest.raises(CertificationError) as flag:
        run_certification(
            _client(tmp_path, FakeTransport()),
            symbol="NVDA",
            authorization=_authorization(),
            authorization_sha256="d" * 64,
            execute_paper=False,
            observed_at_utc=NOW,
        )
    assert flag.value.code is CertificationErrorCode.AUTHORIZATION_INVALID

    closed = FakeTransport(market_open=False)
    with pytest.raises(CertificationError) as market:
        run_certification(
            _client(tmp_path, closed),
            symbol="NVDA",
            authorization=_authorization(),
            authorization_sha256="e" * 64,
            execute_paper=True,
            observed_at_utc=NOW,
        )
    assert market.value.code is CertificationErrorCode.MARKET_CLOSED
    assert all(item[0] == "GET" for item in closed.requests)

    filled = FakeTransport(terminal_status="filled")
    with pytest.raises(CertificationError) as unexpected_fill:
        run_certification(
            _client(tmp_path, filled),
            symbol="NVDA",
            authorization=_authorization(),
            authorization_sha256="f" * 64,
            execute_paper=True,
            observed_at_utc=NOW,
            poll_attempts=1,
            poll_interval_seconds=0,
            sleeper=lambda _seconds: None,
        )
    assert unexpected_fill.value.code is CertificationErrorCode.CANCEL_UNCONFIRMED


def test_authorization_round_trip_is_owner_only_and_bound_to_universe(
    tmp_path: Path,
) -> None:
    path = tmp_path / "authorization.json"
    authorization = _authorization()
    digest = write_json_atomic(path, authorization.to_dict())
    loaded, loaded_digest = load_authorization(path)
    assert loaded == authorization
    assert loaded_digest == digest
    assert stat_mode(path) == 0o600
    loaded.validate(now_utc=NOW, universe_sha256=loaded.universe_snapshot_sha256)

    with pytest.raises(CertificationError) as mismatch:
        loaded.validate(now_utc=NOW, universe_sha256="0" * 64)
    assert mismatch.value.code is CertificationErrorCode.UNIVERSE_MISMATCH

    expired = replace(loaded, expires_at_utc=NOW)
    with pytest.raises(CertificationError) as stale:
        expired.validate(now_utc=NOW, universe_sha256=loaded.universe_snapshot_sha256)
    assert stale.value.code is CertificationErrorCode.AUTHORIZATION_EXPIRED

    changed_source = replace(loaded, system_path_source_sha256="0" * 64)
    with pytest.raises(CertificationError) as source_mismatch:
        changed_source.validate(
            now_utc=NOW,
            universe_sha256=loaded.universe_snapshot_sha256,
        )
    assert source_mismatch.value.code is CertificationErrorCode.AUTHORIZATION_INVALID


def stat_mode(path: Path) -> int:
    """Return only Unix permission bits for a fixture."""
    return path.stat().st_mode & 0o777


def test_cli_prepares_scope_runs_mock_preflight_and_emits_redacted_errors(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    authorization_path = tmp_path / "authorization.json"
    assert (
        main(
            [
                "prepare-authorization",
                "--authorization-id",
                "cli-paper-certification",
                "--expected-session-date",
                "2026-09-11",
                "--issued-at-utc",
                "2026-09-10T20:00:00+00:00",
                "--expires-at-utc",
                "2026-09-11T21:00:00+00:00",
                "--output",
                str(authorization_path),
            ],
            now_utc=NOW,
        )
        == 0
    )
    prepared = json.loads(capsys.readouterr().out)
    assert prepared["status"] == "PREPARED"

    keys = tmp_path / ".keys"
    _write_keys(keys)
    report_path = tmp_path / "preflight.json"
    assert (
        main(
            [
                "--secret-file",
                str(keys),
                "preflight",
                "--symbol",
                "NVDA",
                "--report",
                str(report_path),
            ],
            transport=FakeTransport(),
            now_utc=NOW,
        )
        == 0
    )
    assert json.loads(capsys.readouterr().out)["status"] == "READY"
    assert json.loads(report_path.read_text(encoding="ascii"))["mutation_count"] == 0

    certification_path = tmp_path / "certification.json"
    assert (
        main(
            [
                "--secret-file",
                str(keys),
                "certify",
                "--symbol",
                "NVDA",
                "--authorization",
                str(authorization_path),
                "--report",
                str(certification_path),
                "--execute-paper",
            ],
            transport=FakeTransport(),
            now_utc=NOW,
        )
        == 0
    )
    assert json.loads(capsys.readouterr().out)["status"] == "PASSED"
    assert (
        json.loads(certification_path.read_text(encoding="ascii"))[
            "system_order_path_certified"
        ]
        is False
    )

    _write_keys(keys, "https://api.alpaca.markets/v2")
    assert main(["--secret-file", str(keys), "preflight"], now_utc=NOW) == 1
    blocked = capsys.readouterr().out
    assert "NON_PAPER_ENDPOINT" in blocked
    assert "paper-key" not in blocked
    assert "paper-secret" not in blocked


def test_invalid_authorization_cannot_expand_scope() -> None:
    authorization = _authorization()
    expanded = replace(authorization, maximum_quantity_per_order=2)
    with pytest.raises(CertificationError) as rejected:
        expanded.validate(
            now_utc=NOW,
            universe_sha256=authorization.universe_snapshot_sha256,
        )
    assert rejected.value.code is CertificationErrorCode.AUTHORIZATION_INVALID


def test_checked_in_json_schemas_cover_bounded_outputs(tmp_path: Path) -> None:
    report_schema = json.loads(
        Path("schemas/alpaca-paper-certification-report-v1.schema.json").read_text(
            encoding="utf-8"
        )
    )
    authorization_schema = json.loads(
        Path(
            "schemas/alpaca-paper-certification-authorization-v1.schema.json"
        ).read_text(encoding="utf-8")
    )
    authorization = _authorization()
    assert set(authorization.to_dict()) == set(authorization_schema["required"])
    assert set(authorization.to_dict()).issubset(authorization_schema["properties"])

    preflight = run_preflight(
        _client(tmp_path, FakeTransport()), symbol="NVDA", observed_at_utc=NOW
    )
    certification = run_certification(
        _client(tmp_path, FakeTransport()),
        symbol="NVDA",
        authorization=authorization,
        authorization_sha256="1" * 64,
        execute_paper=True,
        observed_at_utc=NOW,
        poll_attempts=1,
        poll_interval_seconds=0,
        sleeper=lambda _seconds: None,
    )
    for report in (preflight, certification):
        assert set(report_schema["required"]).issubset(report)
        assert set(report).issubset(report_schema["properties"])
