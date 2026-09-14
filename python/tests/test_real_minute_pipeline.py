"""Tests for the bounded real-minute promotion pipeline."""

from __future__ import annotations

import hashlib
import json
import os
import signal
from datetime import UTC, datetime, timedelta
from types import SimpleNamespace
from typing import TYPE_CHECKING, cast

import aegis_mx_research.real_minute_pipeline as pipeline
import pytest
from aegis_mx_research.alpaca_historical import HttpResponse
from aegis_mx_research.canonical_minute import (
    CanonicalMinuteNormalizer,
    CanonicalMinuteSpool,
    MinuteNormalizationCode,
    MinuteNormalizationError,
    RawMinuteBar,
    SpoolIngestResult,
)
from aegis_mx_research.data_repository import (
    DataRepository,
    ManifestLineage,
    ManifestTimeRange,
    QuotaEvidence,
    SourceManifest,
    StorageRequest,
)
from aegis_mx_research.forecast_contracts import (
    UnresolvedInstrumentResolver,
    load_ticker_universe,
)
from aegis_mx_research.reference_data import alpaca_instrument_id

if TYPE_CHECKING:
    from collections.abc import Callable, Iterable, Mapping, Sequence
    from pathlib import Path


def _quota() -> QuotaEvidence:
    return QuotaEvidence(
        limit_bytes=10 * (1 << 40),
        used_bytes=1_000_000,
        source="test quota",
        observed_at_utc="2026-09-12T12:00:00Z",
        authoritative=True,
    )


def _parent(expires: datetime) -> SimpleNamespace:
    return SimpleNamespace(
        approval_sha256="11" * 32,
        policy_sha256="22" * 32,
        expires_at_utc=expires,
    )


def _backfill_body(source_id: str) -> dict[str, object]:
    universe = load_ticker_universe(UnresolvedInstrumentResolver())
    return {
        "assets": {
            "AAPL": {
                "asset_id": "fixture-asset-id",
                "exchange": "NASDAQ",
                "status": "RESOLVED",
            },
            "KRKNF": {
                "asset_id": "fixture-otc-id",
                "exchange": "OTC",
                "status": "UNSUPPORTED",
            },
        },
        "finished_at_utc": "2026-09-11T16:00:00Z",
        "mode": "BACKFILL",
        "sessions": [
            {
                "close_utc": "2026-09-10T20:00:00Z",
                "date": "2026-09-10",
                "open_utc": "2026-09-10T13:30:00Z",
            }
        ],
        "source_manifest_ids": [source_id],
        "start_date": "2026-09-10",
        "end_date": "2026-09-10",
        "status": "COMPLETED",
        "ticker_source_sha256": universe.source_file_sha256.hex(),
        "universe_snapshot_sha256": universe.universe_snapshot_sha256.hex(),
    }


class _ActionTransport:
    def __init__(self, responses: Sequence[bytes]) -> None:
        self.responses = list(responses)
        self.calls = 0

    def get(self, *_args: object, **_kwargs: object) -> HttpResponse:
        body = self.responses[self.calls]
        self.calls += 1
        return HttpResponse(200, body, datetime(2026, 9, 12, 16, tzinfo=UTC))


def _secrets(path: Path) -> Path:
    path.write_text(
        "AEGIS_ALPACA_PAPER_ENDPOINT=https://paper-api.alpaca.markets\n"
        "AEGIS_ALPACA_PAPER_KEY_ID=fixture\n"
        "AEGIS_ALPACA_PAPER_SECRET_KEY=fixture\n",
        encoding="utf-8",
    )
    path.chmod(0o600)
    return path


