"""Tests for bounded Alpaca Basic/IEX historical minute ingestion."""

from __future__ import annotations

import http.client
import json
import os
import signal
import ssl
from datetime import UTC, date, datetime, timedelta
from pathlib import Path
from types import SimpleNamespace
from typing import TYPE_CHECKING, ClassVar, Never, cast

import aegis_mx_research.alpaca_historical as alpaca_module
import pytest
from aegis_mx_research.alpaca_historical import (
    ADJUSTMENT,
    DATA_HOST,
    DEFAULT_SAMPLE_SEED,
    FEED,
    PAPER_HOST,
    AlpacaApiClient,
    AlpacaCredentials,
    AlpacaDataError,
    AlpacaDataErrorCode,
    HttpResponse,
    RunMode,
    _asset_status,
    _atomic_write,
    _execution_summary,
    _load_hashed_json,
    _normalize_bar,
    _parse_rfc3339_ns,
    _storage_projection,
    _termination_signal,
    accept_pilot,
    execute_run,
    load_source_authorization,
    make_tasks,
    parse_calendar,
    prepare_academic_authorization,
    verify_run,
)
from aegis_mx_research.data_repository import (
    DataRepository,
    ManifestLineage,
    ManifestTimeRange,
    PartitionManifest,
    QuotaEvidence,
    SourceManifest,
    StorageError,
    StorageErrorCode,
)
from aegis_mx_research.forecast_contracts import (
    UnresolvedInstrumentResolver,
    load_ticker_universe,
)

import tools.source_policy_check as source_policy_module

if TYPE_CHECKING:
    from collections.abc import Sequence

    from aegis_mx_research.data_repository import AdmissionLease

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
EXAMPLE_POLICY = REPOSITORY_ROOT / "infra/data_poc/source-policy.example.json"
PILOT_DATES = ("2026-09-03", "2026-09-04", "2026-09-08", "2026-09-09", "2026-09-10")


class FakeAlpacaTransport:
    """Deterministic in-process Alpaca response fixture."""

    def __init__(
        self,
        *,
        missing_asset: str | None = None,
        fail_on_bar_call: int | None = None,
    ) -> None:
        """Create a deterministic fixture with one optional missing asset."""
        self.calls: list[tuple[str, str]] = []
        self.missing_asset = missing_asset
        self.fail_on_bar_call = fail_on_bar_call
        self.bar_calls = 0

    def get(
        self,
        host: str,
        path: str,
        parameters: Sequence[tuple[str, str]],
        credentials: AlpacaCredentials,
        *,
        timeout_seconds: float,
        maximum_response_bytes: int,
    ) -> HttpResponse:
        """Return a deterministic bounded response for an expected route."""
        del credentials, timeout_seconds, maximum_response_bytes
        self.calls.append((host, path))
        received = datetime(2026, 9, 11, 13, 32, tzinfo=UTC)
        if (host, path) == (PAPER_HOST, "/v2/clock"):
            payload: object = {
                "is_open": True,
                "timestamp": "2026-09-11T09:31:00-04:00",
            }
            status = 200
        elif (host, path) == (PAPER_HOST, "/v2/calendar"):
            payload = [
                {"date": item, "open": "09:30", "close": "16:00"}
                for item in PILOT_DATES
            ]
            status = 200
        elif host == PAPER_HOST and path.startswith("/v2/assets/"):
            symbol = path.rsplit("/", 1)[1]
            if symbol == self.missing_asset:
                payload = {"code": 40410000, "message": "asset not found"}
                status = 404
            else:
                payload = {
                    "class": "us_equity",
                    "exchange": "NASDAQ",
                    "id": f"fixture-{symbol}",
                    "status": "active",
                    "symbol": symbol,
                }
                status = 200
        elif (host, path) == (DATA_HOST, "/v2/stocks/bars"):
            self.bar_calls += 1
            if self.bar_calls == self.fail_on_bar_call:
                raise AlpacaDataError(
                    AlpacaDataErrorCode.NETWORK_FAILURE,
                    "injected bounded interruption",
                )
            query = dict(parameters)
            symbols = query["symbols"].split(",")
            start = datetime.fromisoformat(query["start"])
            end = datetime.fromisoformat(query["end"])
            bars: dict[str, list[dict[str, object]]] = {}
            for symbol in symbols:
                rows = []
                for day in PILOT_DATES:
                    timestamp = datetime.fromisoformat(f"{day}T13:30:00+00:00")
                    if start <= timestamp < end:
                        rows.append(
                            {
                                "c": 100.5,
                                "h": 101.0,
                                "l": 99.0,
                                "n": 1,
                                "o": 100.0,
                                "t": f"{day}T13:30:00Z",
                                "v": 10,
                                "vw": 100.25,
                            }
                        )
                bars[symbol] = rows
            payload = {"bars": bars, "next_page_token": None}
            status = 200
        else:
            raise AssertionError((host, path, parameters))
        body = json.dumps(payload, separators=(",", ":"), sort_keys=True).encode()
        return HttpResponse(status, body, received)


class ScriptedTransport:
    """Return a finite sequence of response fixtures."""

    def __init__(self, responses: list[HttpResponse]) -> None:
        """Retain responses in call order."""
        self.responses = responses
        self.parameters: list[tuple[tuple[str, str], ...]] = []

    def get(
        self,
        host: str,
        path: str,
        parameters: Sequence[tuple[str, str]],
        credentials: AlpacaCredentials,
        *,
        timeout_seconds: float,
        maximum_response_bytes: int,
    ) -> HttpResponse:
        """Return the next scripted response."""
        del host, path, credentials, timeout_seconds, maximum_response_bytes
        self.parameters.append(tuple(parameters))
        return self.responses.pop(0)


class FakeVerificationRepository:
    """Minimal deterministic repository double for verifier fault tests."""

    manifests: ClassVar[dict[str, object]] = {}
    verification_passed: ClassVar[bool] = True

    def __init__(self, root: Path) -> None:
        """Retain the root used by sampled-payload reads."""
        self.root = root

    def initialize(self) -> None:
        """Model an initialized read-only repository."""

    def verify(self) -> object:
        """Return the configured repository-wide verification result."""
        return SimpleNamespace(
            passed=self.verification_passed,
            to_dict=lambda: {"passed": self.verification_passed},
        )

    def load_manifest(self, manifest_id: str) -> object:
        """Resolve one configured manifest by immutable ID."""
        return self.manifests[manifest_id]


def _authorization(root: Path) -> tuple[Path, Path]:
    approval = root / "manifests/approvals/approval.json"
    policy = root / "manifests/approvals/policy.json"
    prepare_academic_authorization(
        operator_id="academic-test-user",
        data_root=root,
        approval_path=approval,
        policy_path=policy,
        expires_at_utc=datetime(2026, 10, 11, tzinfo=UTC),
        example_policy_path=EXAMPLE_POLICY,
    )
    return approval, policy


def _secrets(path: Path) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "AEGIS_ALPACA_PAPER_ENDPOINT=https://paper-api.alpaca.markets\n"
        "AEGIS_ALPACA_PAPER_KEY_ID=fixture-key\n"  # pragma: allowlist secret
        "AEGIS_ALPACA_PAPER_SECRET_KEY=fixture-secret\n",  # pragma: allowlist secret
        encoding="utf-8",
    )
    path.chmod(0o600)
    return path


def _source_manifest(
    payload: bytes, storage_path: str = "raw/fixture.json"
) -> SourceManifest:
    times = ManifestTimeRange(1, 1, 1, 1, 1, 1, 1, 1, 1, 1)
    return SourceManifest(
        source_name=alpaca_module.SOURCE_ID,
        source_version=alpaca_module.SOURCE_VERSION,
        source_object_id="fixture-object",
        storage_path=storage_path,
        object_sha256=alpaca_module._sha256(payload),
        size_bytes=len(payload),
        record_count=1,
        schema_name="fixture-schema",
        schema_version="1.0.0",
        times=times,
        universe_snapshot_sha256="1" * 64,
        lineage=ManifestLineage(),
    )


def _partition_manifest(
    payload: bytes,
    *,
    partition_key: str = "symbol=AAPL/session=2026-09-03",
    record_count: int = 1,
    object_sha256: str | None = None,
) -> PartitionManifest:
    digest = alpaca_module._sha256(payload)
    return PartitionManifest(
        dataset_name=alpaca_module.DATASET_ID,
        partition_key=partition_key,
        storage_path=f"canonical/{digest}.jsonl",
        object_sha256=object_sha256 or digest,
        size_bytes=len(payload),
        record_count=record_count,
        schema_name="aegis-alpaca-minute-bar",
        schema_version=alpaca_module.CANONICAL_SCHEMA_VERSION,
        times=ManifestTimeRange(1, 1, 1, 1, 1, 1, 1, 1, 1, 1),
        universe_snapshot_sha256="1" * 64,
        source_manifest_ids=(f"source-{'1' * 64}",),
        lineage=ManifestLineage(),
    )


def _backfill_acceptance() -> dict[str, object]:
    snapshot = load_ticker_universe(UnresolvedInstrumentResolver())
    return {
        "assets": {
            symbol: {
                "asset_id": f"fixture-{symbol}",
                "status": "RESOLVED",
            }
            for symbol in snapshot.canonical_symbols
        },
        "pilot_metrics": {
            "canonical_bytes": 100_000,
            "canonical_records": 395,
            "raw_records": 395,
            "source_bytes": 50_000,
        },
    }


