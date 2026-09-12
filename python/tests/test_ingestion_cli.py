"""CLI integration tests for provider-neutral bounded ingestion."""

from __future__ import annotations

import json
import sys
from datetime import date
from typing import TYPE_CHECKING

import pytest
from aegis_mx_research import (
    DECIMAL_GB,
    FetchResult,
    FetchStatus,
    ManifestTimeRange,
    SourceObject,
)
from aegis_mx_research.data_cli import cli_main, main

if TYPE_CHECKING:
    from pathlib import Path


def _base(root: Path) -> list[str]:
    return ["--data-root", str(root)]


def _quota() -> list[str]:
    return [
        "--quota-limit-bytes",
        str(250 * DECIMAL_GB),
        "--quota-used-bytes",
        "0",
        "--quota-source",
        "bounded-cli-test",
        "--quota-observed-at-utc",
        "2026-09-09T12:00:00Z",
        "--quota-authoritative",
    ]


def _fetch_arguments(root: Path, command: str) -> list[str]:
    return [
        *_base(root),
        command,
        "--provider",
        "synthetic",
        "--dataset",
        "synthetic-minute-bars",
        "--start",
        "2026-09-08",
        "--end",
        "2026-09-08",
        "--ticker",
        "AAPL",
        "--request-id",
        "cli-bounded-fetch",
        "--maximum-concurrency",
        "1",
        *_quota(),
    ]


def _output(capsys: pytest.CaptureFixture[str]) -> dict[str, object]:
    return json.loads(capsys.readouterr().out)  # type: ignore[no-any-return]


def _filesystem_fixture(root: Path) -> None:
    root.mkdir(parents=True)
    payload = b"row-one\nrow-two\n"
    (root / "AAPL.data").write_bytes(payload)
    base = 1_789_000_000_000_000_000
    source = SourceObject(
        provider_id="filesystem-replay",
        dataset_id="filesystem-bars",
        source_version="local-fixture-v1",
        locator="AAPL.data",
        version_id="version-1",
        coverage_date=date(2026, 9, 8),
        tickers=("AAPL",),
        estimated_size_bytes=len(payload),
        expected_sha256="f97110882ee83b64bae459c10cf6dd75a04aee50e61ff38f6cdae7d1cc4790ea",  # pragma: allowlist secret  # noqa: E501
        record_count=2,
        schema_name="fixture-lines",
        schema_version="1.0.0",
        content_format="application/x-ndjson",
        times=ManifestTimeRange(
            event_time_min_ns=base,
            event_time_max_ns=base + 1,
            publication_time_min_ns=base + 2,
            publication_time_max_ns=base + 3,
            receive_time_min_ns=base + 4,
            receive_time_max_ns=base + 5,
            processing_time_min_ns=base + 6,
            processing_time_max_ns=base + 7,
            revision_time_min_ns=base + 2,
            revision_time_max_ns=base + 3,
        ),
    )
    metadata = source.to_dict()
    metadata.pop("provider_id")
    (root / "AAPL.source.json").write_text(
        json.dumps(metadata, sort_keys=True), encoding="utf-8"
    )


def test_cli_plan_fetch_is_dry_run_and_unknown_quota_is_rejected(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    root = tmp_path / "data"
    arguments = _fetch_arguments(root, "plan-fetch")
    assert main(arguments) == 0
    planned = _output(capsys)
    assert planned["report"]["dry_run"] is True  # type: ignore[index]
    assert planned["report"]["network_access_performed"] is False  # type: ignore[index]

    without_quota = arguments[: -len(_quota())]
    assert main(without_quota) == 2
    denied = _output(capsys)
    assert denied["audit"]["outcome"] == "REJECTED"  # type: ignore[index]


def test_cli_fetch_requires_execute_then_publishes_and_inspects_source(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    root = tmp_path / "data"
    arguments = _fetch_arguments(root, "fetch")
    assert main(arguments) == 0
    dry_run = _output(capsys)
    assert dry_run["report"]["dry_run"] is True  # type: ignore[index]
    assert not list((root / "raw").rglob("*.source"))

    arguments.append("--execute")
    assert main(arguments) == 0
    fetched = _output(capsys)
    assert fetched["report"]["status"] == "COMPLETED"  # type: ignore[index]
    result_objects = fetched["report"]["objects"]  # type: ignore[index]
    manifest_id = result_objects[0]["manifest_id"]

    assert main([*_base(root), "inspect-source", "--manifest-id", manifest_id]) == 0
    inspected = _output(capsys)
    assert inspected["report"]["manifest_id"] == manifest_id  # type: ignore[index]

    resume = _fetch_arguments(root, "resume-fetch")
    resume.append("--execute")
    assert main(resume) == 0
    resumed = _output(capsys)
    assert resumed["report"]["objects"][0]["status"] == "ALREADY_PRESENT"  # type: ignore[index]


def test_cli_filesystem_provider_and_input_failures_are_structured(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    root = tmp_path / "data"
    fixture = tmp_path / "fixture"
    _filesystem_fixture(fixture)
    arguments = [
        *_base(root),
        "fetch",
        "--provider",
        "filesystem",
        "--filesystem-root",
        str(fixture),
        "--dataset",
        "filesystem-bars",
        "--start",
        "2026-09-08",
        "--end",
        "2026-09-08",
        "--ticker",
        "AAPL",
        "--execute",
        *_quota(),
    ]
    assert main(arguments) == 0
    assert _output(capsys)["report"]["status"] == "COMPLETED"  # type: ignore[index]

    missing_root = [
        item for item in arguments if item not in {"--filesystem-root", str(fixture)}
    ]
    assert main(missing_root) == 1
    assert _output(capsys)["error"]["code"] == "INVALID_REQUEST"  # type: ignore[index]

    outside_universe = _fetch_arguments(root, "plan-fetch")
    outside_universe[outside_universe.index("AAPL")] = "NOTREAL"
    assert main(outside_universe) == 1
    assert _output(capsys)["error"]["code"] == "INVALID_REQUEST"  # type: ignore[index]

    bad_date = _fetch_arguments(root, "plan-fetch")
    bad_date[bad_date.index("2026-09-08")] = "09/08/2026"
    with pytest.raises(SystemExit):
        main(bad_date)
    capsys.readouterr()


def test_cli_reports_failed_execution_and_installed_entrypoint(
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    failed = FetchResult(
        plan_id="12" * 32,
        request_id="cli-bounded-fetch",
        status=FetchStatus.FAILED,
        objects=(),
        network_access_performed=False,
        deterministic_seed=20260831,
    )
    monkeypatch.setattr(
        "aegis_mx_research.data_cli.IngestionCoordinator.execute",
        lambda *_args, **_kwargs: failed,
    )
    arguments = _fetch_arguments(tmp_path / "failed", "fetch")
    arguments.append("--execute")
    assert main(arguments) == 1
    assert _output(capsys)["audit"]["outcome"] == "REJECTED"  # type: ignore[index]

    monkeypatch.setattr(
        sys, "argv", ["aegis-data", *_base(tmp_path / "entry"), "usage"]
    )
    assert cli_main() == 0
    assert _output(capsys)["command"] == "usage"


def test_cli_rejects_malformed_source_manifest_id(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    status = main(
        [
            *_base(tmp_path / "data"),
            "inspect-source",
            "--manifest-id",
            "not-a-manifest",
        ]
    )
    assert status == 1
    error = _output(capsys)
    assert error["error"]["code"] == "MANIFEST_CORRUPT"  # type: ignore[index]
