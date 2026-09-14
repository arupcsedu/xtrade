"""Bounded FRED/ALFRED vintage ingestion and point-in-time tests."""

from __future__ import annotations

import json
import os
from dataclasses import replace
from datetime import UTC, date, datetime, timedelta
from email.message import Message
from io import BytesIO
from pathlib import Path
from typing import TYPE_CHECKING, Any, Self, cast
from urllib.error import HTTPError, URLError

import pytest
from aegis_mx_research import alfred
from aegis_mx_research.data_repository import (
    DataRepository,
    FileSystemState,
    QuotaEvidence,
    StorageError,
    StorageErrorCode,
)
from aegis_mx_research.ingestion import SecretValue

if TYPE_CHECKING:
    import urllib.request
    from collections.abc import Mapping, Sequence

NOW_NS = 1_800_000_000_000_000_000
RECEIPT_NS = 1_704_153_600_000_000_000
VINTAGE_START = date(2024, 1, 1)
VINTAGE_END = date(2024, 2, 29)
OBSERVATION_START = date(2023, 12, 1)
OBSERVATION_END = date(2024, 1, 1)


def _json_bytes(value: object) -> bytes:
    return json.dumps(value, separators=(",", ":"), sort_keys=True).encode()


def _config(**changes: object) -> alfred.AlfredConfig:
    values: dict[str, object] = {
        "vintage_start": VINTAGE_START,
        "vintage_end": VINTAGE_END,
        "observation_start": OBSERVATION_START,
        "observation_end": OBSERVATION_END,
        "series_ids": ("CPIAUCSL",),
        "page_limit": 100,
        "max_pages_per_endpoint": 5,
        "max_requests": 20,
        "requests_per_minute": 100,
        "retry_attempts": 3,
        "timeout_seconds": 10,
        "response_limit_bytes": alfred.MAX_RESPONSE_BYTES,
        "storage_limit_bytes": 999_000_000,
    }
    values.update(changes)
    return alfred.AlfredConfig(**cast("Any", values))


def _authorization(
    *,
    enabled: bool = True,
    expires_at_ns: int = NOW_NS + 1_000_000,
    storage_limit: int = 999_000_000,
) -> alfred.AlfredAuthorization:
    return alfred.AlfredAuthorization(
        approval_id="AEGIS-ALFRED-POC-TEST",
        allowed_series=tuple(sorted(item.series_id for item in alfred.APPROVED_SERIES)),
        valid_from=date(2000, 1, 1),
        valid_through=date(2030, 1, 1),
        expires_at_utc_ns=expires_at_ns,
        storage_limit_bytes=storage_limit,
        api_execution_authorized=enabled,
        approval_sha256="a" * 64,
    )


def _approval_body(**changes: object) -> dict[str, object]:
    body: dict[str, object] = {
        "allowed_series": sorted(item.series_id for item in alfred.APPROVED_SERIES),
        "api_execution_authorized": True,
        "approval_id": "AEGIS-ALFRED-POC-TEST",
        "contains_secrets": False,
        "derived_data_authorized": True,
        "distribution": "OWNER_ONLY_INTERNAL",
        "expires_at_utc": "2030-01-01T00:00:00+00:00",
        "model_training_authorized": True,
        "persistent_storage_authorized": True,
        "schema_version": alfred.ALFRED_SCHEMA_VERSION,
        "source_id": alfred.ALFRED_SOURCE_ID,
        "storage_limit_bytes_decimal": 999_000_000,
        "use_classification": "ACADEMIC_NON_COMMERCIAL",
        "valid_from": "2000-01-01",
        "valid_through": "2030-01-01",
    }
    body.update(changes)
    return body


def _approval_file(path: Path, **changes: object) -> Path:
    body = _approval_body(**changes)
    body["approval_sha256"] = alfred.approval_document_hash(body)
    path.write_text(json.dumps(body, sort_keys=True))
    path.chmod(0o600)
    return path


def _response(
    value: object, *, status: int = 200, receipt: int = RECEIPT_NS
) -> alfred.AlfredHttpResponse:
    return alfred.AlfredHttpResponse(status, _json_bytes(value), receipt, {})


def _metadata_document(**changes: object) -> dict[str, object]:
    spec = alfred.reviewed_series("CPIAUCSL")
    row: dict[str, object] = {
        "frequency": spec.frequency,
        "frequency_short": "M",
        "id": spec.series_id,
        "last_updated": "2024-02-15 12:00:00+00:00",
        "notes": "Federal statistical series.",
        "observation_end": "2024-01-01",
        "observation_start": "1947-01-01",
        "popularity": 95,
        "realtime_end": "2024-02-29",
        "realtime_start": "2024-01-01",
        "seasonal_adjustment": spec.seasonal_adjustment,
        "seasonal_adjustment_short": "SA",
        "title": spec.title,
        "units": spec.units,
        "units_short": "Index",
    }
    row.update(changes)
    return {
        "realtime_end": "2024-02-29",
        "realtime_start": "2024-01-01",
        "seriess": [row],
    }


def _release_document(**changes: object) -> dict[str, object]:
    row: dict[str, object] = {
        "id": 10,
        "name": "Consumer Price Index",
        "press_release": True,
        "realtime_end": "2024-02-29",
        "realtime_start": "2024-01-01",
    }
    row.update(changes)
    return {
        "realtime_end": "2024-02-29",
        "realtime_start": "2024-01-01",
        "releases": [row],
    }


def _release_dates_document(
    rows: Sequence[Mapping[str, object]] | None = None,
    *,
    count: int | None = None,
    offset: int = 0,
    limit: int = 100,
) -> dict[str, object]:
    actual = (
        list(rows)
        if rows is not None
        else [
            {"date": "2024-01-11", "release_id": 10},
            {"date": "2024-02-13", "release_id": 10},
        ]
    )
    return {
        "count": len(actual) if count is None else count,
        "limit": limit,
        "offset": offset,
        "order_by": "release_date",
        "realtime_end": "2024-02-29",
        "realtime_start": "2024-01-01",
        "release_dates": actual,
        "sort_order": "asc",
    }


def _observation(
    value: str,
    realtime_start: str,
    realtime_end: str,
    *,
    observation_date: str = "2023-12-01",
) -> dict[str, object]:
    return {
        "date": observation_date,
        "realtime_end": realtime_end,
        "realtime_start": realtime_start,
        "value": value,
    }


def _observations_document(
    rows: Sequence[Mapping[str, object]] | None = None,
    *,
    count: int | None = None,
    offset: int = 0,
    limit: int = 100,
) -> dict[str, object]:
    actual = (
        list(rows)
        if rows is not None
        else [
            _observation("306.7", "2024-01-11", "2024-02-12"),
            _observation("306.8", "2024-02-13", "9999-12-31"),
            _observation(
                ".", "2024-01-11", "9999-12-31", observation_date="2024-01-01"
            ),
        ]
    )
    return {
        "count": len(actual) if count is None else count,
        "file_type": "json",
        "limit": limit,
        "observation_end": "2024-01-01",
        "observation_start": "2023-12-01",
        "observations": actual,
        "offset": offset,
        "order_by": "observation_date",
        "output_type": 1,
        "realtime_end": "2024-02-29",
        "realtime_start": "2024-01-01",
        "sort_order": "asc",
        "units": "lin",
    }


