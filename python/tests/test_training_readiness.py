"""Training-readiness admission and provenance tests."""

from __future__ import annotations

import hashlib
import json
import os
from datetime import UTC, datetime, timedelta, timezone
from pathlib import Path
from typing import TYPE_CHECKING, cast
from unittest.mock import patch

import aegis_mx_research.training_readiness as readiness
import pyarrow as pa  # type: ignore[import-untyped]
import pyarrow.parquet as pq  # type: ignore[import-untyped]
import pytest
from aegis_mx_research.data_repository import (
    DECIMAL_GB,
    DataRepository,
    FileSystemState,
    QuotaEvidence,
    StoragePolicy,
)
from aegis_mx_research.feature_dataset import FEATURE_NAMES
from aegis_mx_research.forecast_contracts import REQUIRED_HORIZON_LABELS
from aegis_mx_research.training_readiness import (
    EMPTY_SNAPSHOT_SHA256,
    GitProvenance,
    TrainingReadinessError,
    assess_training_readiness,
    publish_training_readiness,
)
from jsonschema import (  # type: ignore[import-untyped]
    Draft202012Validator,
    FormatChecker,
)

if TYPE_CHECKING:
    from collections.abc import Callable, Mapping


def _canonical_bytes(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("ascii")


def _digest(value: object) -> str:
    return hashlib.sha256(_canonical_bytes(value)).hexdigest()


def _self_hashed(body: Mapping[str, object], field: str) -> dict[str, object]:
    return {**body, field: _digest(body)}


def _write_document(path: Path, body: Mapping[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(_canonical_bytes(body) + b"\n")


def _rewrite_document(
    path: Path,
    hash_field: str,
    mutation: Callable[[dict[str, object]], None],
) -> None:
    body = cast("dict[str, object]", json.loads(path.read_text()))
    body.pop(hash_field)
    mutation(body)
    _write_document(path, _self_hashed(body, hash_field))


def _rewrite_manifest(
    fixture: Mapping[str, Path],
    mutation: Callable[[dict[str, object]], None],
) -> None:
    def apply(body: dict[str, object]) -> None:
        mutation(body)
        identity = body.get("dataset_identity")
        if isinstance(identity, dict):
            body["dataset_id"] = _digest(identity)

    _rewrite_document(fixture["manifest"], "manifest_sha256", apply)


def _rewrite_approval(
    fixture: Mapping[str, Path],
    mutation: Callable[[dict[str, object]], None],
    *,
    bind_backfill: bool = True,
) -> None:
    _rewrite_document(fixture["approval"], "document_sha256", mutation)
    if not bind_backfill:
        return
    approval = json.loads(fixture["approval"].read_text())
    _rewrite_document(
        fixture["backfill"],
        "document_sha256",
        lambda body: body.__setitem__(
            "approval_record_sha256", approval["document_sha256"]
        ),
    )
    backfill = json.loads(fixture["backfill"].read_text())
    _rewrite_document(
        fixture["promotion"],
        "document_sha256",
        lambda body: body.__setitem__(
            "backfill_report_sha256", backfill["document_sha256"]
        ),
    )


def _provenance(*, dirty: bool = False) -> GitProvenance:
    file_hash = hashlib.sha256(b"source").hexdigest()
    return GitProvenance(
        "1" * 40,
        dirty,
        hashlib.sha256(b"dirty" if dirty else b"").hexdigest(),
        (("builder.py", file_hash), ("python/requirements-dev.lock", file_hash)),
        _digest(
            [
                {"path": "builder.py", "sha256": file_hash},
                {"path": "python/requirements-dev.lock", "sha256": file_hash},
            ]
        ),
        file_hash,
    )


def _normalization() -> list[dict[str, object]]:
    result: list[dict[str, object]] = []
    unavailable = {
        "sector_return_1m_ppm",
        "sector_relative_return_1m_ppm",
    }
    constant = {
        "news_event_flag",
        "macro_event_flag",
        "data_quality_valid_flag",
        "data_quality_reason_count",
    }
    for feature in FEATURE_NAMES:
        if feature in unavailable:
            count, total, squares = 0, 0, 0
        elif feature in constant:
            count, total, squares = 1000, 0, 0
        else:
            count, total, squares = 1000, 10, 100
        result.append(
            {
                "count": count,
                "feature_name": feature,
                "maximum_fit_exchange_time_ns": 100,
                "sum": total,
                "sum_squares": squares,
            }
        )
    return result


def _fixture(tmp_path: Path) -> dict[str, Path]:
    data_root = tmp_path / "data"
    summary_path = data_root / "derived/summaries/part.parquet"
    summary_path.parent.mkdir(parents=True)
    summary_path.write_bytes(b"summary-object")
    feature_values: list[int | None] = [1] * len(FEATURE_NAMES)
    feature_values[14] = None
    feature_values[15] = None
    labels = [
        {
            "corporate_action_version": "5" * 64,
            "direction": 1,
            "future_price_ticks": 101,
            "horizon": horizon,
            "missing_reason": None,
            "return_ppm": 100,
            "target_exchange_time_ns": 200,
            "validity": "VALID",
        }
        for horizon in REQUIRED_HORIZON_LABELS
    ]
    objects: list[dict[str, object]] = []
    for split in ("TRAIN", "VALIDATION", "TEST"):
        relative = f"datasets/features/{split.lower()}.parquet"
        feature_path = data_root / relative
        feature_path.parent.mkdir(parents=True, exist_ok=True)
        table = pa.Table.from_pylist(
            [
                {
                    "feature_reason_codes": ["HALT_HISTORY_INCOMPLETE"],
                    "labels": labels,
                    "normalized_features_ppm": feature_values,
                    "source_ticker": "AAA",
                    "split": split,
                }
                for _ in range(200)
            ]
        )
        pq.write_table(table, feature_path, compression="zstd")
        objects.append(
            {
                "instrument_id": "1" * 32,
                "kind": "FEATURE_LABEL",
                "object_sha256": hashlib.sha256(feature_path.read_bytes()).hexdigest(),
                "record_count": 200,
                "size_bytes_decimal": feature_path.stat().st_size,
                "split": split,
                "storage_path": relative,
            }
        )
    objects.append(
        {
            "instrument_id": "1" * 32,
            "kind": "SESSION_SUMMARY",
            "object_sha256": hashlib.sha256(b"summary-object").hexdigest(),
            "record_count": 100,
            "size_bytes_decimal": len(b"summary-object"),
            "split": None,
            "storage_path": "derived/summaries/part.parquet",
        }
    )
    ticker = tmp_path / "ticker.txt"
    ticker.write_bytes(b"AAA\nZZZ\n")
    ticker_sha = hashlib.sha256(ticker.read_bytes()).hexdigest()
    approval_body = {
        "approval_id": "alpaca-iex-academic-single-user-v1",
        "data_root": str(data_root.resolve()),
        "decision": "APPROVED",
        "distribution": "INTERNAL_SINGLE_USER_ONLY",
        "expires_at_utc": "2026-10-11T23:59:59Z",
        "feed": "iex",
        "live_trading_capable": False,
        "model_training_authorized": True,
        "purpose": "INTERNAL_ACADEMIC_NONCOMMERCIAL_RESEARCH",
        "source_id": "alpaca_iex_historical_bars",
        "ticker_source_sha256": ticker_sha,
        "timeframe": "1Min",
    }
    approval_document = _self_hashed(approval_body, "document_sha256")
    approval = tmp_path / "approval.json"
    _write_document(approval, approval_document)
    coverage = []
    for symbol in ("AAA", "ZZZ"):
        for horizon in REQUIRED_HORIZON_LABELS:
            resolved = symbol == "AAA"
            coverage.append(
                {
                    "horizon": horizon,
                    "instrument_id": "1" * 32 if resolved else None,
                    "invalid": 0,
                    "missing": 0,
                    "reason_counts": {} if resolved else {"UNSUPPORTED": 1},
                    "resolution_code": "RESOLVED" if resolved else "UNSUPPORTED",
                    "samples": 600 if resolved else 0,
                    "symbol": symbol,
                    "valid": 600 if resolved else 0,
                }
            )
    identity = {
        "event_snapshot_sha256": EMPTY_SNAPSHOT_SHA256,
        "feature_names": list(FEATURE_NAMES),
        "horizons": list(REQUIRED_HORIZON_LABELS),
        "normalization": _normalization(),
        "objects": objects,
        "sector_snapshot_sha256": EMPTY_SNAPSHOT_SHA256,
        "source_partition_ids": ["partition-1"],
    }
    manifest_body = {
        "build_key": "2" * 64,
        "coverage": coverage,
        "dataset_id": _digest(identity),
        "dataset_identity": identity,
        "leakage": {"checked_samples": 1000, "status": "PASS", "violations": []},
        "objects": objects,
        "sample_count": 600,
        "session_summary_count": 100,
    }
    manifest = tmp_path / "manifest.json"
    _write_document(manifest, _self_hashed(manifest_body, "manifest_sha256"))
    backfill_body = {
        "approval_record_sha256": approval_document["document_sha256"],
        "feed": "iex",
        "live_trading_capable": False,
        "missing_symbol_session_pairs": ["AAA:2025-03-10", "ZZZ:2025-03-10"],
        "source_policy_sha256": "4" * 64,
        "ticker_source_sha256": ticker_sha,
        "timeframe": "1Min",
    }
    backfill_document = _self_hashed(backfill_body, "document_sha256")
    backfill = tmp_path / "backfill.json"
    _write_document(backfill, backfill_document)
    promotion_body = {
        "backfill_report_sha256": backfill_document["document_sha256"],
        "data_quality": {"mapping_semantics": "NOW_KNOWN_CURRENT_UNIVERSE"},
        "partition_manifest_ids": ["partition-1"],
    }
    promotion = tmp_path / "promotion.json"
    _write_document(promotion, _self_hashed(promotion_body, "document_sha256"))
    return {
        "approval": approval,
        "backfill": backfill,
        "data_root": data_root,
        "manifest": manifest,
        "promotion": promotion,
        "ticker": ticker,
    }


def _assess(
    fixture: Mapping[str, Path], *, provenance: GitProvenance | None = None
) -> dict[str, object]:
    with (
        patch.object(readiness, "MIN_POOLED_TRAINING_ROWS_PER_HORIZON", 1),
        patch.object(readiness, "MIN_POOLED_TRAINING_SYMBOLS_PER_HORIZON", 1),
    ):
        return assess_training_readiness(
            data_root=fixture["data_root"],
            dataset_manifest_path=fixture["manifest"],
            backfill_report_path=fixture["backfill"],
            promotion_report_path=fixture["promotion"],
            source_approval_path=fixture["approval"],
            ticker_path=fixture["ticker"],
            provenance=_provenance() if provenance is None else provenance,
            assessment_time_utc=datetime(2026, 9, 13, 12, tzinfo=UTC),
        )


def test_ready_profile_excludes_unavailable_and_constant_features(
    tmp_path: Path,
) -> None:
    report = _assess(_fixture(tmp_path))
    assert report["status"] == "READY_FOR_INFRASTRUCTURE_VALIDATION"
    assert report["blocking_reasons"] == []
    policy = cast("dict[str, object]", report["training_input_policy"])
    assert len(cast("list[str]", policy["selected_features"])) == 16
    excluded = {
        item["feature_name"]: item["reason_codes"]
        for item in cast("list[dict[str, object]]", policy["excluded_features"])
    }
    assert "SECTOR_SNAPSHOT_UNAVAILABLE" in cast(
        "list[str]", excluded["sector_return_1m_ppm"]
    )
    assert "EVENT_SNAPSHOT_UNAVAILABLE" in cast(
        "list[str]", excluded["news_event_flag"]
    )
    assert "ZERO_TRAIN_VARIANCE" in cast(
        "list[str]", excluded["data_quality_valid_flag"]
    )
    coverage = cast("dict[str, object]", report["coverage"])
    assert coverage["valid_labels"] == 12 * 600
    assert coverage["abstaining_symbols"] == ["ZZZ"]
    assert report["whole_universe_missing_sessions"] == ["2025-03-10"]
    dependencies = cast("dict[str, object]", report["external_dependencies"])
    assert dependencies["alpaca_api_or_network_required_for_training"] is False
    assert dependencies["alpaca_data_authorization_bound"] is True
    authorization = cast("dict[str, object]", report["authorization"])
    assert authorization["authorization_valid"] is True
    assert authorization["model_training_authorized"] is True
    assert authorization["checked_at_utc"] == "2026-09-13T12:00:00Z"

    schema = json.loads(
        Path("schemas/training-readiness-report-v1.schema.json").read_text()
    )
    Draft202012Validator(schema, format_checker=FormatChecker()).validate(report)


def test_training_profile_uses_covered_variable_news_but_not_uncovered_macro(
    tmp_path: Path,
) -> None:
    fixture = _fixture(tmp_path)

    def add_news_coverage(body: dict[str, object]) -> None:
        identity = cast("dict[str, object]", body["dataset_identity"])
        identity["event_snapshot_sha256"] = "a" * 64
        identity["event_feature_coverage"] = [
            {
                "end_time_ns": 300,
                "kind": "NEWS",
                "source_id": "SEC_EDGAR_ACCEPTED_FILINGS",
                "source_snapshot_sha256": "b" * 64,
                "start_time_ns": 1,
            }
        ]
        normalization = cast("list[dict[str, object]]", identity["normalization"])
        news = next(
            item for item in normalization if item["feature_name"] == "news_event_flag"
        )
        news.update({"count": 1000, "sum": 100, "sum_squares": 100})

    _rewrite_manifest(fixture, add_news_coverage)
    report = _assess(fixture)
    policy = cast("dict[str, object]", report["training_input_policy"])
    selected = cast("list[str]", policy["selected_features"])
    assert "news_event_flag" in selected
    assert "macro_event_flag" not in selected
    excluded = {
        item["feature_name"]: item["reason_codes"]
        for item in cast("list[dict[str, object]]", policy["excluded_features"])
    }
    assert excluded["macro_event_flag"] == [
        "EVENT_SNAPSHOT_UNAVAILABLE",
        "ZERO_TRAIN_VARIANCE",
    ]


def test_dirty_tree_and_skipped_verification_block_training(tmp_path: Path) -> None:
    fixture = _fixture(tmp_path)
    dirty = _assess(fixture, provenance=_provenance(dirty=True))
    assert dirty["status"] == "BLOCKED"
    assert "DIRTY_WORKTREE" in cast("list[str]", dirty["blocking_reasons"])
    skipped = assess_training_readiness(
        data_root=fixture["data_root"],
        dataset_manifest_path=fixture["manifest"],
        backfill_report_path=fixture["backfill"],
        promotion_report_path=fixture["promotion"],
        source_approval_path=fixture["approval"],
        ticker_path=fixture["ticker"],
        provenance=_provenance(),
        assessment_time_utc=datetime(2026, 9, 13, 12, tzinfo=UTC),
        verify_objects=False,
    )
    assert skipped["status"] == "BLOCKED"
    assert "DATASET_OBJECT_VERIFICATION_SKIPPED" in cast(
        "list[str]", skipped["blocking_reasons"]
    )


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("approval_id", ""),
        ("approval_id", "x" * 257),
        ("decision", "DENIED"),
        ("model_training_authorized", False),
        ("live_trading_capable", True),
        ("source_id", "other"),
        ("feed", "sip"),
        ("timeframe", "5Min"),
        ("purpose", "COMMERCIAL"),
        ("distribution", "EXTERNAL"),
        ("data_root", "/wrong"),
        ("data_root", "relative/data"),
        ("data_root", None),
        ("ticker_source_sha256", "0" * 64),
        ("expires_at_utc", "2026-09-13T12:00:00Z"),
        ("expires_at_utc", "2026-10-11T23:59:59+01:00"),
        ("expires_at_utc", "not-a-time"),
    ],
)
def test_source_authorization_contract_fails_closed(
    tmp_path: Path, field: str, value: object
) -> None:
    fixture = _fixture(tmp_path)
    _rewrite_approval(fixture, lambda body: body.__setitem__(field, value))
    report = _assess(fixture)
    assert report["status"] == "BLOCKED"
    assert "SOURCE_AUTHORIZATION_MISMATCH" in cast(
        "list[str]", report["blocking_reasons"]
    )
    authorization = cast("dict[str, object]", report["authorization"])
    assert authorization["authorization_valid"] is False

    schema = json.loads(
        Path("schemas/training-readiness-report-v1.schema.json").read_text()
    )
    Draft202012Validator(schema, format_checker=FormatChecker()).validate(report)


def test_source_authorization_binding_and_assessment_clock_are_authenticated(
    tmp_path: Path,
) -> None:
    aliased = _fixture(tmp_path / "aliased")
    alias = tmp_path / "data-root-alias"
    alias.symlink_to(aliased["data_root"], target_is_directory=True)
    _rewrite_approval(
        aliased, lambda body: body.__setitem__("data_root", str(alias.absolute()))
    )
    assert _assess(aliased)["status"] == "READY_FOR_INFRASTRUCTURE_VALIDATION"

    unbound = _fixture(tmp_path / "unbound")
    _rewrite_approval(
        unbound,
        lambda body: body.__setitem__("approval_id", "replacement"),
        bind_backfill=False,
    )
    assert "SOURCE_AUTHORIZATION_MISMATCH" in cast(
        "list[str]", _assess(unbound)["blocking_reasons"]
    )

    fixture = _fixture(tmp_path / "clock")
    for invalid_time in (
        datetime(2026, 9, 13, 12),  # noqa: DTZ001 - deliberately invalid fixture
        datetime(2026, 9, 13, 13, tzinfo=timezone(timedelta(hours=1))),
    ):
        with pytest.raises(TrainingReadinessError, match="timezone-aware UTC"):
            assess_training_readiness(
                data_root=fixture["data_root"],
                dataset_manifest_path=fixture["manifest"],
                backfill_report_path=fixture["backfill"],
                promotion_report_path=fixture["promotion"],
                source_approval_path=fixture["approval"],
                ticker_path=fixture["ticker"],
                provenance=_provenance(),
                assessment_time_utc=invalid_time,
            )

    _rewrite_approval(
        fixture, lambda body: body.__setitem__("expires_at_utc", "2999-01-01T00:00:00Z")
    )
    with (
        patch.object(readiness, "MIN_POOLED_TRAINING_ROWS_PER_HORIZON", 1),
        patch.object(readiness, "MIN_POOLED_TRAINING_SYMBOLS_PER_HORIZON", 1),
    ):
        assert (
            assess_training_readiness(
                data_root=fixture["data_root"],
                dataset_manifest_path=fixture["manifest"],
                backfill_report_path=fixture["backfill"],
                promotion_report_path=fixture["promotion"],
                source_approval_path=fixture["approval"],
                ticker_path=fixture["ticker"],
                provenance=_provenance(),
            )["status"]
            == "READY_FOR_INFRASTRUCTURE_VALIDATION"
        )

    missing_root = _fixture(tmp_path / "missing-root")
    with pytest.raises(
        TrainingReadinessError, match="data root cannot be authenticated"
    ):
        assess_training_readiness(
            data_root=tmp_path / "absent",
            dataset_manifest_path=missing_root["manifest"],
            backfill_report_path=missing_root["backfill"],
            promotion_report_path=missing_root["promotion"],
            source_approval_path=missing_root["approval"],
            ticker_path=missing_root["ticker"],
            provenance=_provenance(),
        )


def test_leakage_insufficient_labels_and_tamper_fail_closed(tmp_path: Path) -> None:
    fixture = _fixture(tmp_path)
    manifest = json.loads(fixture["manifest"].read_text())
    body = dict(manifest)
    body.pop("manifest_sha256")
    cast("dict[str, object]", body["leakage"])["status"] = "REJECTED"
    _write_document(fixture["manifest"], _self_hashed(body, "manifest_sha256"))
    report = _assess(fixture)
    assert report["status"] == "BLOCKED"
    assert "LEAKAGE_REJECTED" in cast("list[str]", report["blocking_reasons"])

    fixture = _fixture(tmp_path / "second")
    report = assess_training_readiness(
        data_root=fixture["data_root"],
        dataset_manifest_path=fixture["manifest"],
        backfill_report_path=fixture["backfill"],
        promotion_report_path=fixture["promotion"],
        source_approval_path=fixture["approval"],
        ticker_path=fixture["ticker"],
        provenance=_provenance(),
        assessment_time_utc=datetime(2026, 9, 13, 12, tzinfo=UTC),
        minimum_valid=601,
    )
    assert report["status"] == "BLOCKED"
    assert any(
        reason.startswith("INSUFFICIENT_LABELS:")
        for reason in cast("list[str]", report["blocking_reasons"])
    )

    fixture = _fixture(tmp_path / "third")
    (fixture["data_root"] / "datasets/features/train.parquet").write_bytes(b"tampered")
    with pytest.raises(TrainingReadinessError, match="SHA-256 mismatch"):
        _assess(fixture)


def _repository(tmp_path: Path) -> DataRepository:
    repository = DataRepository(
        tmp_path / "repository",
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
        observed_at_utc="2026-09-13T12:00:00Z",
        authoritative=True,
    )


def test_report_publication_is_admitted_immutable_and_idempotent(
    tmp_path: Path,
) -> None:
    report = _assess(_fixture(tmp_path / "assessment"))
    repository = _repository(tmp_path)
    first = publish_training_readiness(repository, _quota(), report)
    second = publish_training_readiness(repository, _quota(), report)
    assert first == second
    assert json.loads(first.read_text())["report_sha256"] == report["report_sha256"]

    changed = dict(report)
    changed["warnings"] = ["CHANGED"]
    with pytest.raises(TrainingReadinessError, match="self-authenticating"):
        publish_training_readiness(repository, _quota(), changed)

    blocked = _assess(
        _fixture(tmp_path / "blocked"), provenance=_provenance(dirty=True)
    )
    with pytest.raises(TrainingReadinessError, match="cannot be published"):
        publish_training_readiness(repository, _quota(), blocked)


def test_bounded_document_and_hash_readers_fail_closed(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    missing = tmp_path / "missing"
    with pytest.raises(TrainingReadinessError, match="cannot authenticate"):
        readiness._hash_file(missing)
    with pytest.raises(TrainingReadinessError, match="nonempty regular"):
        readiness._hash_file(Path("/dev/null"))

    empty = tmp_path / "empty.json"
    empty.touch()
    invalid = tmp_path / "invalid.json"
    invalid.write_text("{")
    array = tmp_path / "array.json"
    array.write_text("[]")
    for path, match in (
        (missing, "missing, malformed, or unsafe"),
        (empty, "missing, malformed, or unsafe"),
        (invalid, "missing, malformed, or unsafe"),
        (array, "must be a JSON object"),
    ):
        with pytest.raises(TrainingReadinessError, match=match):
            readiness._read_document(path, "fixture")

    symlink = tmp_path / "document-link"
    symlink.symlink_to(array)
    with pytest.raises(TrainingReadinessError, match="missing, malformed, or unsafe"):
        readiness._read_document(symlink, "fixture")

    oversized = tmp_path / "oversized.json"
    oversized.write_text("{}\n")
    monkeypatch.setattr(readiness, "MAX_DOCUMENT_BYTES", 2)
    with pytest.raises(TrainingReadinessError, match="missing, malformed, or unsafe"):
        readiness._read_document(oversized, "fixture")

    with pytest.raises(TrainingReadinessError, match="hash is malformed"):
        readiness._verify_self_hash({}, "sha256", "fixture")
    with pytest.raises(TrainingReadinessError, match="self-hash mismatch"):
        readiness._verify_self_hash({"value": 1, "sha256": "0" * 64}, "sha256", "x")
    assert not readiness._is_sha256("z" * 64)


def test_git_provenance_authenticates_sources_and_rejects_ambiguity(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    repository = tmp_path / "repository"
    repository.mkdir()
    source = repository / "builder.py"
    source.write_text("source\n")
    lock = repository / "python/requirements-dev.lock"
    lock.parent.mkdir()
    lock.write_text("lock\n")

    def git_result(_root: Path, arguments: tuple[str, ...]) -> bytes:
        if arguments == ("rev-parse", "HEAD"):
            return b"1" * 40 + b"\n"
        return b" M builder.py\n"

    monkeypatch.setattr(readiness, "_run_git", git_result)
    result = readiness.capture_git_provenance(
        repository, ("builder.py", "python/requirements-dev.lock")
    )
    assert result.worktree_dirty
    assert result.dependency_lock_sha256 == hashlib.sha256(b"lock\n").hexdigest()
    assert result.document()["git_commit"] == "1" * 40

    with pytest.raises(TrainingReadinessError, match="root cannot be authenticated"):
        readiness.capture_git_provenance(tmp_path / "absent", ())

    monkeypatch.setattr(readiness, "_run_git", lambda _root, _args: b"invalid")
    with pytest.raises(TrainingReadinessError, match="commit is malformed"):
        readiness.capture_git_provenance(repository, ())

    monkeypatch.setattr(readiness, "_run_git", git_result)
    for sources, match in (
        (("../outside", "python/requirements-dev.lock"), "path is unsafe"),
        (("missing", "python/requirements-dev.lock"), "source file is missing"),
        (("builder.py",), "dependency lock is not part"),
    ):
        with pytest.raises(TrainingReadinessError, match=match):
            readiness.capture_git_provenance(repository, sources)

    outside = tmp_path / "outside"
    outside.write_text("outside\n")
    (repository / "escape").symlink_to(outside)
    with pytest.raises(TrainingReadinessError, match="escapes repository"):
        readiness.capture_git_provenance(
            repository, ("escape", "python/requirements-dev.lock")
        )

    monkeypatch.undo()
    assert len(readiness._run_git(Path.cwd(), ("rev-parse", "HEAD")).strip()) == 40
    with pytest.raises(TrainingReadinessError, match="Git provenance"):
        readiness._run_git(repository, ("not-a-git-command",))


def test_dataset_object_verification_rejects_unsafe_or_corrupt_inputs(
    tmp_path: Path,
) -> None:
    fixture = _fixture(tmp_path)
    manifest = json.loads(fixture["manifest"].read_text())
    objects = cast("list[dict[str, object]]", manifest["objects"])
    with pytest.raises(TrainingReadinessError, match="empty or out of bounds"):
        readiness._verify_dataset_objects(fixture["data_root"], [])
    with pytest.raises(TrainingReadinessError, match="cannot be authenticated"):
        readiness._verify_dataset_objects(tmp_path / "missing", objects)

    malformed = [dict(objects[0], size_bytes_decimal=0)]
    unsafe = [dict(objects[0], storage_path="../outside")]
    missing = [dict(objects[0], storage_path="datasets/features/missing.parquet")]
    unsupported = [dict(objects[0], kind="UNKNOWN")]
    duplicate = [objects[0], objects[0]]
    for candidate, match in (
        (malformed, "metadata is malformed"),
        (unsafe, "path is unsafe"),
        (missing, "object is missing"),
        (unsupported, "kind is unsupported"),
        (duplicate, "metadata is malformed"),
    ):
        with pytest.raises(TrainingReadinessError, match=match):
            readiness._verify_dataset_objects(fixture["data_root"], candidate)

    target = fixture["data_root"] / "datasets/features/train.parquet"
    original = tmp_path / "original.parquet"
    target.rename(original)
    target.symlink_to(original)
    with pytest.raises(TrainingReadinessError, match="escapes the data root"):
        readiness._verify_dataset_objects(fixture["data_root"], [objects[0]])


def test_feature_and_coverage_metadata_reject_malformed_contracts() -> None:
    identity: dict[str, object] = {
        "normalization": _normalization(),
        "event_snapshot_sha256": EMPTY_SNAPSHOT_SHA256,
        "sector_snapshot_sha256": EMPTY_SNAPSHOT_SHA256,
    }
    for normalization, match in (
        ([], "normalization metadata"),
        ([{}] * len(FEATURE_NAMES), "normalization entry"),
        (list(reversed(_normalization())), "feature order"),
    ):
        identity["normalization"] = normalization
        with pytest.raises(TrainingReadinessError, match=match):
            readiness._feature_profile(identity)
    malformed_values = _normalization()
    malformed_values[0]["count"] = "bad"
    identity["normalization"] = malformed_values
    with pytest.raises(TrainingReadinessError, match="normalization values"):
        readiness._feature_profile(identity)

    identity["normalization"] = _normalization()
    identity["event_feature_coverage"] = [{}]
    with pytest.raises(TrainingReadinessError, match="event feature coverage"):
        readiness._feature_profile(identity)
    identity.pop("event_feature_coverage")
    identity["event_snapshot_sha256"] = "a" * 64
    normalization = cast("list[dict[str, object]]", identity["normalization"])
    for feature_name in ("news_event_flag", "macro_event_flag"):
        stat = next(
            item for item in normalization if item["feature_name"] == feature_name
        )
        stat.update({"count": 10, "sum": 1, "sum_squares": 1})
    selected, _excluded = readiness._feature_profile(identity)
    assert "news_event_flag" in selected
    assert "macro_event_flag" in selected

    with pytest.raises(TrainingReadinessError, match="not an array"):
        readiness._coverage_profile({}, ("AAA",), 100)
    with pytest.raises(TrainingReadinessError, match="entry is malformed"):
        readiness._coverage_profile(["bad"], ("AAA",), 100)
    with pytest.raises(TrainingReadinessError, match="values are malformed"):
        readiness._coverage_profile([{}], ("AAA",), 100)

    base = {
        "horizon": REQUIRED_HORIZON_LABELS[0],
        "invalid": 0,
        "missing": 0,
        "resolution_code": "UNSUPPORTED",
        "samples": 1,
        "symbol": "AAA",
        "valid": 1,
    }
    _profile, blockers = readiness._coverage_profile([base], ("AAA",), 100)
    assert "UNRESOLVED_HAS_SAMPLES:AAA:5m" in blockers
    assert "INCOMPLETE_TICKER_HORIZON_MATRIX" in blockers
    with pytest.raises(TrainingReadinessError, match="entry is duplicated"):
        readiness._coverage_profile([base, base], ("AAA",), 100)


def test_model_ready_scan_rejects_corrupt_parquet_contracts(tmp_path: Path) -> None:
    selected = readiness.REQUIRED_TRAINING_FEATURES
    with pytest.raises(TrainingReadinessError, match="at least one selected feature"):
        readiness._model_ready_profile(tmp_path, (), (), ("AAA",))
    with pytest.raises(TrainingReadinessError, match="scan metadata is malformed"):
        readiness._model_ready_profile(
            tmp_path,
            ({"kind": "FEATURE_LABEL"},),
            selected,
            ("AAA",),
        )

    def rewrite_first_feature(
        fixture: Mapping[str, Path],
        mutation: Callable[[list[dict[str, object]]], None],
    ) -> tuple[list[dict[str, object]], Path]:
        manifest = json.loads(fixture["manifest"].read_text())
        objects = cast("list[dict[str, object]]", manifest["objects"])
        path = fixture["data_root"] / cast("str", objects[0]["storage_path"])
        rows = cast("list[dict[str, object]]", pq.read_table(path).to_pylist())
        mutation(rows)
        pq.write_table(pa.Table.from_pylist(rows), path, compression="zstd")
        return objects, path

    def mix_ticker(rows: list[dict[str, object]]) -> None:
        rows[0]["source_ticker"] = "BBB"

    def remove_label(rows: list[dict[str, object]]) -> None:
        cast("list[dict[str, object]]", rows[0]["labels"]).pop()

    def reverse_labels(rows: list[dict[str, object]]) -> None:
        cast("list[dict[str, object]]", rows[0]["labels"]).reverse()

    for name, mutation, match in (
        ("mixed", mix_ticker, "mixes ticker or split"),
        ("cardinality", remove_label, "label cardinality"),
        ("order", reverse_labels, "label order"),
    ):
        fixture = _fixture(tmp_path / name)
        objects, _path = rewrite_first_feature(fixture, mutation)
        with pytest.raises(TrainingReadinessError, match=match):
            readiness._model_ready_profile(
                fixture["data_root"], objects, selected, ("AAA",)
            )

    invalid = _fixture(tmp_path / "invalid-parquet")
    invalid_manifest = json.loads(invalid["manifest"].read_text())
    invalid_objects = cast("list[dict[str, object]]", invalid_manifest["objects"])
    invalid_path = invalid["data_root"] / cast(
        "str", invalid_objects[0]["storage_path"]
    )
    invalid_path.write_bytes(b"not parquet")
    with pytest.raises(TrainingReadinessError, match="cannot be scanned safely"):
        readiness._model_ready_profile(
            invalid["data_root"], invalid_objects, selected, ("AAA",)
        )

    mismatch = _fixture(tmp_path / "record-count")
    mismatch_manifest = json.loads(mismatch["manifest"].read_text())
    mismatch_objects = cast("list[dict[str, object]]", mismatch_manifest["objects"])
    mismatch_objects[0]["record_count"] = 201
    with pytest.raises(TrainingReadinessError, match="record count mismatch"):
        readiness._model_ready_profile(
            mismatch["data_root"], mismatch_objects, selected, ("AAA",)
        )


def test_model_ready_scan_reports_quality_and_pooled_threshold_blockers(
    tmp_path: Path,
) -> None:
    fixture = _fixture(tmp_path)
    manifest = json.loads(fixture["manifest"].read_text())
    objects = cast("list[dict[str, object]]", manifest["objects"])
    train_path = fixture["data_root"] / cast("str", objects[0]["storage_path"])
    rows = cast("list[dict[str, object]]", pq.read_table(train_path).to_pylist())
    rows[0]["feature_reason_codes"] = ["UNREVIEWED_REASON"]
    pq.write_table(pa.Table.from_pylist(rows), train_path, compression="zstd")
    profile, blockers = readiness._model_ready_profile(
        fixture["data_root"], objects, readiness.REQUIRED_TRAINING_FEATURES, ("AAA",)
    )
    assert profile["scanned_feature_rows"] == 600
    assert "UNAPPROVED_DATA_QUALITY_REASON" in blockers
    assert "INSUFFICIENT_POOLED_TRAINING_ROWS:5m" in blockers
    assert "INSUFFICIENT_POOLED_TRAINING_SYMBOLS:5m" in blockers

    _profile, train_only_blockers = readiness._model_ready_profile(
        fixture["data_root"],
        (objects[0],),
        readiness.REQUIRED_TRAINING_FEATURES,
        ("AAA",),
    )
    assert "MISSING_MODEL_READY_VALIDATION:5m" in train_only_blockers
    assert "MISSING_MODEL_READY_TEST:5m" in train_only_blockers


def test_model_ready_scan_ignores_empty_batches(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    empty_batch = pa.RecordBatch.from_arrays(
        [
            pa.array([], type=pa.string()),
            pa.array([], type=pa.string()),
            pa.array([], type=pa.list_(pa.int64())),
            pa.array([], type=pa.list_(pa.string())),
            pa.array([], type=pa.list_(pa.struct([]))),
        ],
        names=(
            "source_ticker",
            "split",
            "normalized_features_ppm",
            "feature_reason_codes",
            "labels",
        ),
    )

    class EmptyParquet:
        def iter_batches(self, **_keywords: object) -> tuple[pa.RecordBatch, ...]:
            return (empty_batch,)

    monkeypatch.setattr(pq, "ParquetFile", lambda _path: EmptyParquet())
    profile, _blockers = readiness._model_ready_profile(
        tmp_path,
        (
            {
                "kind": "FEATURE_LABEL",
                "record_count": 0,
                "split": "TRAIN",
                "storage_path": "unused",
            },
        ),
        readiness.REQUIRED_TRAINING_FEATURES,
        ("AAA",),
    )
    assert profile["scanned_feature_rows"] == 0


def test_assessment_rejects_invalid_metadata_and_collects_all_blockers(
    tmp_path: Path,
) -> None:
    fixture = _fixture(tmp_path / "invalid-minimum")
    with pytest.raises(TrainingReadinessError, match="threshold must be positive"):
        assess_training_readiness(
            data_root=fixture["data_root"],
            dataset_manifest_path=fixture["manifest"],
            backfill_report_path=fixture["backfill"],
            promotion_report_path=fixture["promotion"],
            source_approval_path=fixture["approval"],
            ticker_path=fixture["ticker"],
            provenance=_provenance(),
            assessment_time_utc=datetime(2026, 9, 13, 12, tzinfo=UTC),
            minimum_valid=0,
        )

    missing_ticker = _fixture(tmp_path / "missing-ticker")
    missing_ticker["ticker"].unlink()
    with pytest.raises(TrainingReadinessError, match="universe cannot be read"):
        _assess(missing_ticker)

    bad_identity = _fixture(tmp_path / "identity")
    _rewrite_document(
        bad_identity["manifest"],
        "manifest_sha256",
        lambda body: body.__setitem__("dataset_id", "bad"),
    )
    with pytest.raises(TrainingReadinessError, match="identity is malformed"):
        _assess(bad_identity)

    incompatible = _fixture(tmp_path / "incompatible")

    def break_horizons(body: dict[str, object]) -> None:
        identity = cast("dict[str, object]", body["dataset_identity"])
        identity["horizons"] = []

    _rewrite_manifest(incompatible, break_horizons)
    with pytest.raises(TrainingReadinessError, match="contract is incompatible"):
        _assess(incompatible)

    malformed_objects = _fixture(tmp_path / "objects")
    _rewrite_manifest(
        malformed_objects, lambda body: body.__setitem__("objects", "bad")
    )
    with pytest.raises(TrainingReadinessError, match="objects are malformed"):
        _assess(malformed_objects)

    malformed_missing = _fixture(tmp_path / "missing-evidence")
    _rewrite_document(
        malformed_missing["backfill"],
        "document_sha256",
        lambda body: body.__setitem__("missing_symbol_session_pairs", ["bad"]),
    )
    with pytest.raises(TrainingReadinessError, match="missing-session evidence"):
        _assess(malformed_missing)

    malformed_quality = _fixture(tmp_path / "quality")
    _rewrite_document(
        malformed_quality["promotion"],
        "document_sha256",
        lambda body: body.__setitem__("data_quality", "bad"),
    )
    with pytest.raises(TrainingReadinessError, match="data-quality evidence"):
        _assess(malformed_quality)

    blocked = _fixture(tmp_path / "blockers")

    def add_manifest_blockers(body: dict[str, object]) -> None:
        body["sample_count"] = 999
        identity = cast("dict[str, object]", body["dataset_identity"])
        identity["source_partition_ids"] = ["different"]
        normalization = cast("list[dict[str, object]]", identity["normalization"])
        normalization[9].update(count=1000, sum=0, sum_squares=0)

    _rewrite_manifest(blocked, add_manifest_blockers)
    _rewrite_document(
        blocked["backfill"],
        "document_sha256",
        lambda body: body.update(
            approval_record_sha256="bad",
            ticker_source_sha256="0" * 64,
            missing_symbol_session_pairs=[],
        ),
    )
    _rewrite_document(
        blocked["promotion"],
        "document_sha256",
        lambda body: cast("dict[str, object]", body["data_quality"]).update(
            mapping_semantics="UNAPPROVED"
        ),
    )
    report = _assess(blocked)
    reasons = cast("list[str]", report["blocking_reasons"])
    assert "PROVENANCE_MISMATCH" in reasons
    assert "SOURCE_AUTHORIZATION_MISMATCH" in reasons
    assert "CANONICAL_PARTITION_LINEAGE_MISMATCH" in reasons
    assert "DATASET_OBJECT_RECORD_COUNT_MISMATCH" in reasons
    assert "MISSING_CORE_FEATURE" in reasons
    assert "UNAPPROVED_REFERENCE_SEMANTICS" in reasons
    assert report["whole_universe_missing_sessions"] == []


def test_publication_cleans_staging_file_after_write_failure(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    report = _assess(_fixture(tmp_path / "assessment"))
    repository = _repository(tmp_path)
    error_message = "fault"

    def fail_fsync(_descriptor: int) -> None:
        raise OSError(error_message)

    monkeypatch.setattr(os, "fsync", fail_fsync)
    with pytest.raises(OSError, match="fault"):
        publish_training_readiness(repository, _quota(), report)
    staged = (
        repository.root / f"tmp/training-readiness/{report['assessment_id']}.json.part"
    )
    assert not staged.exists()