def _publish_bar_source(root: Path) -> SourceManifest:
    repository = DataRepository(root)
    repository.initialize()
    payload = json.dumps(
        {
            "bars": {
                "AAPL": [
                    {
                        "c": 100.01,
                        "h": 100.02,
                        "l": 99.99,
                        "n": 2,
                        "o": 100,
                        "t": "2026-09-10T13:30:00Z",
                        "v": 10,
                        "vw": 100.005,
                    }
                ]
            },
            "next_page_token": None,
        },
        separators=(",", ":"),
        sort_keys=True,
    ).encode()
    digest = hashlib.sha256(payload).hexdigest()
    universe = load_ticker_universe(UnresolvedInstrumentResolver())
    manifest = SourceManifest(
        source_name="alpaca_iex_historical_bars",
        source_version="v2-iex-1min-raw",
        source_object_id="fixture-page",
        storage_path=f"raw/alpaca-iex-minute/{digest}.json",
        object_sha256=digest,
        size_bytes=len(payload),
        record_count=1,
        schema_name="alpaca-market-data-v2-bars-response",
        schema_version="1.0.0",
        times=ManifestTimeRange(
            event_time_min_ns=1789047000000000000,
            event_time_max_ns=1789047000000000000,
            publication_time_min_ns=1789142400000000000,
            publication_time_max_ns=1789142400000000000,
            receive_time_min_ns=1789142400000000000,
            receive_time_max_ns=1789142400000000000,
            processing_time_min_ns=1789142400000000000,
            processing_time_max_ns=1789142400000000000,
            revision_time_min_ns=1789142400000000000,
            revision_time_max_ns=1789142400000000000,
        ),
        universe_snapshot_sha256=universe.universe_snapshot_sha256.hex(),
        lineage=ManifestLineage(),
    )
    staged = root / "tmp/bar.part"
    staged.write_bytes(payload)
    with repository.acquire_admission(
        StorageRequest("bar fixture", output_bytes=1_000_000, temporary_bytes=1_000),
        _quota(),
    ) as lease:
        repository.publish_staged_object(
            "tmp/bar.part",
            manifest.storage_path,
            expected_sha256=digest,
            expected_size_bytes=len(payload),
            lease=lease,
        )
        repository.publish_manifest(manifest, lease)
    return manifest