def _transport(
    *,
    metadata: object | None = None,
    release: object | None = None,
    release_dates: object | None = None,
    observations: object | None = None,
) -> alfred.ScriptedAlfredTransport:
    return alfred.ScriptedAlfredTransport(
        {
            alfred.ALFRED_METADATA_ENDPOINT: [
                _response(_metadata_document() if metadata is None else metadata)
            ],
            alfred.ALFRED_RELEASE_ENDPOINT: [
                _response(_release_document() if release is None else release)
            ],
            alfred.ALFRED_RELEASE_DATES_ENDPOINT: [
                _response(
                    _release_dates_document()
                    if release_dates is None
                    else release_dates
                )
            ],
            alfred.ALFRED_OBSERVATIONS_ENDPOINT: [
                _response(
                    _observations_document() if observations is None else observations
                )
            ],
        }
    )


def _client(
    transport: alfred.AlfredTransport | None = None,
    *,
    config: alfred.AlfredConfig | None = None,
    sleeps: list[float] | None = None,
) -> alfred.AlfredClient:
    recorded = sleeps if sleeps is not None else []
    return alfred.AlfredClient(
        config or _config(),
        SecretValue("a" * 32),
        transport or _transport(),
        monotonic_ns=lambda: 0,
        sleeper=recorded.append,
    )


def _snapshot() -> alfred.AlfredSeriesSnapshot:
    config = _config()
    plan = alfred.build_query_plans(config)[0]
    fetched = _client(config=config).fetch_series(plan)
    return alfred.build_series_snapshot(
        plan, fetched, processing_time_utc_ns=RECEIPT_NS + 1
    )


def _repository(
    tmp_path: Path, *, storage_limit: int = 999_000_000
) -> alfred.AlfredRepository:
    repository = DataRepository(
        tmp_path / "data",
        filesystem_probe=lambda _path: FileSystemState(
            500_000_000_000, 300_000_000_000
        ),
        git_worktree=None,
    )
    repository.initialize()
    return alfred.AlfredRepository(
        repository,
        QuotaEvidence(
            limit_bytes=10_995_116_277_760,
            used_bytes=608_990_093_312,
            source="/opt/rci/bin/hdquota -s",
            observed_at_utc="2026-09-11T00:12:13.122793+00:00",
            authoritative=True,
        ),
        _authorization(storage_limit=storage_limit),
        series_universe_hash=alfred.series_universe_sha256(("CPIAUCSL",)),
    )


def test_reviewed_allowlist_and_restricted_pmi_are_explicit() -> None:
    expected = {
        "CPIAUCSL",
        "PPIACO",
        "PAYEMS",
        "GDP",
        "RSAFS",
        "DFEDTARU",
        "DFEDTARL",
    }
    assert {item.series_id for item in alfred.APPROVED_SERIES} == expected
    assert all(
        item.rights_status is alfred.SeriesRightsStatus.APPROVED_PUBLIC_DOMAIN
        for item in alfred.APPROVED_SERIES
    )
    with pytest.raises(alfred.AlfredError, match="copyrighted") as captured:
        alfred.reviewed_series("NAPM")
    assert captured.value.code is alfred.AlfredErrorCode.RESTRICTED_SERIES
    with pytest.raises(alfred.AlfredError, match="no rights review"):
        alfred.reviewed_series("UNREVIEWED")


def test_query_plan_is_deterministic_raw_and_bounded() -> None:
    config = _config()
    first = alfred.build_query_plans(config)
    second = alfred.build_query_plans(config)
    assert first == second
    assert first[0].plan_id.startswith("alfred-plan-")
    assert len(alfred.series_universe_sha256(config.series_ids)) == 64
    assert first[0].page_limit == 100


@pytest.mark.parametrize(
    "changes",
    [
        {"vintage_start": date(2024, 3, 1)},
        {"observation_start": date(2024, 2, 1)},
        {"vintage_start": date(2000, 1, 1)},
        {"observation_start": date(1900, 1, 1)},
        {"series_ids": ()},
        {"series_ids": ("CPIAUCSL", "CPIAUCSL")},
        {"page_limit": 0},
        {"max_pages_per_endpoint": 0},
        {"max_requests": 0},
        {"requests_per_minute": 121},
        {"retry_attempts": 4},
        {"timeout_seconds": 61},
        {"response_limit_bytes": 0},
        {"storage_limit_bytes": 1_000_000_000},
    ],
)
def test_configuration_rejects_unbounded_values(changes: Mapping[str, object]) -> None:
    with pytest.raises(alfred.AlfredError) as captured:
        _config(**changes)
    assert captured.value.code is alfred.AlfredErrorCode.INVALID_CONFIGURATION


def test_owner_only_self_hashed_authorization(tmp_path: Path) -> None:
    path = _approval_file(tmp_path / "approval.json")
    loaded = alfred.AlfredAuthorization.load(path)
    loaded.require(_config(), NOW_NS)
    assert loaded.api_execution_authorized
    assert loaded.allowed_series == tuple(sorted(loaded.allowed_series))


@pytest.mark.parametrize(
    "changes",
    [
        {"contains_secrets": True},
        {"distribution": "PUBLIC"},
        {"persistent_storage_authorized": False},
        {"model_training_authorized": False},
        {"derived_data_authorized": False},
        {"schema_version": "2.0.0"},
        {"source_id": "wrong"},
        {"allowed_series": ["NAPM"]},
        {"allowed_series": ["GDP", "CPIAUCSL"]},
        {"expires_at_utc": "not-a-date"},
        {"expires_at_utc": "2027-01-01T00:00:00"},
        {"storage_limit_bytes_decimal": 1_000_000_000},
        {"approval_id": "x"},
        {"valid_from": "2031-01-01"},
    ],
)
def test_authorization_rejects_invalid_scope(
    tmp_path: Path, changes: Mapping[str, object]
) -> None:
    path = _approval_file(tmp_path / "approval.json", **changes)
    with pytest.raises(alfred.AlfredError) as captured:
        alfred.AlfredAuthorization.load(path)
    assert captured.value.code in {
        alfred.AlfredErrorCode.UNAUTHORIZED,
        alfred.AlfredErrorCode.RESTRICTED_SERIES,
    }


def test_authorization_file_safety_and_hash(tmp_path: Path) -> None:
    with pytest.raises(alfred.AlfredError):
        alfred.AlfredAuthorization.load(tmp_path / "missing")
    malformed = tmp_path / "malformed"
    malformed.write_text("{")
    malformed.chmod(0o600)
    with pytest.raises(alfred.AlfredError):
        alfred.AlfredAuthorization.load(malformed)
    path = _approval_file(tmp_path / "approval.json")
    path.chmod(0o644)
    with pytest.raises(alfred.AlfredError):
        alfred.AlfredAuthorization.load(path)
    path.chmod(0o600)
    body = json.loads(path.read_text())
    body["approval_id"] = "tampered"
    path.write_text(json.dumps(body))
    with pytest.raises(alfred.AlfredError, match="hash mismatch"):
        alfred.AlfredAuthorization.load(path)


@pytest.mark.parametrize(
    ("authorization", "config", "now"),
    [
        (_authorization(enabled=False), _config(), NOW_NS),
        (_authorization(expires_at_ns=NOW_NS), _config(), NOW_NS),
        (replace(_authorization(), valid_from=date(2024, 1, 2)), _config(), NOW_NS),
        (
            replace(_authorization(), valid_through=date(2024, 2, 28)),
            _config(),
            NOW_NS,
        ),
        (_authorization(storage_limit=1), _config(), NOW_NS),
        (replace(_authorization(), allowed_series=("GDP",)), _config(), NOW_NS),
    ],
)
def test_authorization_runtime_gate_fails_closed(
    authorization: alfred.AlfredAuthorization,
    config: alfred.AlfredConfig,
    now: int,
) -> None:
    with pytest.raises(alfred.AlfredError) as captured:
        authorization.require(config, now)
    assert captured.value.code is alfred.AlfredErrorCode.UNAUTHORIZED