def _patch_backfill_dependencies(
    monkeypatch: pytest.MonkeyPatch, acceptance: object
) -> alpaca_module.TradingSession:
    session = parse_calendar(
        [{"date": "2026-09-10", "open": "09:30", "close": "16:00"}]
    )[0]
    monkeypatch.setattr(
        alpaca_module,
        "resolve_run_sessions",
        lambda _client, _mode: (date(2026, 9, 10), (session,), "calendar-v1", "2" * 64),
    )
    monkeypatch.setattr(
        alpaca_module,
        "_load_pilot_acceptance",
        lambda *_args, **_kwargs: acceptance,
    )
    monkeypatch.setattr(
        alpaca_module,
        "_cluster_quota_evidence",
        lambda _root: QuotaEvidence(
            limit_bytes=10_000_000_000_000,
            used_bytes=0,
            source="test fixture",
            observed_at_utc="2026-09-11T00:00:00+00:00",
            authoritative=True,
        ),
    )
    return session


def _verification_report(manifest_ids: list[str]) -> dict[str, object]:
    return {
        "assets": {"AAPL": {"status": "RESOLVED"}},
        "live_trading_capable": False,
        "missing_symbol_session_pairs": [],
        "partition_manifest_ids": manifest_ids,
        "schema_version": "1.0.0",
        "sessions": [{"date": "2026-09-03"}],
        "status": "COMPLETED",
    }


def test_credentials_accept_shell_style_whitespace_without_exposing_values(
    tmp_path: Path,
) -> None:
    path = tmp_path / ".keys"
    path.write_text(
        "AEGIS_ALPACA_PAPER_ENDPOINT = https://paper-api.alpaca.markets/v2\n"
        "AEGIS_ALPACA_PAPER_KEY_ID = fixture-key\n"  # pragma: allowlist secret
        "AEGIS_ALPACA_PAPER_SECRET_KEY = fixture-secret\n",  # pragma: allowlist secret
        encoding="utf-8",
    )
    path.chmod(0o600)

    credentials = AlpacaCredentials.load(path)

    assert repr(credentials) == "AlpacaCredentials([REDACTED])"
    assert credentials.headers() == {
        "APCA-API-KEY-ID": "fixture-key",
        "APCA-API-SECRET-KEY": "fixture-secret",
    }