def test_real_minute_end_to_end_without_order_capability(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    root = tmp_path / "data"
    source = _publish_bar_source(root)
    backfill_path = root / "reports/backfill.json"
    pipeline._write_immutable_document(
        backfill_path, _backfill_body(source.manifest_id)
    )
    expiry = datetime.now(UTC) + timedelta(days=2)
    parent = _parent(expiry)
    monkeypatch.setattr(pipeline, "load_source_authorization", lambda *_a, **_k: parent)
    monkeypatch.setattr(pipeline, "_quota_evidence", lambda _root: _quota())
    authorization_path = root / "manifests/approvals/actions.json"
    authorization = pipeline.authorize_corporate_actions(
        data_root=root,
        parent_policy=tmp_path / "policy.json",
        parent_approval=tmp_path / "approval.json",
        output_path=authorization_path,
        operator_id="academic-operator",
        expires_at_utc=expiry,
    )
    assert authorization["live_trading_capable"] is False
    action_payload = json.dumps(
        {
            "corporate_actions": {
                "cash_dividends": [
                    {
                        "ex_date": "2026-09-10",
                        "id": "dividend-1",
                        "process_date": "2026-09-10",
                        "rate": 0.25,
                        "symbol": "AAPL",
                    }
                ],
                "forward_splits": [
                    {
                        "ex_date": "2026-09-10",
                        "id": "split-1",
                        "new_rate": 2,
                        "old_rate": 1,
                        "process_date": "2026-09-10",
                        "symbol": "AAPL",
                    }
                ],
            },
            "next_page_token": None,
        },
        separators=(",", ":"),
        sort_keys=True,
    ).encode()
    transport = _ActionTransport((action_payload,))
    action_report_path = root / "reports/actions.json"
    action_report = pipeline.download_corporate_actions(
        data_root=root,
        backfill_report_path=backfill_path,
        parent_policy=tmp_path / "policy.json",
        parent_approval=tmp_path / "approval.json",
        action_authorization=authorization_path,
        secrets_path=_secrets(tmp_path / ".keys"),
        output_report_path=action_report_path,
        transport=transport,
    )
    assert action_report["network_access_performed"] is True
    assert action_report["group_counts"] == {
        "cash_dividends": 1,
        "forward_splits": 1,
    }
    assert transport.calls == 1
    assert (
        pipeline.download_corporate_actions(
            data_root=root,
            backfill_report_path=backfill_path,
            parent_policy=tmp_path / "policy.json",
            parent_approval=tmp_path / "approval.json",
            action_authorization=authorization_path,
            secrets_path=tmp_path / "missing",
            output_report_path=action_report_path,
            transport=transport,
        )["document_sha256"]
        == action_report["document_sha256"]
    )
    promotion_path = root / "reports/promotion.json"
    promotion = pipeline.promote_backfill_to_parquet(
        data_root=root,
        backfill_report_path=backfill_path,
        action_report_path=action_report_path,
        output_report_path=promotion_path,
    )
    assert cast("dict[str, int]", promotion["counts"])["published_records"] == 1
    assert promotion["live_trading_capable"] is False
    verification = pipeline.verify_promotion(data_root=root, report_path=promotion_path)
    assert verification["status"] == "PASS"
    resolver = pipeline.AlpacaBackfillReferenceResolver(
        pipeline._read_document(backfill_path, "backfill"),
        action_report,
        DataRepository(root),
    )
    reference = resolver.resolve(
        "AAPL",
        event_time_ns=1789047000000000000,
        known_at_ns=resolver.processing_time_ns,
    )
    assert reference.instrument_id == alpaca_instrument_id("fixture-asset-id").hex()
    assert reference.corporate_action_ids == ("alpaca:forward_splits:split-1",)
    assert reference.tick_value_currency_nanos == 1


def test_action_and_reference_inputs_fail_closed(  # noqa: PLR0915
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    with pytest.raises(MinuteNormalizationError, match="unavailable"):
        pipeline._read_document(tmp_path / "missing", "fixture")
    target = tmp_path / "target.json"
    target.write_text("{}", encoding="utf-8")
    link = tmp_path / "link.json"
    link.symlink_to(target)
    with pytest.raises(MinuteNormalizationError, match="unsafe file metadata"):
        pipeline._read_document(link, "fixture")
    malformed = tmp_path / "malformed.json"
    malformed.write_text("[]", encoding="utf-8")
    with pytest.raises(MinuteNormalizationError, match="not a JSON object"):
        pipeline._read_document(malformed, "fixture")
    malformed.write_text("{", encoding="utf-8")
    with pytest.raises(MinuteNormalizationError, match="malformed"):
        pipeline._read_document(malformed, "fixture")
    bad_hash = tmp_path / "bad-hash.json"
    bad_hash.write_text('{"document_sha256":"bad","value":1}', encoding="utf-8")
    with pytest.raises(MinuteNormalizationError, match="document hash"):
        pipeline._read_document(bad_hash, "fixture")
    immutable = tmp_path / "immutable.json"
    first_hash = pipeline._write_immutable_document(immutable, {"value": 1})
    assert pipeline._write_immutable_document(immutable, {"value": 1}) == first_hash
    with pytest.raises(MinuteNormalizationError, match="different bytes"):
        pipeline._write_immutable_document(immutable, {"value": 2})
    with pytest.raises(MinuteNormalizationError, match="response is malformed"):
        pipeline._action_response({})
    with pytest.raises(MinuteNormalizationError, match="page token"):
        pipeline._action_response(
            {"corporate_actions": {}, "next_page_token": "x" * 4097}
        )
    with pytest.raises(MinuteNormalizationError, match="groups are malformed"):
        pipeline._action_response({"corporate_actions": {"splits": "not-an-array"}})
    assert pipeline._action_event_date({"process_date": "bad"}) is None
    assert pipeline._action_event_date({}) is None
    with pytest.raises(MinuteNormalizationError, match="timezone aware"):
        pipeline._utc_ns(datetime(2026, 1, 1))  # noqa: DTZ001
    with pytest.raises(MinuteNormalizationError, match="not text"):
        pipeline._parse_utc_ns(1, "fixture")
    with pytest.raises(MinuteNormalizationError, match="malformed"):
        pipeline._parse_utc_ns("not-a-time", "fixture")
    with pytest.raises(MinuteNormalizationError, match="UTC offset"):
        pipeline._parse_utc_ns("2026-01-01T00:00:00", "fixture")

    expiry = datetime.now(UTC) + timedelta(days=2)
    monkeypatch.setattr(
        pipeline,
        "load_source_authorization",
        lambda *_a, **_k: _parent(expiry),
    )
    with pytest.raises(MinuteNormalizationError, match="identity or expiry"):
        pipeline.authorize_corporate_actions(
            data_root=tmp_path,
            parent_policy=tmp_path / "policy",
            parent_approval=tmp_path / "approval",
            output_path=tmp_path / "actions.json",
            operator_id="",
            expires_at_utc=expiry,
        )
    valid_path = tmp_path / "valid-actions.json"
    pipeline.authorize_corporate_actions(
        data_root=tmp_path,
        parent_policy=tmp_path / "policy",
        parent_approval=tmp_path / "approval",
        output_path=valid_path,
        operator_id="operator",
        expires_at_utc=expiry,
    )
    wrong = pipeline._read_document(valid_path, "fixture")
    wrong.pop("document_sha256")
    wrong["endpoint"] = "GET https://example.invalid"
    wrong_path = tmp_path / "wrong-actions.json"
    pipeline._write_immutable_document(wrong_path, wrong)
    with pytest.raises(MinuteNormalizationError, match="scope does not match"):
        pipeline._validate_action_authorization(
            wrong_path,
            data_root=tmp_path,
            parent_policy=tmp_path / "policy",
            parent_approval=tmp_path / "approval",
        )
    expired = dict(wrong)
    expired["endpoint"] = "GET https://data.alpaca.markets/v1/corporate-actions"
    expired["expires_at_utc"] = "2020-01-01T00:00:00Z"
    expired_path = tmp_path / "expired-actions.json"
    pipeline._write_immutable_document(expired_path, expired)
    with pytest.raises(MinuteNormalizationError, match="expired"):
        pipeline._validate_action_authorization(
            expired_path,
            data_root=tmp_path,
            parent_policy=tmp_path / "policy",
            parent_approval=tmp_path / "approval",
        )
    message = pipeline.main(
        [
            "--data-root",
            str(tmp_path),
            "fetch-actions",
            "--secrets-file",
            str(tmp_path / "none"),
        ]
    )
    assert message == 0


def test_action_download_rejects_bad_scope_and_provider_payloads(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    root = tmp_path / "data"
    source = _publish_bar_source(root)
    expiry = datetime.now(UTC) + timedelta(days=2)
    monkeypatch.setattr(
        pipeline,
        "load_source_authorization",
        lambda *_a, **_k: _parent(expiry),
    )
    monkeypatch.setattr(pipeline, "_quota_evidence", lambda _root: _quota())
    authorization = root / "manifests/approvals/actions.json"
    pipeline.authorize_corporate_actions(
        data_root=root,
        parent_policy=tmp_path / "policy",
        parent_approval=tmp_path / "approval",
        output_path=authorization,
        operator_id="operator",
        expires_at_utc=expiry,
    )
    secrets = _secrets(tmp_path / ".keys")

    def run_with(body: dict[str, object], response: bytes, name: str) -> None:
        backfill = root / f"reports/{name}-backfill.json"
        pipeline._write_immutable_document(backfill, body)
        pipeline.download_corporate_actions(
            data_root=root,
            backfill_report_path=backfill,
            parent_policy=tmp_path / "policy",
            parent_approval=tmp_path / "approval",
            action_authorization=authorization,
            secrets_path=secrets,
            output_report_path=root / f"reports/{name}-actions.json",
            transport=_ActionTransport((response,)),
        )

    incomplete = _backfill_body(source.manifest_id)
    incomplete["status"] = "FAILED"
    with pytest.raises(MinuteNormalizationError, match="not complete"):
        run_with(incomplete, b"{}", "incomplete")
    changed = _backfill_body(source.manifest_id)
    changed["ticker_source_sha256"] = "33" * 32
    with pytest.raises(MinuteNormalizationError, match="universe changed"):
        run_with(changed, b"{}", "changed")
    for name, response, message in (
        (
            "record-type",
            b'{"corporate_actions":{"splits":[1]},"next_page_token":null}',
            "not an object",
        ),
        (
            "record-date",
            b'{"corporate_actions":{"splits":[{"id":"one"}]},"next_page_token":null}',
            "valid action date",
        ),
    ):
        with pytest.raises(MinuteNormalizationError, match=message):
            run_with(_backfill_body(source.manifest_id), response, name)

    monkeypatch.setattr(pipeline, "MAX_ACTION_PAGES", 1)
    continued = b'{"corporate_actions":{},"next_page_token":"another-page"}'
    with pytest.raises(MinuteNormalizationError, match="page bound"):
        run_with(_backfill_body(source.manifest_id), continued, "page-bound")


def test_atomic_failure_quota_and_staged_conflict(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    real_fsync = os.fsync
    monkeypatch.setattr(
        os,
        "fsync",
        lambda _descriptor: (_ for _ in ()).throw(OSError("injected fsync")),
    )
    with pytest.raises(OSError, match="injected fsync"):
        pipeline._write_immutable_document(tmp_path / "failed.json", {"value": 1})
    assert not (tmp_path / "failed.json").exists()
    monkeypatch.setattr(os, "fsync", real_fsync)

    real_statvfs = os.statvfs
    status = SimpleNamespace(f_bsize=1, f_blocks=12 * (1 << 40), f_bfree=11 * (1 << 40))
    monkeypatch.setattr(os, "statvfs", lambda _root: status)
    evidence = pipeline._quota_evidence(tmp_path)
    assert evidence.limit_bytes == 10 * (1 << 40)
    monkeypatch.setattr(os, "statvfs", real_statvfs)
    root = tmp_path / "data"
    repository = DataRepository(root)
    repository.initialize()
    payload = b"source"
    digest = hashlib.sha256(payload).hexdigest()
    staged = root / f"tmp/alpaca-reference-{digest}.partial"
    staged.write_bytes(b"different")
    with (
        repository.acquire_admission(
            StorageRequest("conflict", output_bytes=1_000_000, temporary_bytes=1_000),
            _quota(),
        ) as lease,
        pytest.raises(MinuteNormalizationError, match="conflicts"),
    ):
        pipeline._stage_and_publish(repository, lease, payload, "raw/fixture/source")
    staged.write_bytes(payload)
    with repository.acquire_admission(
        StorageRequest("matching", output_bytes=1_000_000, temporary_bytes=1_000),
        _quota(),
    ) as lease:
        published_digest, published_size = pipeline._stage_and_publish(
            repository, lease, payload, "raw/fixture/source"
        )
    assert (published_digest, published_size) == (digest, len(payload))


def test_resolver_rejects_unsupported_cutoff_and_session(
    tmp_path: Path,
) -> None:
    root = tmp_path / "data"
    source = _publish_bar_source(root)
    backfill = pipeline._document(_backfill_body(source.manifest_id))
    action_report = pipeline._document(
        {
            "finished_at_ns": 1789228800000000000,
            "source_manifest_ids": [],
        }
    )
    resolver = pipeline.AlpacaBackfillReferenceResolver(
        backfill, action_report, DataRepository(root)
    )
    with pytest.raises(MinuteNormalizationError) as captured:
        resolver.resolve(
            "KRKNF",
            event_time_ns=1789047000000000000,
            known_at_ns=resolver.processing_time_ns,
        )
    assert captured.value.code is MinuteNormalizationCode.MISSING_REFERENCE
    with pytest.raises(MinuteNormalizationError, match="predates"):
        resolver.resolve(
            "AAPL",
            event_time_ns=1789047000000000000,
            known_at_ns=1,
        )
    with pytest.raises(MinuteNormalizationError) as captured:
        resolver.resolve(
            "AAPL",
            event_time_ns=1788960600000000000,
            known_at_ns=resolver.processing_time_ns,
        )
    assert captured.value.code is MinuteNormalizationCode.OUTSIDE_SESSION
    with pytest.raises(MinuteNormalizationError, match="regular market session"):
        resolver.resolve(
            "AAPL",
            event_time_ns=1789046940000000000,
            known_at_ns=resolver.processing_time_ns,
        )


def test_resolver_rejects_malformed_reference_and_action_evidence(
    tmp_path: Path,
) -> None:
    root = tmp_path / "data"
    source = _publish_bar_source(root)
    repository = DataRepository(root)
    action_report = pipeline._document(
        {
            "finished_at_ns": 1789228800000000000,
            "source_manifest_ids": [],
        }
    )
    with pytest.raises(MinuteNormalizationError, match="evidence is incomplete"):
        pipeline.AlpacaBackfillReferenceResolver({}, action_report, repository)
    malformed_sessions = _backfill_body(source.manifest_id)
    malformed_sessions["sessions"] = ["bad"]
    with pytest.raises(MinuteNormalizationError, match="calendar row"):
        pipeline.AlpacaBackfillReferenceResolver(
            pipeline._document(malformed_sessions), action_report, repository
        )
    earlier = pipeline._document({"finished_at_ns": 1, "source_manifest_ids": []})
    with pytest.raises(MinuteNormalizationError, match="predates"):
        pipeline.AlpacaBackfillReferenceResolver(
            pipeline._document(_backfill_body(source.manifest_id)), earlier, repository
        )

    non_source_repository = SimpleNamespace(
        root=root, load_manifest=lambda _manifest_id: object()
    )

    named_action = pipeline._document(
        {
            "finished_at_ns": 1789228800000000000,
            "source_manifest_ids": ["source-" + "1" * 64],
        }
    )
    with pytest.raises(MinuteNormalizationError, match="non-source"):
        pipeline.AlpacaBackfillReferenceResolver(
            pipeline._document(_backfill_body(source.manifest_id)),
            named_action,
            cast("DataRepository", non_source_repository),
        )

    bad_action_payload = (
        b'{"corporate_actions":{"forward_splits":[{"ex_date":"2026-09-10"}]}}'
    )
    digest = hashlib.sha256(bad_action_payload).hexdigest()
    path = root / "raw/bad-action.json"
    path.write_bytes(bad_action_payload)
    bad_manifest = SourceManifest(
        source_name="fixture",
        source_version="v1",
        source_object_id="bad-action",
        storage_path="raw/bad-action.json",
        object_sha256=digest,
        size_bytes=len(bad_action_payload),
        record_count=1,
        schema_name="fixture",
        schema_version="1.0.0",
        times=source.times,
        universe_snapshot_sha256=source.universe_snapshot_sha256,
        lineage=ManifestLineage(),
    )

    action_repository = SimpleNamespace(
        root=root, load_manifest=lambda _manifest_id: bad_manifest
    )

    with pytest.raises(MinuteNormalizationError, match="identity is malformed"):
        pipeline.AlpacaBackfillReferenceResolver(
            pipeline._document(_backfill_body(source.manifest_id)),
            named_action,
            cast("DataRepository", action_repository),
        )
    path.write_bytes(b"changed")
    with pytest.raises(MinuteNormalizationError, match="source object changed"):
        pipeline.AlpacaBackfillReferenceResolver(
            pipeline._document(_backfill_body(source.manifest_id)),
            named_action,
            cast("DataRepository", action_repository),
        )


def test_promotion_guards_and_verifier_detect_corruption(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    root = tmp_path / "data"
    source = _publish_bar_source(root)
    backfill_path = root / "reports/backfill.json"
    pipeline._write_immutable_document(
        backfill_path, _backfill_body(source.manifest_id)
    )
    action_path = root / "reports/actions.json"
    pipeline._write_immutable_document(
        action_path,
        {
            "finished_at_ns": 1789228800000000000,
            "source_manifest_ids": [],
        },
    )
    with pytest.raises(MinuteNormalizationError, match="must be positive"):
        pipeline.promote_backfill_to_parquet(
            data_root=root,
            backfill_report_path=backfill_path,
            action_report_path=action_path,
            output_report_path=root / "reports/none.json",
            maximum_source_objects=0,
        )
    limited_report = root / "reports/limited.json"
    limited_counts = cast(
        "Mapping[str, int]",
        pipeline.promote_backfill_to_parquet(
            data_root=root,
            backfill_report_path=backfill_path,
            action_report_path=action_path,
            output_report_path=limited_report,
            maximum_source_objects=1,
        )["counts"],
    )
    assert limited_counts["input_records"] == 1
    assert limited_counts["inserted_records"] == 1
    assert limited_counts["partition_count"] == 1
    assert limited_counts["published_records"] == 1
    assert limited_counts["rejected_outside_session"] == 0
    assert limited_counts["source_objects"] == 1
    bad_backfill = _backfill_body(source.manifest_id)
    bad_backfill["source_manifest_ids"] = ["source-" + "9" * 64]
    bad_backfill_path = root / "reports/bad-backfill.json"
    pipeline._write_immutable_document(bad_backfill_path, bad_backfill)
    monkeypatch.setattr(
        DataRepository,
        "load_manifest",
        lambda _self, _manifest_id: object(),
    )
    with pytest.raises(MinuteNormalizationError, match="non-Alpaca-bars"):
        pipeline.promote_backfill_to_parquet(
            data_root=root,
            backfill_report_path=bad_backfill_path,
            action_report_path=action_path,
            output_report_path=root / "reports/bad.json",
        )
    monkeypatch.undo()

    report_path = root / "reports/promotion.json"
    pipeline.promote_backfill_to_parquet(
        data_root=root,
        backfill_report_path=backfill_path,
        action_report_path=action_path,
        output_report_path=report_path,
    )
    assert (
        pipeline.promote_backfill_to_parquet(
            data_root=root,
            backfill_report_path=backfill_path,
            action_report_path=action_path,
            output_report_path=report_path,
        )["status"]
        == "COMPLETED"
    )
    report = pipeline._read_document(report_path, "promotion")
    repository = DataRepository(root)
    manifest_id = cast("list[str]", report["partition_manifest_ids"])[0]
    manifest = repository.load_manifest(manifest_id)
    parquet = root / manifest.storage_path
    original = parquet.read_bytes()
    parquet.write_bytes(original + b"corrupt")
    with pytest.raises(MinuteNormalizationError, match="does not match"):
        pipeline.verify_promotion(data_root=root, report_path=report_path)
    parquet.write_bytes(original)
    wrong_count = dict(report)
    wrong_count.pop("document_sha256")
    counts = dict(cast("dict[str, int]", wrong_count["counts"]))
    counts["published_records"] += 1
    wrong_count["counts"] = counts
    wrong_report = root / "reports/wrong-count.json"
    pipeline._write_immutable_document(wrong_report, wrong_count)
    with pytest.raises(MinuteNormalizationError, match="record count differs"):
        pipeline.verify_promotion(data_root=root, report_path=wrong_report)


def test_promotion_stop_checks(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    root = tmp_path / "data"
    source = _publish_bar_source(root)
    backfill_path = root / "reports/backfill.json"
    pipeline._write_immutable_document(
        backfill_path, _backfill_body(source.manifest_id)
    )
    action_path = root / "reports/actions.json"
    pipeline._write_immutable_document(
        action_path,
        {"finished_at_ns": 1789228800000000000, "source_manifest_ids": []},
    )
    monkeypatch.setattr(pipeline, "_STOP_REQUESTED", True)
    with pytest.raises(MinuteNormalizationError, match="interrupted after checkpoint"):
        pipeline.promote_backfill_to_parquet(
            data_root=root,
            backfill_report_path=backfill_path,
            action_report_path=action_path,
            output_report_path=root / "reports/stopped-ingest.json",
        )

    monkeypatch.setattr(pipeline, "_STOP_REQUESTED", False)
    original_ingest = CanonicalMinuteSpool.ingest

    def ingest_then_stop(
        self: CanonicalMinuteSpool,
        manifest: SourceManifest,
        records: Iterable[RawMinuteBar],
        normalizer: CanonicalMinuteNormalizer,
        *,
        fault_injector: Callable[[str], None] | None = None,
    ) -> SpoolIngestResult:
        result = original_ingest(
            self,
            manifest,
            records,
            normalizer,
            fault_injector=fault_injector,
        )
        pipeline._STOP_REQUESTED = True
        return result

    monkeypatch.setattr(CanonicalMinuteSpool, "ingest", ingest_then_stop)
    with pytest.raises(MinuteNormalizationError, match="publication interrupted"):
        pipeline.promote_backfill_to_parquet(
            data_root=root,
            backfill_report_path=backfill_path,
            action_report_path=action_path,
            output_report_path=root / "reports/stopped-publish.json",
        )
    monkeypatch.setattr(pipeline, "_STOP_REQUESTED", False)


def test_cli_dispatch_signals_and_structured_error(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    monkeypatch.setattr(
        pipeline,
        "authorize_corporate_actions",
        lambda **_kwargs: {"status": "AUTHORIZED"},
    )
    monkeypatch.setattr(
        pipeline, "download_corporate_actions", lambda **_kwargs: {"status": "FETCHED"}
    )
    monkeypatch.setattr(
        pipeline,
        "promote_backfill_to_parquet",
        lambda **_kwargs: {"status": "PROMOTED"},
    )
    monkeypatch.setattr(
        pipeline, "verify_promotion", lambda **_kwargs: {"status": "PASS"}
    )
    common = ["--data-root", str(tmp_path)]
    assert (
        pipeline.main(
            [
                *common,
                "authorize-actions",
                "--operator-id",
                "operator",
                "--expires-at-utc",
                "2026-10-01T00:00:00Z",
                "--acknowledge-internal-academic-use",
            ]
        )
        == 0
    )
    assert (
        pipeline.main(
            [
                *common,
                "fetch-actions",
                "--execute",
                "--secrets-file",
                str(tmp_path / "keys"),
            ]
        )
        == 0
    )
    assert (
        pipeline.main(
            [
                *common,
                "fetch-actions",
                "--secrets-file",
                str(tmp_path / "keys"),
            ]
        )
        == 0
    )
    assert pipeline.main([*common, "promote"]) == 0
    assert pipeline.main([*common, "promote", "--execute"]) == 0
    assert pipeline.main([*common, "verify"]) == 0
    assert "PASS" in capsys.readouterr().out
    monkeypatch.setattr(
        pipeline,
        "verify_promotion",
        lambda **_kwargs: (_ for _ in ()).throw(ValueError("fixture failure")),
    )
    assert pipeline.main([*common, "verify"]) == 1
    assert "PIPELINE_REJECTED" in capsys.readouterr().out
    pipeline._signal_handler(15, None)
    assert pipeline._STOP_REQUESTED is True
    monkeypatch.setattr(signal, "signal", lambda *_args: None)
    monkeypatch.setattr(pipeline, "main", lambda: 7)
    assert pipeline.cli_main() == 7