def test_client_preserves_initial_release_revision_missing_value_and_times() -> None:
    config = _config()
    plan = alfred.build_query_plans(config)[0]
    client = _client(config=config)
    fetched = client.fetch_series(plan)
    snapshot = alfred.build_series_snapshot(
        plan, fetched, processing_time_utc_ns=RECEIPT_NS + 1
    )
    assert client.request_count == 4
    assert [item.vintage_kind for item in snapshot.vintages] == [
        alfred.AlfredVintageKind.INITIAL_RELEASE,
        alfred.AlfredVintageKind.REVISION,
        alfred.AlfredVintageKind.INITIAL_RELEASE,
    ]
    assert [item.value_microunits for item in snapshot.vintages] == [
        306_700_000,
        306_800_000,
        None,
    ]
    assert snapshot.vintages[0].local_receipt_time_utc_ns == RECEIPT_NS
    assert snapshot.vintages[0].processing_time_utc_ns == RECEIPT_NS + 1
    assert (
        snapshot.vintages[0].release_time_precision
        is alfred.ReleaseTimePrecision.DATE_ONLY
    )
    assert snapshot.vintages[0].to_dict()["release_time_utc_ns"] is None
    assert snapshot.vintages[-1].value_status is alfred.AlfredValueStatus.MISSING


def test_as_known_at_never_substitutes_future_revision() -> None:
    snapshot = _snapshot()
    store = alfred.AlfredVintageStore(snapshot.vintages)
    first_known = snapshot.vintages[0].known_at_utc_ns
    revision_known = snapshot.vintages[1].known_at_utc_ns
    before = store.latest_available_before(first_known, series_id="CPIAUCSL")
    initial = store.as_known_at(first_known, series_id="CPIAUCSL")
    revised = store.as_known_at(revision_known, series_id="CPIAUCSL")
    assert before == ()
    assert initial[0].value_microunits == 306_700_000
    assert revised[0].value_microunits == 306_800_000
    assert all(item.known_at_utc_ns <= revision_known for item in revised)
    with pytest.raises(alfred.AlfredError) as captured:
        store.require_no_future(snapshot.vintages, first_known)
    assert captured.value.code is alfred.AlfredErrorCode.FUTURE_LEAKAGE


def test_revision_queries_and_replay_are_deterministic() -> None:
    first = _snapshot()
    second = _snapshot()
    assert first.to_dict() == second.to_dict()
    assert first.snapshot_sha256 == second.snapshot_sha256
    store = alfred.AlfredVintageStore(first.vintages)
    revisions = store.revisions_after(first.vintages[0].known_at_utc_ns)
    assert revisions
    store.require_no_future(first.vintages[:1], first.vintages[0].known_at_utc_ns)


def test_conflicting_vintage_is_rejected() -> None:
    config = _config()
    plan = alfred.build_query_plans(config)[0]
    fetched = _client(config=config).fetch_series(plan)
    conflict = replace(fetched.observations[0], value_microunits=1)
    with pytest.raises(alfred.AlfredError) as captured:
        alfred.build_series_snapshot(
            plan,
            replace(fetched, observations=(*fetched.observations, conflict)),
            processing_time_utc_ns=RECEIPT_NS + 1,
        )
    assert captured.value.code is alfred.AlfredErrorCode.CONFLICTING_VINTAGE


def test_missing_release_date_is_explicitly_degraded() -> None:
    config = _config()
    plan = alfred.build_query_plans(config)[0]
    fetched = _client(config=config).fetch_series(plan)
    snapshot = alfred.build_series_snapshot(
        plan,
        replace(fetched, release_dates=(*fetched.release_dates, date(2024, 2, 20))),
        processing_time_utc_ns=RECEIPT_NS + 1,
    )
    report = alfred.AlfredRunReport(
        run_id="alfred-run-" + "a" * 64,
        series_universe_sha256=alfred.series_universe_sha256(("CPIAUCSL",)),
        vintage_start=VINTAGE_START,
        vintage_end=VINTAGE_END,
        observation_start=OBSERVATION_START,
        observation_end=OBSERVATION_END,
        generated_at_utc_ns=NOW_NS,
        network_access_performed=False,
        request_count=4,
        retry_count=0,
        snapshots=(snapshot,),
        manifest_ids=(),
        usage_before_bytes=0,
        usage_after_bytes=0,
        storage_limit_bytes=999_000_000,
    )
    assert report.to_dict()["data_quality"] == {
        "precise_intraday_release_times": 0,
        "release_time_precision": "DATE_ONLY_OR_NOT_AVAILABLE",
        "state": "DEGRADED",
    }
    assert (
        cast("dict[str, int]", report.to_dict()["coverage"])["missing_release_dates"]
        == 1
    )


def test_client_throttles_and_bounds_retries() -> None:
    sleeps: list[float] = []
    throttled = alfred.AlfredHttpResponse(429, b"{}", RECEIPT_NS, {"Retry-After": "2"})
    transport = _transport()
    transport._responses[alfred.ALFRED_METADATA_ENDPOINT].insert(0, throttled)
    client = _client(transport, sleeps=sleeps)
    client.fetch_series(alfred.build_query_plans(_config())[0])
    assert client.retry_count == 1
    assert client.request_count == 5
    assert 2.0 in sleeps
    assert all("api_key" not in parameters for _, parameters in transport.requests)


@pytest.mark.parametrize("status", [400, 423, 429, 500])
def test_http_failures_are_typed_and_bounded(status: int) -> None:
    attempts = 1 if status == 400 else 3
    transport = alfred.ScriptedAlfredTransport(
        {
            alfred.ALFRED_METADATA_ENDPOINT: [
                alfred.AlfredHttpResponse(status, b"{}", RECEIPT_NS, {})
                for _ in range(attempts)
            ]
        }
    )
    with pytest.raises(alfred.AlfredError) as captured:
        _client(transport).fetch_series(alfred.build_query_plans(_config())[0])
    assert captured.value.code in {
        alfred.AlfredErrorCode.PROVIDER_UNAVAILABLE,
        alfred.AlfredErrorCode.RATE_LIMITED,
        alfred.AlfredErrorCode.RETRY_EXHAUSTED,
    }


@pytest.mark.parametrize(
    "transport",
    [
        _transport(metadata=[]),
        _transport(metadata={"unexpected": []}),
        _transport(metadata=_metadata_document(units="Percent")),
        _transport(metadata=_metadata_document(notes="Copyright owner")),
        _transport(release=_release_document(name="Wrong")),
        _transport(
            release_dates=_release_dates_document([{"date": "bad", "release_id": 10}])
        ),
        _transport(
            observations=_observations_document(
                [_observation("NaN", "2024-01-11", "9999-12-31")]
            )
        ),
        _transport(
            observations=_observations_document(
                [_observation("1", "2025-01-01", "9999-12-31")]
            )
        ),
    ],
)
def test_malformed_or_semantically_changed_responses_fail_closed(
    transport: alfred.ScriptedAlfredTransport,
) -> None:
    with pytest.raises(alfred.AlfredError):
        _client(transport).fetch_series(alfred.build_query_plans(_config())[0])