def test_termination_unwinds_and_removes_incomplete_atomic_temp(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    target = tmp_path / "record.json"

    def terminate_during_sync(_descriptor: int) -> None:
        _termination_signal(15, None)

    monkeypatch.setattr(os, "fsync", terminate_during_sync)
    with pytest.raises(AlpacaDataError) as failure:
        _atomic_write(target, b"payload")

    assert failure.value.code is AlpacaDataErrorCode.INTERRUPTED
    assert not target.exists()
    assert list(tmp_path.iterdir()) == []


def test_timestamp_parsers_fail_closed() -> None:
    naive = datetime(2026, 1, 1, tzinfo=UTC).replace(tzinfo=None)
    with pytest.raises(AlpacaDataError):
        alpaca_module._utc_ns(naive)
    for rfc3339_value in (42, "2026-02-30T00:00:00Z"):
        with pytest.raises(AlpacaDataError):
            alpaca_module._parse_rfc3339_ns(rfc3339_value)
    for utc_second_value in (None, "bad"):
        with pytest.raises(AlpacaDataError):
            alpaca_module._parse_utc_second(utc_second_value, "field")
    for clock_value in (None, "bad", "2026-01-01T00:00:00"):
        with pytest.raises(AlpacaDataError):
            alpaca_module._parse_clock_datetime(clock_value)


def test_secure_reader_and_hashed_json_reject_unsafe_evidence(tmp_path: Path) -> None:
    empty = tmp_path / "empty"
    empty.write_bytes(b"")
    with pytest.raises(AlpacaDataError, match="empty"):
        alpaca_module._read_secure(empty, 10, "fixture")

    oversized = tmp_path / "oversized"
    oversized.write_bytes(b"123")
    with pytest.raises(AlpacaDataError, match="size bound"):
        alpaca_module._read_secure(oversized, 2, "fixture")

    target = tmp_path / "target"
    target.write_bytes(b"value")
    link = tmp_path / "link"
    link.symlink_to(target)
    with pytest.raises(AlpacaDataError, match="non-symlink"):
        alpaca_module._read_secure(link, 10, "fixture")

    with pytest.raises(AlpacaDataError, match="cannot read"):
        alpaca_module._read_secure(tmp_path / "missing", 10, "fixture")

    malformed = tmp_path / "malformed.json"
    malformed.write_bytes(b"{")
    with pytest.raises(AlpacaDataError, match="malformed JSON"):
        _load_hashed_json(malformed, "fixture")

    nonobject = tmp_path / "array.json"
    nonobject.write_text("[]", encoding="utf-8")
    with pytest.raises(AlpacaDataError, match="not a JSON object"):
        _load_hashed_json(nonobject, "fixture")

    mismatch = tmp_path / "mismatch.json"
    mismatch.write_text('{"document_sha256":"' + "0" * 64 + '"}', encoding="utf-8")
    with pytest.raises(AlpacaDataError, match="does not match"):
        _load_hashed_json(mismatch, "fixture")


def test_secure_reader_detects_open_race_and_growth(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    path = tmp_path / "evidence"
    path.write_bytes(b"value")
    actual = os.lstat(path)
    monkeypatch.setattr(
        os,
        "fstat",
        lambda _descriptor: SimpleNamespace(
            st_dev=actual.st_dev,
            st_ino=actual.st_ino + 1,
        ),
    )
    with pytest.raises(AlpacaDataError, match="changed while"):
        alpaca_module._read_secure(path, 10, "fixture")

    monkeypatch.undo()
    path.write_bytes(b"01234567890")
    actual = os.lstat(path)
    monkeypatch.setattr(
        os,
        "lstat",
        lambda _path: SimpleNamespace(
            st_mode=actual.st_mode,
            st_size=1,
            st_dev=actual.st_dev,
            st_ino=actual.st_ino,
        ),
    )
    with pytest.raises(AlpacaDataError, match="size bound"):
        alpaca_module._read_secure(path, 10, "fixture")


@pytest.mark.parametrize(
    "content",
    [
        b"\xff",
        b"MALFORMED\n",
        (
            b"AEGIS_ALPACA_PAPER_ENDPOINT=https://paper-api.alpaca.markets\n"
            b"AEGIS_ALPACA_PAPER_ENDPOINT=https://paper-api.alpaca.markets\n"
        ),
        b"AEGIS_ALPACA_PAPER_ENDPOINT=https://example.invalid\n",
        b"AEGIS_ALPACA_PAPER_ENDPOINT=https://paper-api.alpaca.markets\n",
    ],
)
def test_credentials_reject_malformed_or_incomplete_files(
    tmp_path: Path, content: bytes
) -> None:
    path = tmp_path / ".keys"
    path.write_bytes(content)
    path.chmod(0o600)
    with pytest.raises(AlpacaDataError):
        AlpacaCredentials.load(path)


def test_credentials_reject_missing_unsafe_and_symlink_files(tmp_path: Path) -> None:
    with pytest.raises(AlpacaDataError, match="unavailable"):
        AlpacaCredentials.load(tmp_path / "missing")

    unsafe = _secrets(tmp_path / "unsafe")
    unsafe.chmod(0o644)
    with pytest.raises(AlpacaDataError, match="unsafe"):
        AlpacaCredentials.load(unsafe)

    link = tmp_path / "linked"
    link.symlink_to(unsafe)
    with pytest.raises(AlpacaDataError, match="unsafe"):
        AlpacaCredentials.load(link)

    with pytest.raises(AlpacaDataError, match="absent or malformed"):
        AlpacaCredentials("", "secret")

    commented = tmp_path / "commented"
    commented.write_text(
        "# comment\n\n"
        "AEGIS_ALPACA_PAPER_ENDPOINT=https://paper-api.alpaca.markets\n"
        "AEGIS_ALPACA_PAPER_KEY_ID=fixture\n"
        "AEGIS_ALPACA_PAPER_SECRET_KEY=fixture\n",
        encoding="utf-8",
    )
    commented.chmod(0o600)
    assert "REDACTED" in repr(AlpacaCredentials.load(commented))


def test_fixed_host_transport_bounds_hosts_responses_and_network_failures(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    credentials = AlpacaCredentials("fixture", "fixture")
    ssl_context = ssl.create_default_context()
    transport = alpaca_module.FixedHostHttpsTransport(ssl_context)
    with pytest.raises(AlpacaDataError, match="allowlist"):
        transport.get(
            "example.invalid",
            "/v2/clock",
            (),
            credentials,
            timeout_seconds=1,
            maximum_response_bytes=10,
        )

    class Response:
        status = 200

        def __init__(self, body: bytes, retry_after: str | None = None) -> None:
            self.body = body
            self.retry_after = retry_after

        def read(self, _size: int) -> bytes:
            return self.body

        def getheader(self, _name: str) -> str | None:
            return self.retry_after

    class Connection:
        response = Response(b"{}", "999")
        raise_request = False
        requested_target = ""
        closed = False

        def __init__(self, host: str, *, timeout: float, context: object) -> None:
            assert host == PAPER_HOST
            assert timeout == 1
            assert context is ssl_context

        def request(self, method: str, target: str, *, headers: dict[str, str]) -> None:
            assert method == "GET"
            assert "APCA-API-KEY-ID" in headers
            type(self).requested_target = target
            if type(self).raise_request:
                message = "fixture"
                raise OSError(message)

        def getresponse(self) -> Response:
            return type(self).response

        def close(self) -> None:
            type(self).closed = True

    monkeypatch.setattr(http.client, "HTTPSConnection", Connection)
    response = transport.get(
        PAPER_HOST,
        "/v2/calendar",
        (("start", "2026-01-01"),),
        credentials,
        timeout_seconds=1,
        maximum_response_bytes=10,
    )
    assert response.retry_after_seconds == 60
    assert "start=2026-01-01" in Connection.requested_target
    assert Connection.closed is True

    Connection.response = Response(b"x" * 11, "nondigit")
    with pytest.raises(AlpacaDataError, match="byte limit"):
        transport.get(
            PAPER_HOST,
            "/v2/clock",
            (),
            credentials,
            timeout_seconds=1,
            maximum_response_bytes=10,
        )

    Connection.raise_request = True
    with pytest.raises(AlpacaDataError, match="OSError"):
        transport.get(
            PAPER_HOST,
            "/v2/clock",
            (),
            credentials,
            timeout_seconds=1,
            maximum_response_bytes=10,
        )


def test_api_client_retries_rejects_and_validates_tokens() -> None:
    now = datetime(2026, 9, 11, tzinfo=UTC)
    sleeps: list[float] = []
    transport = ScriptedTransport(
        [
            HttpResponse(500, b"{}", now),
            HttpResponse(200, b'{"ok":true}', now),
        ]
    )
    ticks = iter((1.0, 1.0, 1.1, 1.1, 2.0, 2.0))
    client = AlpacaApiClient(
        AlpacaCredentials("fixture", "fixture"),
        transport=transport,
        sleep=sleeps.append,
        monotonic=lambda: next(ticks),
    )
    assert client.clock().attempts == 2
    assert client.retry_count == 1
    assert sleeps

    malformed = AlpacaApiClient(
        AlpacaCredentials("fixture", "fixture"),
        transport=ScriptedTransport([HttpResponse(200, b"{", now)]),
        sleep=lambda _delay: None,
        monotonic=lambda: 0,
    )
    with pytest.raises(AlpacaDataError, match="malformed JSON"):
        malformed.clock()

    rejected = AlpacaApiClient(
        AlpacaCredentials("fixture", "fixture"),
        transport=ScriptedTransport([HttpResponse(400, b"{}", now)]),
        sleep=lambda _delay: None,
        monotonic=lambda: 0,
    )
    with pytest.raises(AlpacaDataError) as failure:
        rejected.clock()
    assert failure.value.code is AlpacaDataErrorCode.PROVIDER_REJECTED

    limited = AlpacaApiClient(
        AlpacaCredentials("fixture", "fixture"),
        transport=ScriptedTransport([HttpResponse(429, b"{}", now)] * 4),
        sleep=lambda _delay: None,
        monotonic=lambda: 0,
    )
    with pytest.raises(AlpacaDataError) as failure:
        limited.clock()
    assert failure.value.code is AlpacaDataErrorCode.RATE_LIMITED

    for token in ("", "x" * 4097):
        with pytest.raises(AlpacaDataError, match="page token"):
            malformed.bars(("AAPL",), now, now + timedelta(minutes=1), token)

    bar_transport = ScriptedTransport([HttpResponse(200, b'{"bars":{}}', now)])
    bar_client = AlpacaApiClient(
        AlpacaCredentials("fixture", "fixture"),
        transport=bar_transport,
        sleep=lambda _delay: None,
        monotonic=lambda: 0,
    )
    bar_client.bars(("AAPL",), now, now + timedelta(minutes=1), "next")
    assert ("page_token", "next") in bar_transport.parameters[0]


def test_authorization_is_exact_content_bound_and_tamper_detected(
    tmp_path: Path,
) -> None:
    root = tmp_path / "data"
    approval, policy = _authorization(root)
    snapshot = load_ticker_universe(UnresolvedInstrumentResolver())

    authorization = load_source_authorization(
        policy, approval, snapshot=snapshot, data_root=root
    )
    assert authorization.use_classification == "ACADEMIC"
    assert authorization.data_root == root

    approval.write_bytes(
        approval.read_bytes().replace(b'"feed":"iex"', b'"feed":"sip"')
    )
    with pytest.raises(AlpacaDataError, match="hash does not match") as failure:
        load_source_authorization(policy, approval, snapshot=snapshot, data_root=root)
    assert failure.value.code is AlpacaDataErrorCode.AUTHORIZATION_INVALID


def test_authorization_creation_and_scope_fail_closed(tmp_path: Path) -> None:
    future = datetime(2026, 10, 11, tzinfo=UTC)
    approval = tmp_path / "approval.json"
    policy = tmp_path / "policy.json"
    with pytest.raises(AlpacaDataError, match="operator_id"):
        prepare_academic_authorization(
            operator_id="invalid operator",
            data_root=tmp_path,
            approval_path=approval,
            policy_path=policy,
            expires_at_utc=future,
            example_policy_path=EXAMPLE_POLICY,
        )
    with pytest.raises(AlpacaDataError, match="not UTC"):
        prepare_academic_authorization(
            operator_id="operator",
            data_root=tmp_path,
            approval_path=approval,
            policy_path=policy,
            expires_at_utc=future.replace(tzinfo=None),
            example_policy_path=EXAMPLE_POLICY,
        )

    bad_example = tmp_path / "bad-example.json"
    bad_example.write_text("{", encoding="utf-8")
    with pytest.raises(AlpacaDataError, match="cannot be loaded"):
        prepare_academic_authorization(
            operator_id="operator",
            data_root=tmp_path,
            approval_path=approval,
            policy_path=policy,
            expires_at_utc=future,
            example_policy_path=bad_example,
        )

    array_example = tmp_path / "array-example.json"
    array_example.write_text("[]", encoding="utf-8")
    with pytest.raises(AlpacaDataError, match="malformed"):
        prepare_academic_authorization(
            operator_id="operator",
            data_root=tmp_path,
            approval_path=approval,
            policy_path=policy,
            expires_at_utc=future,
            example_policy_path=array_example,
        )

    object_sources = json.loads(EXAMPLE_POLICY.read_text(encoding="utf-8"))
    object_sources["sources"] = "not-an-array"
    object_sources_example = tmp_path / "object-sources-example.json"
    object_sources_example.write_text(json.dumps(object_sources), encoding="utf-8")
    with pytest.raises(AlpacaDataError, match="inventory is malformed"):
        prepare_academic_authorization(
            operator_id="operator",
            data_root=tmp_path,
            approval_path=approval,
            policy_path=policy,
            expires_at_utc=future,
            example_policy_path=object_sources_example,
        )

    source_missing = json.loads(EXAMPLE_POLICY.read_text(encoding="utf-8"))
    source_missing["sources"] = [
        item
        for item in source_missing["sources"]
        if item["source_id"] != "alpaca_iex_historical_bars"
    ]
    missing_example = tmp_path / "missing-example.json"
    missing_example.write_text(json.dumps(source_missing), encoding="utf-8")
    with pytest.raises(AlpacaDataError, match="does not contain"):
        prepare_academic_authorization(
            operator_id="operator",
            data_root=tmp_path,
            approval_path=approval,
            policy_path=policy,
            expires_at_utc=future,
            example_policy_path=missing_example,
        )


def test_authorization_wraps_policy_validator_failure(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    def reject_policy(_value: object) -> int:
        message = "injected rejection"
        raise source_policy_module.SourcePolicyError(message)

    monkeypatch.setattr(alpaca_module, "validate_policy", reject_policy)
    with pytest.raises(AlpacaDataError, match="derived source policy is invalid"):
        prepare_academic_authorization(
            operator_id="operator",
            data_root=tmp_path,
            approval_path=tmp_path / "approval.json",
            policy_path=tmp_path / "policy.json",
            expires_at_utc=datetime(2026, 10, 11, tzinfo=UTC),
            example_policy_path=EXAMPLE_POLICY,
        )


def test_source_authorization_rejects_malformed_or_invalid_evidence(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    root = tmp_path / "data"
    approval, policy = _authorization(root)
    snapshot = load_ticker_universe(UnresolvedInstrumentResolver())

    policy.write_bytes(b"{")
    with pytest.raises(AlpacaDataError, match="malformed JSON"):
        load_source_authorization(policy, approval, snapshot=snapshot, data_root=root)

    approval, policy = _authorization(root)

    def reject_policy(_value: object) -> int:
        message = "injected rejection"
        raise source_policy_module.SourcePolicyError(message)

    monkeypatch.setattr(alpaca_module, "validate_policy", reject_policy)
    with pytest.raises(AlpacaDataError, match="source policy rejected"):
        load_source_authorization(policy, approval, snapshot=snapshot, data_root=root)

    monkeypatch.setattr(alpaca_module, "validate_policy", lambda _value: 0)
    policy.write_text("[]", encoding="utf-8")
    approval.write_text("[]", encoding="utf-8")
    with pytest.raises(AlpacaDataError, match="must be JSON objects"):
        load_source_authorization(policy, approval, snapshot=snapshot, data_root=root)


def test_source_authorization_rejects_disabled_or_mismatched_scope(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    root = tmp_path / "data"
    approval, policy = _authorization(root)
    snapshot = load_ticker_universe(UnresolvedInstrumentResolver())
    monkeypatch.setattr(alpaca_module, "validate_policy", lambda _value: 0)

    policy_value = json.loads(policy.read_text(encoding="utf-8"))
    source = next(
        item
        for item in policy_value["sources"]
        if item["source_id"] == alpaca_module.SOURCE_ID
    )
    source["enabled"] = False
    alpaca_module._write_json(policy, policy_value)
    with pytest.raises(AlpacaDataError, match="exact Alpaca academic scope"):
        load_source_authorization(policy, approval, snapshot=snapshot, data_root=root)

    for malformed_sources in ("not-an-array", [1]):
        approval, policy = _authorization(root)
        policy_value = json.loads(policy.read_text(encoding="utf-8"))
        policy_value["sources"] = malformed_sources
        alpaca_module._write_json(policy, policy_value)
        with pytest.raises(AlpacaDataError, match="exact Alpaca academic scope"):
            load_source_authorization(
                policy, approval, snapshot=snapshot, data_root=root
            )

    approval, policy = _authorization(root)
    approval_value = json.loads(approval.read_text(encoding="utf-8"))
    approval_value.pop("document_sha256")
    approval_value["plan"] = "NOT_BASIC"
    approval_sha = alpaca_module._sha256(alpaca_module._canonical_bytes(approval_value))
    approval_value["document_sha256"] = approval_sha
    alpaca_module._write_json(approval, approval_value)
    policy_value = json.loads(policy.read_text(encoding="utf-8"))
    source = next(
        item
        for item in policy_value["sources"]
        if item["source_id"] == alpaca_module.SOURCE_ID
    )
    source["approval_record_sha256"] = approval_sha
    alpaca_module._write_json(policy, policy_value)
    with pytest.raises(AlpacaDataError, match="scope does not match"):
        load_source_authorization(policy, approval, snapshot=snapshot, data_root=root)


def test_source_authorization_expiry_universe_and_root_are_exact(
    tmp_path: Path,
) -> None:
    snapshot = load_ticker_universe(UnresolvedInstrumentResolver())
    valid = alpaca_module.SourceAuthorization(
        policy_sha256="0" * 64,
        approval_sha256="1" * 64,
        expires_at_utc=datetime(2099, 1, 1, tzinfo=UTC),
        universe_source_sha256=snapshot.source_file_sha256.hex(),
        data_root=tmp_path,
        use_classification="ACADEMIC",
    )
    valid.validate(snapshot, tmp_path)

    expired = alpaca_module.SourceAuthorization(
        valid.policy_sha256,
        valid.approval_sha256,
        datetime(2000, 1, 1, tzinfo=UTC),
        valid.universe_source_sha256,
        valid.data_root,
        valid.use_classification,
    )
    with pytest.raises(AlpacaDataError, match="expired"):
        expired.validate(snapshot, tmp_path)

    wrong_universe = alpaca_module.SourceAuthorization(
        valid.policy_sha256,
        valid.approval_sha256,
        valid.expires_at_utc,
        "f" * 64,
        valid.data_root,
        valid.use_classification,
    )
    with pytest.raises(AlpacaDataError, match=r"ticker\.txt"):
        wrong_universe.validate(snapshot, tmp_path)

    with pytest.raises(AlpacaDataError, match="data root"):
        valid.validate(snapshot, tmp_path / "different")


def test_calendar_and_task_partitioning_are_deterministic() -> None:
    sessions = parse_calendar(
        [{"date": item, "open": "09:30", "close": "16:00"} for item in PILOT_DATES]
    )
    tasks = make_tasks(tuple(f"T{index}" for index in range(9)), sessions)

    assert len(tasks) == 2
    assert tasks == make_tasks(tuple(f"T{index}" for index in range(9)), sessions)
    assert sessions[0].open_utc == datetime(2026, 9, 3, 13, 30, tzinfo=UTC)


@pytest.mark.parametrize(
    "payload",
    [
        {},
        ["not-an-object"],
        [{"date": "bad", "open": "09:30", "close": "16:00"}],
        [
            {"date": "2026-09-03", "open": "09:30", "close": "16:00"},
            {"date": "2026-09-03", "open": "09:30", "close": "16:00"},
        ],
    ],
)
def test_calendar_rejects_malformed_or_duplicate_sessions(payload: object) -> None:
    with pytest.raises(AlpacaDataError):
        parse_calendar(payload)


def test_calendar_time_bounds_and_two_year_boundary_are_explicit() -> None:
    for boundary in (
        alpaca_module.TradingSession(
            date(2026, 1, 2),
            datetime(2026, 1, 2, 14, 30, tzinfo=UTC),
            datetime(2026, 1, 2, 21, 0, tzinfo=UTC),
        ),
    ):
        assert boundary.session_date == date(2026, 1, 2)

    with pytest.raises(AlpacaDataError, match="boundaries"):
        alpaca_module.TradingSession(
            date(2026, 1, 2),
            datetime(2026, 1, 2, 14, 30, tzinfo=UTC),
            datetime(2026, 1, 2, 14, 30, tzinfo=UTC),
        )
    with pytest.raises(AlpacaDataError, match="not text"):
        alpaca_module._calendar_time(date(2026, 1, 2), None)
    with pytest.raises(AlpacaDataError, match="malformed"):
        alpaca_module._calendar_time(date(2026, 1, 2), "99:00")
    assert alpaca_module._two_year_start(date(2024, 2, 29)) == date(2022, 3, 1)


def test_run_session_resolution_requires_complete_two_year_calendar() -> None:
    now = datetime(2026, 9, 11, 14, 0, tzinfo=UTC)

    def client_for(rows: list[dict[str, str]]) -> AlpacaApiClient:
        responses = [
            HttpResponse(
                200,
                json.dumps({"timestamp": "2026-09-11T10:00:00-04:00"}).encode(),
                now,
            ),
            HttpResponse(200, json.dumps(rows).encode(), now),
        ]
        return AlpacaApiClient(
            AlpacaCredentials("fixture", "fixture"),
            transport=ScriptedTransport(responses),
            sleep=lambda _delay: None,
            monotonic=lambda: 0,
        )

    recent_only = [
        {"date": item, "open": "09:30", "close": "16:00"} for item in PILOT_DATES
    ]
    with pytest.raises(AlpacaDataError, match="requested boundary"):
        alpaca_module.resolve_run_sessions(client_for(recent_only), RunMode.BACKFILL)

    covered = [
        {"date": "2024-09-10", "open": "09:30", "close": "16:00"},
        {"date": "2024-09-11", "open": "09:30", "close": "16:00"},
        {"date": "2026-09-10", "open": "09:30", "close": "16:00"},
    ]
    latest, sessions, _, _ = alpaca_module.resolve_run_sessions(
        client_for(covered), RunMode.BACKFILL
    )
    assert latest == date(2026, 9, 10)
    assert tuple(item.session_date for item in sessions) == (
        date(2024, 9, 11),
        date(2026, 9, 10),
    )

    before_close = [
        HttpResponse(
            200,
            json.dumps({"timestamp": "2026-09-03T08:00:00-04:00"}).encode(),
            now,
        ),
        HttpResponse(200, json.dumps(recent_only[:1]).encode(), now),
    ]
    client = AlpacaApiClient(
        AlpacaCredentials("fixture", "fixture"),
        transport=ScriptedTransport(before_close),
        sleep=lambda _delay: None,
        monotonic=lambda: 0,
    )
    with pytest.raises(AlpacaDataError, match="no complete session"):
        alpaca_module.resolve_run_sessions(client, RunMode.PILOT)

    malformed_clock = AlpacaApiClient(
        AlpacaCredentials("fixture", "fixture"),
        transport=ScriptedTransport([HttpResponse(200, b"[]", now)]),
        sleep=lambda _delay: None,
        monotonic=lambda: 0,
    )
    with pytest.raises(AlpacaDataError, match="clock response is malformed"):
        alpaca_module.resolve_run_sessions(malformed_clock, RunMode.PILOT)

    only_four = [
        HttpResponse(
            200,
            json.dumps({"timestamp": "2026-09-11T10:00:00-04:00"}).encode(),
            now,
        ),
        HttpResponse(200, json.dumps(recent_only[:4]).encode(), now),
    ]
    short_client = AlpacaApiClient(
        AlpacaCredentials("fixture", "fixture"),
        transport=ScriptedTransport(only_four),
        sleep=lambda _delay: None,
        monotonic=lambda: 0,
    )
    with pytest.raises(AlpacaDataError, match="five complete pilot sessions"):
        alpaca_module.resolve_run_sessions(short_client, RunMode.PILOT)


def test_minute_bar_normalization_uses_exact_integer_units_and_session_time() -> None:
    session = parse_calendar(
        [{"date": "2026-09-03", "open": "09:30", "close": "16:00"}]
    )[0]
    parsed, timestamp_ns = _parse_rfc3339_ns("2026-09-03T13:30:00.123456789Z")
    assert parsed.microsecond == 123456
    assert timestamp_ns % 1_000_000_000 == 123456789

    row, disposition = _normalize_bar(
        "AAPL",
        {
            "c": "100.123456789",
            "h": "101.000000000",
            "l": "99.000000000",
            "o": "100.000000000",
            "t": "2026-09-03T13:30:00Z",
            "v": 42,
        },
        "source-" + "0" * 64,
        {session.session_date: session},
        {"AAPL": {"asset_id": "fixture-asset", "status": "RESOLVED"}},
    )

    assert disposition == "ACCEPTED"
    assert row is not None
    assert row["close_price_currency_nanos"] == 100_123_456_789
    assert row["volume_shares"] == 42
    assert row["price_unit"] == "USD_NANOS"
    assert row["quantity_unit"] == "SHARES"


def test_minute_bar_normalization_rejects_ambiguous_or_invalid_values() -> None:
    with pytest.raises(AlpacaDataError, match="RFC3339 UTC"):
        _parse_rfc3339_ns("2026-09-03T13:30:00+00:00")

    session = parse_calendar(
        [{"date": "2026-09-03", "open": "09:30", "close": "16:00"}]
    )[0]
    invalid_bar: dict[str, object] = {
        "c": 102,
        "h": 101,
        "l": 99,
        "o": 100,
        "t": "2026-09-03T13:30:00Z",
        "v": 1,
    }
    with pytest.raises(AlpacaDataError, match="OHLC invariants"):
        _normalize_bar(
            "AAPL",
            invalid_bar,
            "source-" + "0" * 64,
            {session.session_date: session},
            {"AAPL": {"asset_id": "fixture", "status": "RESOLVED"}},
        )

    invalid_bar.update({"c": 100, "h": 101, "v": 1.5})
    with pytest.raises(AlpacaDataError, match="nonnegative integer"):
        _normalize_bar(
            "AAPL",
            invalid_bar,
            "source-" + "0" * 64,
            {session.session_date: session},
            {"AAPL": {"asset_id": "fixture", "status": "RESOLVED"}},
        )


@pytest.mark.parametrize(
    "value",
    [True, object(), "not-a-number", "NaN", 0, "0.0000000001", 10_000_000_000],
)
def test_price_conversion_rejects_nonexact_nonpositive_or_overflow_values(
    value: object,
) -> None:
    with pytest.raises(AlpacaDataError):
        alpaca_module._price_nanos(value)


@pytest.mark.parametrize("value", [True, -1, 1.5, "1"])
def test_bar_count_rejects_noninteger_or_negative_values(value: object) -> None:
    with pytest.raises(AlpacaDataError):
        alpaca_module._bar_count(value)


@pytest.mark.parametrize(
    "payload",
    [
        [],
        {},
        {"bars": {1: []}},
        {"bars": {"AAPL": {}}},
        {"bars": {"AAPL": [1]}},
        {"bars": {}, "next_page_token": ""},
        {"bars": {}, "next_page_token": 1},
        {"bars": {}, "next_page_token": "x" * 4097},
    ],
)
def test_page_parser_rejects_malformed_shapes_and_tokens(payload: object) -> None:
    with pytest.raises(AlpacaDataError):
        alpaca_module._page_bars(payload)


def test_staged_payload_conflict_fails_before_publication(tmp_path: Path) -> None:
    payload = b"expected"
    digest = alpaca_module._sha256(payload)
    staged = tmp_path / f"tmp/alpaca-{digest}.partial"
    staged.parent.mkdir(parents=True)
    staged.write_bytes(b"conflict")
    repository = SimpleNamespace(root=tmp_path)

    with pytest.raises(AlpacaDataError, match="staged object conflicts"):
        alpaca_module._stage_payload(
            cast("DataRepository", repository),
            cast("AdmissionLease", object()),
            payload,
            "raw/final.json",
        )

    staged.write_bytes(payload)
    published: list[str] = []
    repository.publish_staged_object = lambda _staged, final, **_kwargs: (
        published.append(final)
    )
    assert alpaca_module._stage_payload(
        cast("DataRepository", repository),
        cast("AdmissionLease", object()),
        payload,
        "raw/final.json",
    ) == (digest, len(payload))
    assert published == ["raw/final.json"]


def test_source_page_loading_rejects_manifest_payload_and_json_faults(
    tmp_path: Path,
) -> None:
    repository = SimpleNamespace(
        root=tmp_path, load_manifest=lambda _identifier: object()
    )
    with pytest.raises(AlpacaDataError, match="incompatible source manifest"):
        alpaca_module._load_source_page(cast("DataRepository", repository), "bad")

    payload_path = tmp_path / "raw/fixture.json"
    payload_path.parent.mkdir(parents=True)
    payload_path.write_bytes(b"different")
    manifest = _source_manifest(b"expected")
    repository.load_manifest = lambda _identifier: manifest
    with pytest.raises(AlpacaDataError, match="does not match its manifest"):
        alpaca_module._load_source_page(cast("DataRepository", repository), "bad")

    malformed = b"{"
    payload_path.write_bytes(malformed)
    manifest = _source_manifest(malformed)
    repository.load_manifest = lambda _identifier: manifest
    with pytest.raises(AlpacaDataError, match="source page is malformed"):
        alpaca_module._load_source_page(cast("DataRepository", repository), "bad")


def test_partition_publication_detects_scope_session_and_duplicate_faults(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    session = parse_calendar(
        [{"date": "2026-09-03", "open": "09:30", "close": "16:00"}]
    )[0]
    task = alpaca_module.FetchTask(("AAPL",), (session,))
    manifest = _source_manifest(b"{}")
    repository = cast("DataRepository", SimpleNamespace(root=tmp_path))
    lease = cast("AdmissionLease", object())
    assets = {"AAPL": {"asset_id": "fixture", "status": "RESOLVED"}}

    monkeypatch.setattr(
        alpaca_module,
        "_load_source_page",
        lambda _repository, _identifier: (
            manifest,
            {
                "bars": {
                    "MSFT": [
                        {
                            "c": 100,
                            "h": 101,
                            "l": 99,
                            "o": 100,
                            "t": "2026-09-03T13:30:00Z",
                            "v": 1,
                        }
                    ]
                },
                "next_page_token": None,
            },
        ),
    )
    with pytest.raises(AlpacaDataError, match="out-of-scope symbol"):
        alpaca_module._publish_partitions(
            repository,
            lease,
            task=task,
            manifest_ids=(manifest.manifest_id,),
            universe_sha256="1" * 64,
            assets=assets,
        )

    outside = {
        "c": 100,
        "h": 101,
        "l": 99,
        "o": 100,
        "t": "2026-09-03T12:00:00Z",
        "v": 1,
    }
    monkeypatch.setattr(
        alpaca_module,
        "_load_source_page",
        lambda _repository, _identifier: (
            manifest,
            {"bars": {"AAPL": [outside]}, "next_page_token": None},
        ),
    )
    missing = alpaca_module._publish_partitions(
        repository,
        lease,
        task=task,
        manifest_ids=(manifest.manifest_id,),
        universe_sha256="1" * 64,
        assets=assets,
    )
    assert missing.out_of_session == 1
    assert missing.missing_pairs == ["AAPL:2026-09-03"]

    first = {**outside, "t": "2026-09-03T13:30:00Z"}
    conflicting = {**first, "c": 101}
    monkeypatch.setattr(
        alpaca_module,
        "_load_source_page",
        lambda _repository, _identifier: (
            manifest,
            {"bars": {"AAPL": [first, first, conflicting]}, "next_page_token": None},
        ),
    )
    with pytest.raises(AlpacaDataError, match="conflicting duplicate"):
        alpaca_module._publish_partitions(
            repository,
            lease,
            task=task,
            manifest_ids=(manifest.manifest_id,),
            universe_sha256="1" * 64,
            assets=assets,
        )


def test_normalization_marks_out_of_session_and_unresolved_instrument() -> None:
    session = parse_calendar(
        [{"date": "2026-09-03", "open": "09:30", "close": "16:00"}]
    )[0]
    base: dict[str, object] = {
        "c": 100,
        "h": 101,
        "l": 99,
        "o": 100,
        "t": "2026-09-03T12:00:00Z",
        "v": 1,
    }
    row, disposition = _normalize_bar(
        "AAPL",
        base,
        "source-" + "0" * 64,
        {session.session_date: session},
        {},
    )
    assert row is None
    assert disposition == "OUT_OF_SESSION"

    base["t"] = "2026-09-03T13:30:00.000000001Z"
    with pytest.raises(AlpacaDataError, match="minute aligned"):
        _normalize_bar(
            "AAPL",
            base,
            "source-" + "0" * 64,
            {session.session_date: session},
            {},
        )

    base["t"] = "2026-09-03T13:30:00Z"
    row, disposition = _normalize_bar(
        "AAPL",
        base,
        "source-" + "0" * 64,
        {session.session_date: session},
        {},
    )
    assert disposition == "ACCEPTED"
    assert row is not None
    assert row["instrument_id"] is None
    assert row["instrument_resolution"] == "UNRESOLVED"


def test_asset_not_found_is_explicitly_unsupported() -> None:
    transport = FakeAlpacaTransport(missing_asset="AAPL")
    client = AlpacaApiClient(
        AlpacaCredentials("fixture", "fixture"),
        transport=transport,
        sleep=lambda _seconds: None,
        monotonic=lambda: 0.0,
    )

    status = _asset_status(client, "AAPL")

    assert status["status"] == "UNSUPPORTED"
    assert status["reason"] == "PROVIDER_ASSET_NOT_FOUND"


@pytest.mark.parametrize(
    ("payload", "message"),
    [
        ([], "not an object"),
        ({"symbol": "MSFT"}, "does not match"),
        ({"symbol": "AAPL", "id": "id"}, "identity fields"),
    ],
)
def test_asset_resolution_rejects_malformed_provider_identity(
    payload: object, message: str
) -> None:
    now = datetime(2026, 9, 11, tzinfo=UTC)
    client = AlpacaApiClient(
        AlpacaCredentials("fixture", "fixture"),
        transport=ScriptedTransport(
            [HttpResponse(200, json.dumps(payload).encode(), now)]
        ),
        sleep=lambda _delay: None,
        monotonic=lambda: 0,
    )
    with pytest.raises(AlpacaDataError, match=message):
        _asset_status(client, "AAPL")


def test_asset_resolution_marks_inactive_and_otc_as_unsupported() -> None:
    now = datetime(2026, 9, 11, tzinfo=UTC)
    for asset in (
        {
            "class": "us_equity",
            "exchange": "NASDAQ",
            "id": "id",
            "status": "inactive",
            "symbol": "AAPL",
        },
        {
            "class": "us_equity",
            "exchange": "OTC",
            "id": "id",
            "status": "active",
            "symbol": "AAPL",
        },
    ):
        client = AlpacaApiClient(
            AlpacaCredentials("fixture", "fixture"),
            transport=ScriptedTransport(
                [HttpResponse(200, json.dumps(asset).encode(), now)]
            ),
            sleep=lambda _delay: None,
            monotonic=lambda: 0,
        )
        assert _asset_status(client, "AAPL")["status"] == "UNSUPPORTED"


def test_five_session_pilot_is_bounded_manifested_and_accepted(tmp_path: Path) -> None:
    root = tmp_path / "data"
    approval, policy = _authorization(root)
    secrets = _secrets(tmp_path / "secrets/.keys")
    transport = FakeAlpacaTransport()

    report = execute_run(
        mode=RunMode.PILOT,
        data_root=root,
        policy_path=policy,
        approval_path=approval,
        secrets_path=secrets,
        transport=transport,
    )

    assert report["status"] == "COMPLETED"
    assert report["feed"] == FEED
    assert report["adjustment"] == ADJUSTMENT
    assert report["session_count"] == 5
    counts = report["counts"]
    assert isinstance(counts, dict)
    assert counts["requested_symbols"] == 79
    assert counts["canonical_records"] == 79 * 5
    assert counts["missing_symbol_session_pairs"] == 0
    assert report["network_access_performed"] is True
    assert report["live_trading_capable"] is False
    human_report = (root / "reports/alpaca-iex-minute/pilot/report.md").read_text(
        encoding="utf-8"
    )
    assert "Unsupported symbols: `none`" in human_report
    assert "Feed: `IEX`" in human_report

    report_path = root / "reports/alpaca-iex-minute/pilot/report.json"
    verification = verify_run(
        report_path,
        data_root=root,
        sample_seed=DEFAULT_SAMPLE_SEED,
        sample_size=16,
    )
    assert verification["corruption_scan_passed"] is True
    assert verification["sampled_partition_count"] == 16

    acceptance_path = root / "reports/alpaca-iex-minute/pilot/acceptance.json"
    acceptance = accept_pilot(
        report_path, data_root=root, acceptance_path=acceptance_path
    )
    assert acceptance["accepted"] is True
    assert acceptance["repository_verification_passed"] is True

    snapshot = load_ticker_universe(UnresolvedInstrumentResolver())
    authorization = load_source_authorization(
        policy, approval, snapshot=snapshot, data_root=root
    )
    loaded_acceptance = alpaca_module._load_pilot_acceptance(
        acceptance_path,
        authorization=authorization,
        snapshot=snapshot,
    )
    assert loaded_acceptance["accepted"] is True

    repeated, _ = _load_hashed_json(acceptance_path, "pilot acceptance")
    assert repeated["document_sha256"] == acceptance["document_sha256"]

    idempotent = execute_run(
        mode=RunMode.PILOT,
        data_root=root,
        policy_path=policy,
        approval_path=approval,
        secrets_path=secrets,
        transport=FakeAlpacaTransport(),
    )
    assert idempotent["document_sha256"] == report["document_sha256"]


def test_backfill_execution_uses_accepted_assets_and_completed_checkpoints(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    acceptance = _backfill_acceptance()
    _patch_backfill_dependencies(monkeypatch, acceptance)

    def task_statistics(
        _client: object,
        _repository: object,
        _lease: object,
        *,
        task: alpaca_module.FetchTask,
        **_kwargs: object,
    ) -> alpaca_module.TaskStatistics:
        return alpaca_module.TaskStatistics(
            partition_manifest_ids=[f"partition-{task.task_id}{'1' * 40}"]
        )

    monkeypatch.setattr(alpaca_module, "_execute_task", task_statistics)
    first_root = tmp_path / "first"
    approval, policy = _authorization(first_root)
    report = execute_run(
        mode=RunMode.BACKFILL,
        data_root=first_root,
        policy_path=policy,
        approval_path=approval,
        secrets_path=_secrets(tmp_path / "first.keys"),
        pilot_acceptance_path=tmp_path / "accepted.json",
        transport=cast("alpaca_module.AlpacaTransport", object()),
    )
    assert report["mode"] == "BACKFILL"
    assert cast("dict[str, object]", report["counts"])["tasks"] == 10

    second_root = tmp_path / "second"
    approval, policy = _authorization(second_root)
    snapshot = load_ticker_universe(UnresolvedInstrumentResolver())
    tasks = make_tasks(
        snapshot.canonical_symbols,
        (
            parse_calendar([{"date": "2026-09-10", "open": "09:30", "close": "16:00"}])[
                0
            ],
        ),
    )
    monkeypatch.setattr(
        alpaca_module,
        "_load_checkpoint",
        lambda *_args, **_kwargs: {
            "active_task": None,
            "completed_task_ids": [task.task_id for task in tasks],
            "mode": "BACKFILL",
        },
    )
    monkeypatch.setattr(
        alpaca_module,
        "_load_task_statistics",
        lambda path: alpaca_module.TaskStatistics(
            partition_manifest_ids=[
                f"partition-{alpaca_module._sha256(str(path).encode())}"
            ]
        ),
    )
    monkeypatch.setattr(
        alpaca_module,
        "_execute_task",
        lambda *_args, **_kwargs: pytest.fail("completed task was re-executed"),
    )
    resumed = execute_run(
        mode=RunMode.BACKFILL,
        data_root=second_root,
        policy_path=policy,
        approval_path=approval,
        secrets_path=_secrets(tmp_path / "second.keys"),
        pilot_acceptance_path=tmp_path / "accepted.json",
        transport=cast("alpaca_module.AlpacaTransport", object()),
    )
    assert cast("dict[str, object]", resumed["counts"])["tasks"] == 10


@pytest.mark.parametrize(
    "assets",
    ["not-an-object", {"AAPL": {"status": "RESOLVED"}}],
)
def test_backfill_rejects_malformed_or_incomplete_pilot_asset_map(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, assets: object
) -> None:
    acceptance = _backfill_acceptance()
    acceptance["assets"] = assets
    _patch_backfill_dependencies(monkeypatch, acceptance)
    root = tmp_path / "data"
    approval, policy = _authorization(root)
    with pytest.raises(AlpacaDataError, match=r"asset map|asset coverage"):
        execute_run(
            mode=RunMode.BACKFILL,
            data_root=root,
            policy_path=policy,
            approval_path=approval,
            secrets_path=_secrets(tmp_path / "keys"),
            transport=cast("alpaca_module.AlpacaTransport", object()),
        )


def test_backfill_wraps_storage_rejection_and_refuses_report_replacement(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    _patch_backfill_dependencies(monkeypatch, _backfill_acceptance())
    root = tmp_path / "rejected"
    approval, policy = _authorization(root)

    def reject_admission(*_args: object, **_kwargs: object) -> Never:
        message = "injected storage rejection"
        raise StorageError(StorageErrorCode.ADMISSION_DENIED, message)

    monkeypatch.setattr(DataRepository, "acquire_admission", reject_admission)
    with pytest.raises(AlpacaDataError, match="storage admission rejected"):
        execute_run(
            mode=RunMode.BACKFILL,
            data_root=root,
            policy_path=policy,
            approval_path=approval,
            secrets_path=_secrets(tmp_path / "rejected.keys"),
            transport=cast("alpaca_module.AlpacaTransport", object()),
        )

    monkeypatch.undo()
    _patch_backfill_dependencies(monkeypatch, _backfill_acceptance())
    conflict_root = tmp_path / "conflict"
    approval, policy = _authorization(conflict_root)
    report_path = alpaca_module._report_path(conflict_root, RunMode.BACKFILL)
    alpaca_module._write_json(
        report_path,
        {
            "plan_sha256": "different",
            "schema_version": "1.0.0",
            "status": "COMPLETED",
        },
    )
    with pytest.raises(AlpacaDataError, match="different immutable run report"):
        execute_run(
            mode=RunMode.BACKFILL,
            data_root=conflict_root,
            policy_path=policy,
            approval_path=approval,
            secrets_path=_secrets(tmp_path / "conflict.keys"),
            transport=cast("alpaca_module.AlpacaTransport", object()),
        )


def test_run_verifier_rejects_report_and_repository_failures(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    report = _verification_report([])
    report["status"] = "RUNNING"
    monkeypatch.setattr(
        alpaca_module,
        "_load_hashed_json",
        lambda _path, _label: (report, "f" * 64),
    )
    with pytest.raises(AlpacaDataError, match="not complete and non-live"):
        verify_run(tmp_path / "report.json", data_root=tmp_path)

    report["status"] = "COMPLETED"
    FakeVerificationRepository.verification_passed = False
    monkeypatch.setattr(alpaca_module, "DataRepository", FakeVerificationRepository)
    with pytest.raises(AlpacaDataError, match="repository manifest"):
        verify_run(tmp_path / "report.json", data_root=tmp_path)
    FakeVerificationRepository.verification_passed = True


def test_run_verifier_rejects_manifest_and_coverage_faults(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    report = _verification_report([])
    monkeypatch.setattr(
        alpaca_module,
        "_load_hashed_json",
        lambda _path, _label: (report, "f" * 64),
    )
    monkeypatch.setattr(alpaca_module, "DataRepository", FakeVerificationRepository)
    FakeVerificationRepository.verification_passed = True

    report["partition_manifest_ids"] = "bad"
    with pytest.raises(AlpacaDataError, match="coverage fields are malformed"):
        verify_run(tmp_path / "report.json", data_root=tmp_path)

    report["partition_manifest_ids"] = ["incompatible"]
    FakeVerificationRepository.manifests = {"incompatible": object()}
    with pytest.raises(AlpacaDataError, match="incompatible canonical partition"):
        verify_run(tmp_path / "report.json", data_root=tmp_path)

    first_payload = b"first"
    second_payload = b"second"
    first = _partition_manifest(first_payload)
    second = _partition_manifest(second_payload)
    report["partition_manifest_ids"] = [first.manifest_id, second.manifest_id]
    FakeVerificationRepository.manifests = {
        first.manifest_id: first,
        second.manifest_id: second,
    }
    with pytest.raises(AlpacaDataError, match="duplicate canonical partition key"):
        verify_run(tmp_path / "report.json", data_root=tmp_path)

    foreign = _partition_manifest(
        first_payload, partition_key="symbol=MSFT/session=2026-09-03"
    )
    report["partition_manifest_ids"] = [foreign.manifest_id]
    report["sessions"] = [{"date": "2026-09-03"}]
    FakeVerificationRepository.manifests = {foreign.manifest_id: foreign}
    with pytest.raises(AlpacaDataError, match="exact partition"):
        verify_run(tmp_path / "report.json", data_root=tmp_path)

    report["partition_manifest_ids"] = [first.manifest_id]
    report["sessions"] = [
        {"date": "2026-09-03"},
        {"date": "2026-09-04"},
    ]
    FakeVerificationRepository.manifests = {first.manifest_id: first}
    with pytest.raises(AlpacaDataError, match="coverage does not match"):
        verify_run(tmp_path / "report.json", data_root=tmp_path)


@pytest.mark.parametrize(
    ("sessions", "assets", "missing", "message"),
    [
        ([1], {"AAPL": {}}, [], "session coverage"),
        ([{}], {"AAPL": {}}, [], "session date"),
        ([{"date": "bad"}], {"AAPL": {}}, [], "session date"),
        ([{"date": "20260903"}], {"AAPL": {}}, [], "not canonical"),
        (
            [
                {"date": "2026-09-03"},
                {"date": "2026-09-03"},
            ],
            {"AAPL": {}},
            [],
            "duplicate session",
        ),
        ([{"date": "2026-09-03"}], {1: {}}, [], "asset coverage"),
        ([{"date": "2026-09-03"}], {"AAPL": {}}, [1], "missing-pair"),
        (
            [{"date": "2026-09-03"}],
            {"AAPL": {}},
            ["AAPL:2026-09-03:extra"],
            "missing-pair",
        ),
        (
            [{"date": "2026-09-03"}],
            {"AAPL": {}},
            ["MSFT:2026-09-03"],
            "invalid or duplicated",
        ),
        (
            [{"date": "2026-09-03"}],
            {"AAPL": {}},
            ["AAPL:2026-09-03", "AAPL:2026-09-03"],
            "invalid or duplicated",
        ),
    ],
)
def test_exact_coverage_key_validation_fails_closed(
    sessions: list[object],
    assets: dict[object, object],
    missing: list[object],
    message: str,
) -> None:
    with pytest.raises(AlpacaDataError, match=message):
        alpaca_module._expected_coverage_keys(sessions, assets, missing)

    expected, absent = alpaca_module._expected_coverage_keys(
        [{"date": "2026-09-03"}],
        {"AAPL": {}},
        ["AAPL:2026-09-03"],
    )
    assert expected == {"symbol=AAPL/session=2026-09-03"}
    assert absent == expected


@pytest.mark.parametrize(
    ("fault", "message"),
    [
        ("hash", "hash mismatch"),
        ("count", "record count mismatch"),
        ("json", "row is malformed"),
        ("schema", "incompatible schema"),
        ("duplicate", "duplicate rows"),
    ],
)
def test_run_verifier_rejects_sampled_partition_corruption(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    fault: str,
    message: str,
) -> None:
    valid_row = {
        "bar_start_exchange_event_time_ns": 1,
        "schema_version": alpaca_module.CANONICAL_SCHEMA_VERSION,
    }
    rows: list[object] = [valid_row]
    record_count = 1
    object_sha256: str | None = None
    if fault == "json":
        payload = b"{\n"
    else:
        if fault == "schema":
            rows = [{**valid_row, "schema_version": "incompatible"}]
        elif fault == "duplicate":
            rows = [valid_row, valid_row]
            record_count = 2
        payload = b"".join(alpaca_module._canonical_bytes(row) + b"\n" for row in rows)
    if fault == "count":
        record_count = 2
    elif fault == "hash":
        object_sha256 = "2" * 64
    manifest = _partition_manifest(
        payload,
        record_count=record_count,
        object_sha256=object_sha256,
    )
    destination = tmp_path / manifest.storage_path
    destination.parent.mkdir(parents=True)
    destination.write_bytes(payload)
    report = _verification_report([manifest.manifest_id])
    FakeVerificationRepository.manifests = {manifest.manifest_id: manifest}
    FakeVerificationRepository.verification_passed = True
    monkeypatch.setattr(alpaca_module, "DataRepository", FakeVerificationRepository)
    monkeypatch.setattr(
        alpaca_module,
        "_load_hashed_json",
        lambda _path, _label: (report, "f" * 64),
    )

    with pytest.raises(AlpacaDataError, match=message):
        verify_run(tmp_path / "report.json", data_root=tmp_path, sample_size=1)


def test_pilot_acceptance_rejects_nonpilot_report(tmp_path: Path) -> None:
    report_path = tmp_path / "report.json"
    alpaca_module._write_json(
        report_path,
        {"mode": "BACKFILL", "schema_version": "1.0.0"},
    )
    with pytest.raises(AlpacaDataError, match="report is not a pilot"):
        accept_pilot(
            report_path,
            data_root=tmp_path,
            acceptance_path=tmp_path / "acceptance.json",
        )


def test_backfill_projection_requires_positive_accepted_pilot_metrics() -> None:
    sessions = parse_calendar(
        [{"date": item, "open": "09:30", "close": "16:00"} for item in PILOT_DATES]
    )
    with pytest.raises(AlpacaDataError, match="accepted pilot"):
        _storage_projection(RunMode.BACKFILL, sessions, 79, None)

    output, temporary, basis = _storage_projection(
        RunMode.BACKFILL,
        sessions,
        79,
        {
            "pilot_metrics": {
                "canonical_bytes": 100_000,
                "canonical_records": 395,
                "raw_records": 395,
                "source_bytes": 50_000,
            }
        },
    )
    assert output >= 1_000_000_000
    assert temporary == 200_000_000
    assert basis["method"] == "PILOT_RATES_X_MAXIMUM_RECORDS_X_1_5"

    with pytest.raises(AlpacaDataError, match="lacks storage metrics"):
        _storage_projection(RunMode.BACKFILL, sessions, 79, {})
    with pytest.raises(AlpacaDataError, match="metrics are invalid"):
        _storage_projection(
            RunMode.BACKFILL,
            sessions,
            79,
            {
                "pilot_metrics": {
                    "canonical_bytes": 0,
                    "canonical_records": 1,
                    "raw_records": 1,
                    "source_bytes": 1,
                }
            },
        )


def test_checkpoint_and_task_report_validation_fail_closed(tmp_path: Path) -> None:
    missing = tmp_path / "missing.json"
    initial = alpaca_module._load_checkpoint(missing, RunMode.PILOT, "run", "plan")
    assert initial["active_task"] is None

    corrupt = tmp_path / "corrupt.json"
    alpaca_module._write_json(
        corrupt,
        {
            "active_task": None,
            "completed_task_ids": "not-a-list",
            "mode": "PILOT",
            "plan_sha256": "plan",
            "run_id": "run",
            "schema_version": "1.0.0",
        },
    )
    with pytest.raises(AlpacaDataError, match="does not match"):
        alpaca_module._load_checkpoint(corrupt, RunMode.PILOT, "run", "plan")

    malformed_statistics = tmp_path / "task.json"
    alpaca_module._write_json(
        malformed_statistics,
        {"schema_version": "1.0.0", "statistics": "bad"},
    )
    with pytest.raises(AlpacaDataError, match="malformed"):
        alpaca_module._load_task_statistics(malformed_statistics)

    missing_fields = tmp_path / "task-fields.json"
    alpaca_module._write_json(
        missing_fields,
        {"schema_version": "1.0.0", "statistics": {}},
    )
    with pytest.raises(AlpacaDataError, match="invalid fields"):
        alpaca_module._load_task_statistics(missing_fields)


def test_checkpoint_size_is_bounded(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(alpaca_module, "MAX_CHECKPOINT_BYTES", 1)
    with pytest.raises(AlpacaDataError, match="checkpoint exceeds"):
        alpaca_module._save_checkpoint(tmp_path / "checkpoint.json", {"value": 1})


def test_execute_task_follows_bounded_pagination(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    session = parse_calendar(
        [{"date": "2026-09-03", "open": "09:30", "close": "16:00"}]
    )[0]
    task = alpaca_module.FetchTask(("AAPL",), (session,))
    now = datetime(2026, 9, 11, tzinfo=UTC)
    transport = ScriptedTransport(
        [
            HttpResponse(
                200,
                json.dumps({"bars": {}, "next_page_token": "next"}).encode(),
                now,
            ),
            HttpResponse(
                200,
                json.dumps({"bars": {}, "next_page_token": None}).encode(),
                now,
            ),
        ]
    )
    client = AlpacaApiClient(
        AlpacaCredentials("fixture", "fixture"),
        transport=transport,
        sleep=lambda _delay: None,
        monotonic=lambda: 0,
    )
    monkeypatch.setattr(
        alpaca_module,
        "_publish_source_page",
        lambda *_args, page_index, **_kwargs: SimpleNamespace(
            manifest_id=f"source-{'1' * 62}{page_index:02d}"
        ),
    )
    monkeypatch.setattr(
        alpaca_module,
        "_publish_partitions",
        lambda *_args, **_kwargs: alpaca_module.TaskStatistics(),
    )
    checkpoint: dict[str, object] = {
        "active_task": None,
        "completed_task_ids": [],
        "mode": "PILOT",
    }

    statistics = alpaca_module._execute_task(
        client,
        cast("DataRepository", SimpleNamespace(root=tmp_path)),
        cast("AdmissionLease", object()),
        task=task,
        run_id="run",
        universe_sha256="1" * 64,
        assets={},
        checkpoint=checkpoint,
        checkpoint_path=tmp_path / "checkpoint.json",
    )

    assert statistics.canonical_records == 0
    assert checkpoint["active_task"] is None
    second_query = dict(transport.parameters[1])
    assert second_query["page_token"] == "next"  # noqa: S105


def test_pilot_acceptance_must_match_authorization_and_universe(tmp_path: Path) -> None:
    root = tmp_path / "data"
    approval, policy = _authorization(root)
    snapshot = load_ticker_universe(UnresolvedInstrumentResolver())
    authorization = load_source_authorization(
        policy, approval, snapshot=snapshot, data_root=root
    )
    acceptance_path = tmp_path / "acceptance.json"
    alpaca_module._write_json(
        acceptance_path,
        {
            "accepted": False,
            "mode": "PILOT",
            "schema_version": "1.0.0",
        },
    )
    with pytest.raises(AlpacaDataError, match="does not authorize"):
        alpaca_module._load_pilot_acceptance(
            acceptance_path,
            authorization=authorization,
            snapshot=snapshot,
        )


def test_execute_task_rejects_conflicting_and_oversized_checkpoint_state(
    tmp_path: Path,
) -> None:
    session = parse_calendar(
        [{"date": "2026-09-03", "open": "09:30", "close": "16:00"}]
    )[0]
    task = alpaca_module.FetchTask(("AAPL",), (session,))
    client = cast("AlpacaApiClient", object())
    repository = cast("DataRepository", object())
    lease = cast("AdmissionLease", object())
    checkpoint_path = tmp_path / "checkpoint.json"

    for active in (
        {"task_id": "different"},
        {
            "download_complete": "not-bool",
            "next_page_index": 0,
            "next_page_token": None,
            "source_manifest_ids": [],
            "task_id": task.task_id,
        },
    ):
        checkpoint: dict[str, object] = {
            "active_task": active,
            "completed_task_ids": [],
            "mode": "PILOT",
        }
        with pytest.raises(AlpacaDataError, match="checkpoint"):
            alpaca_module._execute_task(
                client,
                repository,
                lease,
                task=task,
                run_id="run",
                universe_sha256="0" * 64,
                assets={},
                checkpoint=checkpoint,
                checkpoint_path=checkpoint_path,
            )

    page_ids = [f"source-{'0' * 62}{index:02d}" for index in range(64)]
    checkpoint = {
        "active_task": {
            "download_complete": False,
            "next_page_index": 64,
            "next_page_token": "next",
            "source_manifest_ids": page_ids,
            "task_id": task.task_id,
        },
        "completed_task_ids": [],
        "mode": "PILOT",
    }
    with pytest.raises(AlpacaDataError, match="page bound"):
        alpaca_module._execute_task(
            client,
            repository,
            lease,
            task=task,
            run_id="run",
            universe_sha256="0" * 64,
            assets={},
            checkpoint=checkpoint,
            checkpoint_path=checkpoint_path,
        )


def test_execution_summary_is_bounded_and_reports_unsupported_symbols(
    tmp_path: Path,
) -> None:
    summary = _execution_summary(
        {
            "assets": {
                "AAPL": {"status": "RESOLVED"},
                "KRKNF": {"status": "UNSUPPORTED"},
            },
            "counts": {"requested_symbols": 2},
            "duration_seconds": 1.25,
            "end_date": "2026-09-10",
            "latest_complete_market_date": "2026-09-10",
            "mode": "PILOT",
            "provider_request_count": 3,
            "provider_retry_count": 0,
            "run_id": "fixture",
            "start_date": "2026-09-03",
            "status": "COMPLETED",
            "storage": {"source_bytes": 1},
        },
        data_root=tmp_path / "aegis",
    )

    assert "assets" not in summary
    assert summary["unsupported_symbols"] == ["KRKNF"]
    assert cast("str", summary["report_path"]).endswith("pilot/report.json")


def test_cli_routes_dry_run_authorization_execution_and_verification(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    assert alpaca_module.main(["--data-root", str(tmp_path), "pilot"]) == 0
    assert json.loads(capsys.readouterr().out)["dry_run"] is True

    approval = tmp_path / "approval.json"
    policy = tmp_path / "policy.json"
    assert (
        alpaca_module.main(
            [
                "--data-root",
                str(tmp_path),
                "authorize-academic-scope",
                "--operator-id",
                "fixture",
                "--expires-at-utc",
                "2099-01-01T00:00:00Z",
                "--approval-output",
                str(approval),
                "--policy-output",
                str(policy),
                "--accept-personal-noncommercial-terms",
            ]
        )
        == 0
    )
    assert json.loads(capsys.readouterr().out)["status"] == "AUTHORIZED"

    fake_report: dict[str, object] = {
        "assets": {"AAPL": {"status": "RESOLVED"}},
        "counts": {"requested_symbols": 1},
        "duration_seconds": 1,
        "end_date": "2026-09-10",
        "latest_complete_market_date": "2026-09-10",
        "mode": "PILOT",
        "provider_request_count": 1,
        "provider_retry_count": 0,
        "run_id": "run",
        "start_date": "2026-09-03",
        "status": "COMPLETED",
        "storage": {"source_bytes": 1},
    }

    def fake_execute(**_kwargs: object) -> dict[str, object]:
        return fake_report

    monkeypatch.setattr(alpaca_module, "execute_run", fake_execute)
    assert (
        alpaca_module.main(
            [
                "--data-root",
                str(tmp_path),
                "pilot",
                "--execute",
                "--policy",
                str(policy),
                "--approval",
                str(approval),
            ]
        )
        == 0
    )
    assert json.loads(capsys.readouterr().out)["mode"] == "PILOT"

    fake_report["mode"] = "BACKFILL"
    assert (
        alpaca_module.main(
            [
                "--data-root",
                str(tmp_path),
                "backfill",
                "--execute",
                "--pilot-acceptance",
                str(tmp_path / "accepted.json"),
            ]
        )
        == 0
    )
    assert json.loads(capsys.readouterr().out)["mode"] == "BACKFILL"

    monkeypatch.setattr(
        alpaca_module,
        "accept_pilot",
        lambda *_args, **_kwargs: {"accepted": True},
    )
    assert (
        alpaca_module.main(
            [
                "--data-root",
                str(tmp_path),
                "verify",
                "--report",
                str(tmp_path / "report.json"),
                "--accept-pilot",
            ]
        )
        == 0
    )
    assert json.loads(capsys.readouterr().out)["accepted"] is True

    monkeypatch.setattr(
        alpaca_module,
        "verify_run",
        lambda *_args, **_kwargs: {"corruption_scan_passed": True},
    )
    assert (
        alpaca_module.main(
            [
                "--data-root",
                str(tmp_path),
                "verify",
                "--report",
                str(tmp_path / "report.json"),
            ]
        )
        == 0
    )
    assert json.loads(capsys.readouterr().out)["corruption_scan_passed"] is True

    verification_output = tmp_path / "verification.json"
    assert (
        alpaca_module.main(
            [
                "--data-root",
                str(tmp_path),
                "verify",
                "--report",
                str(tmp_path / "report.json"),
                "--verification-output",
                str(verification_output),
            ]
        )
        == 0
    )
    persisted_verification = json.loads(capsys.readouterr().out)
    assert persisted_verification["corruption_scan_passed"] is True
    assert "document_sha256" in persisted_verification
    assert verification_output.exists()


def test_cli_failures_are_structured_and_cli_main_installs_signals(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    def fail_execute(**_kwargs: object) -> dict[str, object]:
        raise AlpacaDataError(AlpacaDataErrorCode.PLAN_INVALID, "safe failure")

    monkeypatch.setattr(alpaca_module, "execute_run", fail_execute)
    assert alpaca_module.main(["--data-root", str(tmp_path), "pilot", "--execute"]) == 1
    output = json.loads(capsys.readouterr().out)
    assert output["error"]["code"] == "PLAN_INVALID"

    signals: list[int] = []
    monkeypatch.setattr(
        signal,
        "signal",
        lambda number, _handler: signals.append(number),
    )
    monkeypatch.setattr(alpaca_module, "main", lambda: 7)
    assert alpaca_module.cli_main() == 7
    assert signals == [signal.SIGINT, signal.SIGTERM]


def test_interrupted_pilot_resumes_after_final_page_without_refetch(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    root = tmp_path / "data"
    approval, policy = _authorization(root)
    secrets = _secrets(tmp_path / "secrets/.keys")
    first_transport = FakeAlpacaTransport()
    original_publish = alpaca_module._publish_partitions

    def interrupt_after_final_page(*_args: object, **_kwargs: object) -> Never:
        raise AlpacaDataError(
            AlpacaDataErrorCode.NETWORK_FAILURE,
            "injected bounded interruption",
        )

    monkeypatch.setattr(
        alpaca_module, "_publish_partitions", interrupt_after_final_page
    )

    with pytest.raises(AlpacaDataError, match="injected bounded interruption"):
        execute_run(
            mode=RunMode.PILOT,
            data_root=root,
            policy_path=policy,
            approval_path=approval,
            secrets_path=secrets,
            transport=first_transport,
        )

    checkpoints = list((root / "tmp/checkpoints").glob("alpaca-iex-pilot-*.json"))
    assert len(checkpoints) == 1
    checkpoint = json.loads(checkpoints[0].read_text(encoding="utf-8"))
    assert len(checkpoint["completed_task_ids"]) == 0
    assert checkpoint["active_task"]["download_complete"] is True

    monkeypatch.setattr(alpaca_module, "_publish_partitions", original_publish)
    resumed_transport = FakeAlpacaTransport()

    report = execute_run(
        mode=RunMode.PILOT,
        data_root=root,
        policy_path=policy,
        approval_path=approval,
        secrets_path=secrets,
        transport=resumed_transport,
    )

    assert report["status"] == "COMPLETED"
    counts = cast("dict[str, object]", report["counts"])
    assert counts["canonical_records"] == 79 * 5
    assert first_transport.bar_calls + resumed_transport.bar_calls == 10
    assert not checkpoints[0].exists()