def test_multipage_response_and_pagination_guard() -> None:
    first = _release_dates_document([{"date": "2024-01-11", "release_id": 10}], count=2)
    second = _release_dates_document(
        [{"date": "2024-02-13", "release_id": 10}], count=2, offset=1
    )
    transport = _transport()
    transport._responses[alfred.ALFRED_RELEASE_DATES_ENDPOINT] = [
        _response(first),
        _response(second),
    ]
    fetched = _client(transport).fetch_series(alfred.build_query_plans(_config())[0])
    assert fetched.release_dates == (date(2024, 1, 11), date(2024, 2, 13))
    assert len(fetched.pages) == 5

    stuck = _transport(
        release_dates=_release_dates_document([], count=1),
    )
    with pytest.raises(alfred.AlfredError) as captured:
        _client(stuck).fetch_series(alfred.build_query_plans(_config())[0])
    assert captured.value.code is alfred.AlfredErrorCode.PAGINATION_INVALID


class _FakeResponse:
    status = 200

    def __init__(self, body: bytes = b"{}") -> None:
        self.body = body
        self.headers = Message()

    def __enter__(self) -> Self:
        return self

    def __exit__(self, *_args: object) -> None:
        return None

    def read(self, _limit: int) -> bytes:
        return self.body


class _FakeOpener:
    def __init__(self, outcome: object) -> None:
        self.outcome = outcome
        self.last_url = ""

    def open(self, request: urllib.request.Request, *, timeout: int) -> _FakeResponse:
        assert timeout == 10
        self.last_url = request.full_url
        if isinstance(self.outcome, Exception):
            raise self.outcome
        return cast("_FakeResponse", self.outcome)


def test_real_transport_fixed_host_secret_containment_and_errors() -> None:
    transport = alfred.UrllibAlfredTransport(wall_clock_ns=lambda: NOW_NS)
    opener = _FakeOpener(_FakeResponse(b"{}"))
    transport._opener = cast("Any", opener)
    result = transport.get(
        alfred.ALFRED_METADATA_ENDPOINT,
        {"series_id": "CPIAUCSL"},
        SecretValue("z" * 32),
        10,
    )
    assert result.received_at_utc_ns == NOW_NS
    assert opener.last_url.startswith("https://api.stlouisfed.org/fred/series?")
    assert "api_key=" in opener.last_url
    with pytest.raises(alfred.AlfredError, match="endpoint"):
        transport.get("/evil", {}, SecretValue("z" * 32), 10)

    transport._opener = cast("Any", _FakeOpener(URLError("key=" + "z" * 32)))
    with pytest.raises(alfred.AlfredError) as captured:
        transport.get(alfred.ALFRED_METADATA_ENDPOINT, {}, SecretValue("z" * 32), 10)
    assert "z" * 32 not in str(captured.value)


def test_real_transport_http_error_is_returned_for_client_policy() -> None:
    headers = Message()
    headers["Retry-After"] = "1"
    error = HTTPError(
        "https://api.stlouisfed.org/",
        429,
        "rate limited",
        headers,
        BytesIO(b"{}"),
    )
    transport = alfred.UrllibAlfredTransport(wall_clock_ns=lambda: NOW_NS)
    transport._opener = cast("Any", _FakeOpener(error))
    result = transport.get(
        alfred.ALFRED_METADATA_ENDPOINT, {}, SecretValue("z" * 32), 10
    )
    assert result.status == 429
    assert result.headers["Retry-After"] == "1"


def test_repository_atomic_immutable_publication_and_reports(tmp_path: Path) -> None:
    sink = _repository(tmp_path)
    adapter = alfred.AlfredAdapter(
        _config(),
        _authorization(),
        repository=sink,
        wall_clock_ns=lambda: NOW_NS,
    )
    report = adapter.run(_client())
    machine, human = alfred.write_reports(report, sink)
    assert machine.is_file()
    assert human.is_file()
    assert report.to_dict()["live_trading_capable"] is False
    assert report.network_access_performed is False
    manifests = list((sink.repository.root / "manifests/alfred").glob("*.json"))
    assert manifests
    first_usage = sink.usage_bytes()
    report_again = adapter.run(_client())
    assert report_again.run_id == report.run_id
    assert sink.usage_bytes() == first_usage


def test_repository_cap_and_authorization_binding_fail_closed(tmp_path: Path) -> None:
    sink = _repository(tmp_path, storage_limit=100)
    with pytest.raises(alfred.AlfredError) as captured:
        sink.publish(
            series_id="CPIAUCSL",
            plan_id="alfred-plan-" + "b" * 64,
            object_kind=alfred.AlfredObjectKind.OBSERVATIONS,
            payload=b"{}",
            record_count=1,
            event_time_min_ns=1,
            event_time_max_ns=1,
            revision_time_min_ns=1,
            revision_time_max_ns=1,
            receipt_time_utc_ns=1,
            processing_time_utc_ns=1,
        )
    assert captured.value.code is alfred.AlfredErrorCode.STORAGE_LIMIT

    normal = _repository(tmp_path / "other")
    adapter = alfred.AlfredAdapter(
        _config(),
        replace(_authorization(), approval_sha256="c" * 64),
        repository=normal,
        wall_clock_ns=lambda: NOW_NS,
    )
    with pytest.raises(alfred.AlfredError) as mismatch:
        adapter.run(_client())
    assert mismatch.value.code is alfred.AlfredErrorCode.UNAUTHORIZED


def test_cli_defaults_to_network_free_plan(capsys: pytest.CaptureFixture[str]) -> None:
    result = alfred.cli_main(
        [
            "ingest",
            "--vintage-start",
            "2024-01-01",
            "--vintage-end",
            "2024-02-29",
            "--observation-start",
            "2023-12-01",
            "--observation-end",
            "2024-01-01",
            "--series",
            "CPIAUCSL",
        ]
    )
    document = json.loads(capsys.readouterr().out)
    assert result == 0
    assert document["network_access_performed"] is False
    assert document["blocked_series"] == ["NAPM"]


def test_cli_execution_requires_each_external_gate(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    arguments = [
        "ingest",
        "--execute",
        "--vintage-start",
        "2024-01-01",
        "--vintage-end",
        "2024-02-29",
        "--observation-start",
        "2023-12-01",
        "--observation-end",
        "2024-01-01",
        "--series",
        "CPIAUCSL",
    ]
    with pytest.raises(alfred.AlfredError, match="approval"):
        alfred.cli_main(arguments)
    approval = _approval_file(tmp_path / "approval.json")
    with pytest.raises(alfred.AlfredError, match="quota"):
        alfred.cli_main([*arguments, "--approval", str(approval)])
    monkeypatch.delenv("AEGIS_FRED_API_KEY", raising=False)
    with pytest.raises(alfred.AlfredError, match="API_KEY"):
        alfred.cli_main(
            [
                *arguments,
                "--approval",
                str(approval),
                "--quota-limit-bytes",
                "10995116277760",
                "--quota-used-bytes",
                "608990093312",
                "--quota-source",
                "hdquota",
                "--quota-observed-at-utc",
                "2026-09-11T00:12:13+00:00",
                "--quota-authoritative",
            ]
        )


def test_schemas_are_closed_and_validate_representative_artifacts(
    tmp_path: Path,
) -> None:
    jsonschema = pytest.importorskip("jsonschema")
    root = Path(__file__).parents[2] / "schemas"
    snapshot = _snapshot()
    jsonschema.Draft202012Validator(
        json.loads((root / "alfred-series-snapshot-v1.schema.json").read_text()),
        format_checker=jsonschema.FormatChecker(),
    ).validate(snapshot.to_dict())
    approval = _approval_body()
    approval["approval_sha256"] = alfred.approval_document_hash(approval)
    jsonschema.Draft202012Validator(
        json.loads((root / "alfred-approval-v1.schema.json").read_text()),
        format_checker=jsonschema.FormatChecker(),
    ).validate(approval)
    sink = _repository(tmp_path)
    manifest = sink.publish(
        series_id="CPIAUCSL",
        plan_id=alfred.build_query_plans(_config())[0].plan_id,
        object_kind=alfred.AlfredObjectKind.CANONICAL_SNAPSHOT,
        payload=_json_bytes(snapshot.to_dict()),
        record_count=len(snapshot.vintages),
        event_time_min_ns=min(
            item.observation_time_utc_ns for item in snapshot.vintages
        ),
        event_time_max_ns=max(
            item.observation_time_utc_ns for item in snapshot.vintages
        ),
        revision_time_min_ns=min(item.known_at_utc_ns for item in snapshot.vintages),
        revision_time_max_ns=max(item.known_at_utc_ns for item in snapshot.vintages),
        receipt_time_utc_ns=RECEIPT_NS,
        processing_time_utc_ns=RECEIPT_NS + 1,
    )
    manifest_document = json.loads(manifest.encode())
    jsonschema.Draft202012Validator(
        json.loads((root / "alfred-artifact-manifest-v1.schema.json").read_text()),
        format_checker=jsonschema.FormatChecker(),
    ).validate(manifest_document)
    report = alfred.AlfredAdapter(
        _config(), _authorization(), wall_clock_ns=lambda: NOW_NS
    ).run(_client())
    jsonschema.Draft202012Validator(
        json.loads((root / "alfred-run-report-v1.schema.json").read_text()),
        format_checker=jsonschema.FormatChecker(),
    ).validate(report.to_dict())


def test_negative_and_fractional_native_values_are_exact() -> None:
    assert alfred._parse_micro_units("-1.234567") == (
        alfred.AlfredValueStatus.OBSERVED,
        -1_234_567,
    )
    assert alfred._parse_micro_units("0") == (
        alfred.AlfredValueStatus.OBSERVED,
        0,
    )
    with pytest.raises(alfred.AlfredError):
        alfred._parse_micro_units("1.0000001")


def test_release_dates_are_not_claimed_as_intraday_times() -> None:
    release = _snapshot().releases[0]
    assert release.to_dict()["release_time_precision"] == "DATE_ONLY"
    assert release.to_dict()["release_time_utc_ns"] is None
    expected = datetime(2024, 1, 12, tzinfo=UTC)
    epoch = datetime(1970, 1, 1, tzinfo=UTC)
    expected_ns = int((expected - epoch) / timedelta(microseconds=1)) * 1_000
    assert release.known_at_utc_ns == expected_ns


def test_no_trading_dependency_or_capability() -> None:
    source = Path(alfred.__file__).read_text()
    assert "aegis_mx_risk" not in source
    assert "aegis_mx_oms" not in source
    assert "gateway" not in source.casefold()
    assert _snapshot().to_dict()["live_trading_capable"] is False


@pytest.mark.parametrize("value", [None, "", "bad\x00value", "x" * 513])
def test_bounded_text_rejects_invalid_input(value: object) -> None:
    with pytest.raises(alfred.AlfredError):
        alfred._bounded_text(cast("str", value), "field")


def test_identifier_hash_and_time_helpers_fail_closed() -> None:
    with pytest.raises(alfred.AlfredError):
        alfred._require_sha256("bad", "hash")
    with pytest.raises(alfred.AlfredError):
        alfred._require_series_id("bad-id")
    with pytest.raises(alfred.AlfredError):
        alfred._require_series_id(cast("str", None))
    with pytest.raises(alfred.AlfredError):
        alfred._utc_ns(datetime.fromisoformat("2024-01-01T00:00:00"))
    with pytest.raises(alfred.AlfredError):
        alfred._utc_ns(datetime(1960, 1, 1, tzinfo=UTC))
    with pytest.raises(alfred.AlfredError):
        alfred._conservative_known_ns(date(9999, 12, 31))
    with pytest.raises(alfred.AlfredError):
        alfred._parse_date(1, "date")
    with pytest.raises(alfred.AlfredError):
        alfred._parse_date("invalid", "date")
    with pytest.raises(alfred.AlfredError):
        alfred._parse_updated_ns("invalid")
    with pytest.raises(alfred.AlfredError):
        alfred._parse_updated_ns("2024-01-01T00:00:00")
    with pytest.raises(alfred.AlfredError):
        alfred._parse_micro_units("9999999999999999")


def test_series_contract_rejects_invalid_fields() -> None:
    spec = alfred.reviewed_series("CPIAUCSL")
    with pytest.raises(alfred.AlfredError):
        replace(spec, title="")
    assert spec.to_dict()["series_id"] == "CPIAUCSL"
    assert alfred.series_universe_sha256(
        ("GDP", "CPIAUCSL")
    ) == alfred.series_universe_sha256(("CPIAUCSL", "GDP"))


def test_authorization_rejects_nonobject_unknown_fields_and_empty_series(
    tmp_path: Path,
) -> None:
    nonobject = tmp_path / "nonobject.json"
    nonobject.write_text("[]")
    nonobject.chmod(0o600)
    with pytest.raises(alfred.AlfredError, match="not an object"):
        alfred.AlfredAuthorization.load(nonobject)

    unknown = _approval_body(extra="field")
    unknown["approval_sha256"] = alfred.approval_document_hash(unknown)
    path = tmp_path / "unknown.json"
    path.write_text(json.dumps(unknown))
    path.chmod(0o600)
    with pytest.raises(alfred.AlfredError, match="fields"):
        alfred.AlfredAuthorization.load(path)

    for index, changes in enumerate(
        (
            {"allowed_series": []},
            {"api_execution_authorized": "yes"},
            {"storage_limit_bytes_decimal": True},
            {"valid_from": 1},
        )
    ):
        candidate = _approval_file(
            tmp_path / f"bad-{index}.json", **cast("dict[str, object]", changes)
        )
        with pytest.raises(alfred.AlfredError):
            alfred.AlfredAuthorization.load(candidate)


def test_authorization_rejects_links_directories_and_oversize(tmp_path: Path) -> None:
    directory = tmp_path / "directory"
    directory.mkdir()
    with pytest.raises(alfred.AlfredError):
        alfred.AlfredAuthorization.load(directory)
    target = _approval_file(tmp_path / "target.json")
    link = tmp_path / "link.json"
    link.symlink_to(target)
    with pytest.raises(alfred.AlfredError):
        alfred.AlfredAuthorization.load(link)
    oversized = tmp_path / "oversized.json"
    oversized.write_bytes(b"x" * (alfred.MAX_POLICY_BYTES + 1))
    oversized.chmod(0o600)
    with pytest.raises(alfred.AlfredError):
        alfred.AlfredAuthorization.load(oversized)


def test_record_and_snapshot_invariants_detect_tampering() -> None:
    snapshot = _snapshot()
    release = snapshot.releases[0]
    with pytest.raises(alfred.AlfredError):
        replace(release, release_id=0)
    with pytest.raises(alfred.AlfredError, match="hash mismatch"):
        replace(release, record_sha256="b" * 64)

    vintage = snapshot.vintages[0]
    invalid_changes: tuple[dict[str, object], ...] = (
        {"observation_time_utc_ns": 1},
        {"known_at_utc_ns": 1},
        {"revision_number": 0},
        {"local_receipt_time_utc_ns": 0},
        {"processing_time_utc_ns": 1},
        {"known_through_utc_ns": vintage.known_at_utc_ns},
        {"value_status": alfred.AlfredValueStatus.MISSING},
        {"revision_number": 2},
        {"release_date": None},
    )
    for changes in invalid_changes:
        with pytest.raises(alfred.AlfredError):
            replace(vintage, **cast("Any", changes))
    with pytest.raises(alfred.AlfredError):
        replace(vintage, source_request_id="")
    with pytest.raises(alfred.AlfredError, match="hash mismatch"):
        replace(vintage, record_sha256="b" * 64)
    with pytest.raises(alfred.AlfredError, match="incomplete"):
        replace(snapshot, vintages=())
    with pytest.raises(alfred.AlfredError, match="incomplete"):
        replace(snapshot, metadata=replace(snapshot.metadata, series_id="GDP"))
    with pytest.raises(alfred.AlfredError, match="duplicated"):
        replace(snapshot, source_request_ids=("same", "same"))


def test_vintage_store_rejects_duplicate_and_bad_query_times() -> None:
    snapshot = _snapshot()
    with pytest.raises(alfred.AlfredError, match="duplicate revision"):
        alfred.AlfredVintageStore((*snapshot.vintages, snapshot.vintages[0]))
    store = alfred.AlfredVintageStore(snapshot.vintages)
    with pytest.raises(alfred.AlfredError):
        store.as_known_at(0)
    with pytest.raises(alfred.AlfredError):
        store.revisions_after(0)
    assert store.as_known_at(NOW_NS)
    assert store.as_known_at(NOW_NS, series_id="GDP") == ()


def _raw_page(endpoint: str, document: object) -> alfred.AlfredRawPage:
    return alfred.AlfredRawPage(
        endpoint=endpoint,
        series_id="CPIAUCSL",
        offset=0,
        body=_json_bytes(document),
        received_at_utc_ns=RECEIPT_NS,
    )


def test_json_and_container_boundaries_are_strict() -> None:
    huge = replace(
        _raw_page(alfred.ALFRED_METADATA_ENDPOINT, {}),
        body=b"x" * (alfred.MAX_RESPONSE_BYTES + 1),
    )
    with pytest.raises(alfred.AlfredError):
        alfred._json_object(huge)
    for body in (b"{", b"\xff", b"[]"):
        with pytest.raises(alfred.AlfredError):
            alfred._json_object(
                replace(_raw_page(alfred.ALFRED_METADATA_ENDPOINT, {}), body=body)
            )
    with pytest.raises(alfred.AlfredError):
        alfred._closed_keys({}, {"required"})
    with pytest.raises(alfred.AlfredError):
        alfred._object_list({}, "items")
    with pytest.raises(alfred.AlfredError):
        alfred._object_list([1], "items")
    for value in (True, -1, alfred.MAX_INT64 + 1):
        with pytest.raises(alfred.AlfredError):
            alfred._integer(value, "integer")


def test_metadata_and_release_shape_guards() -> None:
    spec = alfred.reviewed_series("CPIAUCSL")

    def parse_metadata_and_time(document: object) -> None:
        page = _raw_page(alfred.ALFRED_METADATA_ENDPOINT, document)
        row, _ = alfred._parse_metadata(page, spec)
        alfred._parse_updated_ns(row["last_updated"])

    for document in (
        {**_metadata_document(), "seriess": []},
        {**_metadata_document(), "seriess": [1]},
        _metadata_document(notes=1),
        _metadata_document(notes="x" * (alfred.MAX_NOTES_BYTES + 1)),
        _metadata_document(last_updated="bad"),
        _metadata_document(last_updated="2024-01-01T00:00:00"),
    ):
        with pytest.raises(alfred.AlfredError):
            parse_metadata_and_time(document)
    for document in (
        {**_release_document(), "releases": []},
        _release_document(id=0),
        _release_document(press_release="yes"),
    ):
        with pytest.raises(alfred.AlfredError):
            alfred._parse_release(
                _raw_page(alfred.ALFRED_RELEASE_ENDPOINT, document), spec
            )


def test_release_date_validation_rejects_id_order_and_bounds() -> None:
    bad_documents = (
        _release_dates_document([{"date": "2024-01-11", "release_id": 11}]),
        _release_dates_document(
            [
                {"date": "2024-02-13", "release_id": 10},
                {"date": "2024-01-11", "release_id": 10},
            ]
        ),
        _release_dates_document([], count=alfred.MAX_ROWS_PER_SERIES + 1),
        _release_dates_document([], limit=alfred.MAX_PAGE_LIMIT + 1),
    )
    for document in bad_documents:
        with pytest.raises(alfred.AlfredError):
            alfred._parse_release_dates_page(
                _raw_page(alfred.ALFRED_RELEASE_DATES_ENDPOINT, document), 10
            )


@pytest.mark.parametrize(
    "changes",
    [
        {"file_type": "xml"},
        {"units": "chg"},
        {"output_type": 2},
        {"sort_order": "desc"},
    ],
)
def test_observation_semantics_cannot_silently_change(
    changes: Mapping[str, object],
) -> None:
    document = _observations_document()
    document.update(changes)
    with pytest.raises(alfred.AlfredError):
        alfred._parse_observations_page(
            _raw_page(alfred.ALFRED_OBSERVATIONS_ENDPOINT, document),
            alfred.build_query_plans(_config())[0],
        )


@pytest.mark.parametrize(
    "row",
    [
        _observation("1", "2024-01-11", "9999-12-31", observation_date="2022-01-01"),
        _observation("1", "2024-02-01", "2024-01-01"),
        _observation("1", "2023-01-01", "2023-12-31"),
        _observation("1", "2025-01-01", "9999-12-31"),
    ],
)
def test_observation_temporal_window_is_fail_closed(row: Mapping[str, object]) -> None:
    with pytest.raises(alfred.AlfredError) as captured:
        alfred._parse_observations_page(
            _raw_page(
                alfred.ALFRED_OBSERVATIONS_ENDPOINT,
                _observations_document([row]),
            ),
            alfred.build_query_plans(_config())[0],
        )
    assert captured.value.code is alfred.AlfredErrorCode.TIMESTAMP_INVALID


def test_observation_order_and_empty_series_are_rejected() -> None:
    rows = [
        _observation("1", "2024-02-01", "9999-12-31"),
        _observation("1", "2024-01-01", "2024-01-31"),
    ]
    with pytest.raises(alfred.AlfredError) as captured:
        alfred._parse_observations_page(
            _raw_page(
                alfred.ALFRED_OBSERVATIONS_ENDPOINT,
                _observations_document(rows),
            ),
            alfred.build_query_plans(_config())[0],
        )
    assert captured.value.code is alfred.AlfredErrorCode.PAGINATION_INVALID

    transport = _transport(observations=_observations_document([]))
    with pytest.raises(alfred.AlfredError, match="no observations"):
        _client(transport).fetch_series(alfred.build_query_plans(_config())[0])


def test_client_response_and_request_budgets() -> None:
    tiny_response = _response(_metadata_document())
    client = _client(
        alfred.ScriptedAlfredTransport(
            {alfred.ALFRED_METADATA_ENDPOINT: [tiny_response]}
        ),
        config=_config(response_limit_bytes=1),
    )
    with pytest.raises(alfred.AlfredError) as captured:
        client.fetch_series(alfred.build_query_plans(client.config)[0])
    assert captured.value.code is alfred.AlfredErrorCode.RESPONSE_TOO_LARGE

    client = _client(config=_config(max_requests=1))
    with pytest.raises(alfred.AlfredError, match="budget"):
        client.fetch_series(alfred.build_query_plans(client.config)[0])


def test_scripted_transport_exhaustion_and_exception() -> None:
    transport = alfred.ScriptedAlfredTransport({})
    with pytest.raises(alfred.AlfredError, match="unavailable"):
        transport.get("missing", {}, SecretValue("x"), 1)
    expected = alfred.AlfredError(alfred.AlfredErrorCode.PROVIDER_UNAVAILABLE, "x")
    transport = alfred.ScriptedAlfredTransport({"endpoint": [expected]})
    with pytest.raises(alfred.AlfredError) as captured:
        transport.get("endpoint", {}, SecretValue("x"), 1)
    assert captured.value is expected


def test_redirect_handler_refuses_redirect() -> None:
    with pytest.raises(alfred.AlfredError, match="redirect"):
        alfred._NoRedirect().redirect_request(
            cast("Any", None), None, 302, "found", None, "https://evil.example/"
        )


def test_transport_network_property_and_typed_redirect_exception() -> None:
    transport = alfred.UrllibAlfredTransport()
    assert transport.network_access
    expected = alfred.AlfredError(
        alfred.AlfredErrorCode.PROVIDER_UNAVAILABLE, "redirect refused"
    )
    transport._opener = cast("Any", _FakeOpener(expected))
    with pytest.raises(alfred.AlfredError) as captured:
        transport.get(alfred.ALFRED_METADATA_ENDPOINT, {}, SecretValue("z" * 32), 10)
    assert captured.value is expected


def test_release_pagination_page_count_offset_and_cross_page_order() -> None:
    plan = alfred.build_query_plans(_config())[0]
    page = _response(
        _release_dates_document([{"date": "2024-01-11", "release_id": 10}], count=2)
    )
    bound_transport = _transport()
    bound_transport._responses[alfred.ALFRED_RELEASE_DATES_ENDPOINT] = [page]
    with pytest.raises(alfred.AlfredError, match="page bound"):
        _client(bound_transport, config=_config(max_pages_per_endpoint=1)).fetch_series(
            plan
        )

    bad_offset = _transport(
        release_dates=_release_dates_document(
            [{"date": "2024-01-11", "release_id": 10}], offset=1
        )
    )
    with pytest.raises(alfred.AlfredError, match="pagination changed"):
        _client(bad_offset).fetch_series(plan)

    reverse = _transport()
    reverse._responses[alfred.ALFRED_RELEASE_DATES_ENDPOINT] = [
        _response(
            _release_dates_document([{"date": "2024-02-13", "release_id": 10}], count=2)
        ),
        _response(
            _release_dates_document(
                [{"date": "2024-01-11", "release_id": 10}], count=2, offset=1
            )
        ),
    ]
    with pytest.raises(alfred.AlfredError, match="conflict across pages"):
        _client(reverse).fetch_series(plan)


def test_observation_pagination_page_count_offset_progress_and_order() -> None:
    plan = alfred.build_query_plans(_config())[0]
    first_row = _observation("1", "2024-01-11", "2024-02-12")
    first_page = _response(_observations_document([first_row], count=2))
    bound = _transport()
    bound._responses[alfred.ALFRED_OBSERVATIONS_ENDPOINT] = [first_page]
    with pytest.raises(alfred.AlfredError, match="page bound"):
        _client(bound, config=_config(max_pages_per_endpoint=1)).fetch_series(plan)

    bad_offset = _transport(observations=_observations_document([first_row], offset=1))
    with pytest.raises(alfred.AlfredError, match="pagination changed"):
        _client(bad_offset).fetch_series(plan)

    no_progress = _transport(observations=_observations_document([], count=1))
    with pytest.raises(alfred.AlfredError, match="made no progress"):
        _client(no_progress).fetch_series(plan)

    reverse = _transport()
    reverse._responses[alfred.ALFRED_OBSERVATIONS_ENDPOINT] = [
        _response(
            _observations_document(
                [_observation("2", "2024-02-13", "9999-12-31")], count=2
            )
        ),
        _response(
            _observations_document(
                [_observation("1", "2024-01-11", "2024-02-12")],
                count=2,
                offset=1,
            )
        ),
    ]
    with pytest.raises(alfred.AlfredError, match="ordering conflicts"):
        _client(reverse).fetch_series(plan)


def test_overlapping_vintage_intervals_are_rejected() -> None:
    plan = alfred.build_query_plans(_config())[0]
    fetched = _client().fetch_series(plan)
    overlap = replace(fetched.observations[0], realtime_end=date(2024, 2, 14))
    with pytest.raises(alfred.AlfredError, match="overlap"):
        alfred.build_series_snapshot(
            plan,
            replace(fetched, observations=(overlap, *fetched.observations[1:])),
            processing_time_utc_ns=RECEIPT_NS + 1,
        )


def test_adapter_without_repository_is_a_pure_deterministic_transform() -> None:
    report = alfred.AlfredAdapter(
        _config(), _authorization(), wall_clock_ns=lambda: NOW_NS
    ).run(_client())
    assert report.manifest_ids == ()
    assert report.usage_before_bytes == 0
    assert report.usage_after_bytes == 0


def test_repository_counts_partials_and_rejects_unsafe_trees(tmp_path: Path) -> None:
    sink = _repository(tmp_path)
    partial = sink.repository.root / "tmp/alfred-test.partial"
    partial.write_bytes(b"123")
    (sink.repository.root / "tmp/unrelated.partial").write_bytes(b"not counted")
    assert sink.usage_bytes() == 3

    unsafe = _repository(tmp_path / "unsafe")
    raw = unsafe.repository.root / "raw/alfred"
    raw.mkdir(parents=True)
    fifo = raw / "fifo"
    os.mkfifo(fifo)
    with pytest.raises(alfred.AlfredError, match="tree is unsafe"):
        unsafe.usage_bytes()

    linked = _repository(tmp_path / "linked")
    linked_raw = linked.repository.root / "raw/alfred"
    linked_raw.mkdir(parents=True)
    (linked_raw / "target").mkdir()
    (linked_raw / "directory-link").symlink_to(linked_raw / "target")
    with pytest.raises(alfred.AlfredError, match="symlink"):
        linked.usage_bytes()

    root_linked = _repository(tmp_path / "root-link")
    real = root_linked.repository.root / "raw/real"
    real.mkdir(parents=True)
    (root_linked.repository.root / "raw/alfred").symlink_to(real)
    with pytest.raises(alfred.AlfredError, match="root is a symlink"):
        root_linked.usage_bytes()


def test_repository_storage_cap_and_global_admission_are_independent(
    tmp_path: Path,
) -> None:
    cap_sink = _repository(tmp_path / "cap", storage_limit=100)
    raw = cap_sink.repository.root / "raw/alfred"
    raw.mkdir(parents=True)
    (raw / "used").write_bytes(b"x" * 100)
    with pytest.raises(alfred.AlfredError, match="cap is exhausted"):
        cap_sink.usage_bytes()

    remaining_sink = _repository(tmp_path / "remaining", storage_limit=100)
    remaining_raw = remaining_sink.repository.root / "raw/alfred"
    remaining_raw.mkdir(parents=True)
    (remaining_raw / "used").write_bytes(b"x" * 99)
    with (
        pytest.raises(alfred.AlfredError, match="cap is exhausted"),
        remaining_sink.publication_batch(),
    ):
        pass

    unknown = _repository(tmp_path / "unknown")
    unknown.quota = QuotaEvidence(
        limit_bytes=None,
        used_bytes=None,
        source=None,
        observed_at_utc=None,
        authoritative=False,
    )
    with (
        pytest.raises(alfred.AlfredError, match="admission failed"),
        unknown.publication_batch(),
    ):
        pass

    unknown_publish = _repository(tmp_path / "unknown-publish")
    unknown_publish.quota = QuotaEvidence(
        limit_bytes=None,
        used_bytes=None,
        source=None,
        observed_at_utc=None,
        authoritative=False,
    )
    with pytest.raises(alfred.AlfredError, match="admission failed"):
        _publish_fixture(unknown_publish)


def _publish_fixture(
    sink: alfred.AlfredRepository, payload: bytes = b"{}"
) -> alfred.AlfredArtifactManifest:
    return sink.publish(
        series_id="CPIAUCSL",
        plan_id="alfred-plan-" + "b" * 64,
        object_kind=alfred.AlfredObjectKind.OBSERVATIONS,
        payload=payload,
        record_count=1,
        event_time_min_ns=1,
        event_time_max_ns=1,
        revision_time_min_ns=1,
        revision_time_max_ns=1,
        receipt_time_utc_ns=1,
        processing_time_utc_ns=1,
    )


@pytest.mark.parametrize(("payload", "record_count"), [(b"", 1), (b"{}", 0)])
def test_repository_rejects_invalid_publication(
    tmp_path: Path, payload: bytes, record_count: int
) -> None:
    sink = _repository(tmp_path)
    with pytest.raises(alfred.AlfredError, match="publication is invalid"):
        sink.publish(
            series_id="CPIAUCSL",
            plan_id="alfred-plan-" + "b" * 64,
            object_kind=alfred.AlfredObjectKind.OBSERVATIONS,
            payload=payload,
            record_count=record_count,
            event_time_min_ns=1,
            event_time_max_ns=1,
            revision_time_min_ns=1,
            revision_time_max_ns=1,
            receipt_time_utc_ns=1,
            processing_time_utc_ns=1,
        )


def test_repository_rechecks_cap_under_admission_fence(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    sink = _repository(tmp_path)
    usages = iter((0, sink.storage_limit_bytes))
    monkeypatch.setattr(sink, "usage_bytes", lambda: next(usages))
    with pytest.raises(alfred.AlfredError, match="concurrent"):
        _publish_fixture(sink)


def test_repository_translates_storage_publication_failure(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    sink = _repository(tmp_path)

    def fail(*_args: object, **_kwargs: object) -> None:
        raise StorageError(StorageErrorCode.IO_FAILURE, "injected")

    monkeypatch.setattr(sink.repository, "publish_staged_object", fail)
    with pytest.raises(alfred.AlfredError, match="object publication"):
        _publish_fixture(sink)


def test_repository_stage_and_manifest_conflicts_are_immutable(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    sink = _repository(tmp_path)
    stage = sink.repository.root / "tmp/alfred-stage.partial"
    alfred.AlfredRepository._stage(stage, b"same")
    alfred.AlfredRepository._stage(stage, b"same")
    with pytest.raises(alfred.AlfredError, match="conflicts"):
        alfred.AlfredRepository._stage(stage, b"different")

    manifest = _publish_fixture(sink)
    path = sink.repository.root / f"manifests/alfred/{manifest.manifest_id}.json"
    path.chmod(0o600)
    path.write_bytes(b"tampered")
    with pytest.raises(alfred.AlfredError, match="manifest conflicts"):
        sink._publish_manifest(manifest)

    fresh = replace(manifest, plan_id="alfred-plan-" + "c" * 64)
    monkeypatch.setattr(
        os,
        "link",
        lambda *_args, **_kwargs: (_ for _ in ()).throw(FileExistsError()),
    )
    with pytest.raises(alfred.AlfredError, match="appeared concurrently"):
        sink._publish_manifest(fresh)


def test_write_all_fails_on_zero_progress(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(os, "write", lambda _descriptor, _payload: 0)
    with pytest.raises(alfred.AlfredError, match="partial"):
        alfred._write_all(1, b"x")


def test_manifest_cleanup_tolerates_already_removed_stage(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    sink = _repository(tmp_path)
    snapshot = _snapshot()
    manifest = alfred.AlfredArtifactManifest(
        approval_id="approval",
        series_universe_sha256=alfred.series_universe_sha256(("CPIAUCSL",)),
        series_id="CPIAUCSL",
        plan_id=snapshot.plan_id,
        object_kind=alfred.AlfredObjectKind.CANONICAL_SNAPSHOT,
        storage_path="canonical/alfred/cpiaucsl/canonical_snapshot/"
        + "a" * 64
        + ".json",
        object_sha256="a" * 64,
        size_bytes=1,
        record_count=1,
        event_time_min_ns=1,
        event_time_max_ns=1,
        revision_time_min_ns=1,
        revision_time_max_ns=1,
        receipt_time_utc_ns=1,
        processing_time_utc_ns=1,
    )
    original_link = os.link

    def link_and_remove(source: Path, target: Path, *, follow_symlinks: bool) -> None:
        original_link(source, target, follow_symlinks=follow_symlinks)
        source.unlink()

    monkeypatch.setattr(os, "link", link_and_remove)
    sink._publish_manifest(manifest)


def test_cli_complete_offline_fixture_path(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    approval = _approval_file(tmp_path / "approval.json")
    monkeypatch.setenv("AEGIS_FRED_API_KEY", "a" * 32)
    real_client = alfred.AlfredClient
    monkeypatch.setattr(
        alfred,
        "DataRepository",
        lambda root: DataRepository(
            root,
            filesystem_probe=lambda _path: FileSystemState(
                500_000_000_000, 300_000_000_000
            ),
            git_worktree=None,
        ),
    )
    monkeypatch.setattr(
        alfred,
        "AlfredClient",
        lambda config, _key, _transport_value: real_client(
            config,
            SecretValue("a" * 32),
            _transport(),
            monotonic_ns=lambda: 0,
            sleeper=lambda _seconds: None,
        ),
    )
    result = alfred.cli_main(
        [
            "ingest",
            "--execute",
            "--vintage-start",
            "2024-01-01",
            "--vintage-end",
            "2024-02-29",
            "--observation-start",
            "2023-12-01",
            "--observation-end",
            "2024-01-01",
            "--series",
            "CPIAUCSL",
            "--approval",
            str(approval),
            "--data-root",
            str(tmp_path / "data"),
            "--quota-limit-bytes",
            "10995116277760",
            "--quota-used-bytes",
            "608990093312",
            "--quota-source",
            "hdquota",
            "--quota-observed-at-utc",
            "2026-09-11T00:12:13+00:00",
            "--quota-authoritative",
        ]
    )
    output = json.loads(capsys.readouterr().out)
    assert result == 0
    assert Path(output["machine_report"]).is_file()
    assert Path(output["human_report"]).is_file()
