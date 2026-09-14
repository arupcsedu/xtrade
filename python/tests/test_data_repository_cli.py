"""Filesystem integration tests for the ``aegis-data`` command."""

from __future__ import annotations

import json
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from pathlib import Path

from aegis_mx_research import DECIMAL_GB
from aegis_mx_research.data_cli import main
from aegis_mx_research.data_repository import (
    LEGACY_STORAGE_POLICY_V2_SHA256,
    STORAGE_AREAS,
)


def _base(root: Path) -> list[str]:
    return ["--data-root", str(root)]


def _quota() -> list[str]:
    return [
        "--quota-limit-bytes",
        str(250 * DECIMAL_GB),
        "--quota-used-bytes",
        str(10 * DECIMAL_GB),
        "--quota-source",
        "synthetic-cli-fixture",
        "--quota-observed-at-utc",
        "2026-09-09T16:00:00Z",
        "--quota-authoritative",
    ]


def test_usage_outputs_labeled_decimal_and_binary_values(
    tmp_path: Path, capsys: object
) -> None:
    root = tmp_path / "data"
    assert main([*_base(root), "usage"]) == 0
    output = json.loads(capsys.readouterr().out)  # type: ignore[attr-defined]
    assert output["command"] == "usage"
    assert output["report"]["total"]["bytes_decimal"] >= 0
    assert "gib_binary" in output["report"]["total"]
    assert output["audit"]["outcome"] == "SUCCEEDED"


def test_estimate_requires_authoritative_quota_and_reports_denial(
    tmp_path: Path, capsys: object
) -> None:
    root = tmp_path / "data"
    assert main([*_base(root), "estimate", "--output-bytes", "1"]) == 2
    denied = json.loads(capsys.readouterr().out)  # type: ignore[attr-defined]
    assert denied["report"]["admitted"] is False
    assert "QUOTA_UNKNOWN" in denied["report"]["reasons"]

    arguments = [
        *_base(root),
        "estimate",
        "--output-bytes",
        "1000",
        "--temporary-bytes",
        "2000",
        "--retry-overhead-bytes",
        "3000",
        *_quota(),
    ]
    assert main(arguments) == 0
    accepted = json.loads(capsys.readouterr().out)  # type: ignore[attr-defined]
    assert accepted["report"]["admitted"] is True


def test_verify_and_cleanup_plan_are_read_only(tmp_path: Path, capsys: object) -> None:
    root = tmp_path / "data"
    assert main([*_base(root), "verify"]) == 0
    verified = json.loads(capsys.readouterr().out)  # type: ignore[attr-defined]
    assert verified["report"]["passed"] is True

    temporary = root / "tmp" / "interrupted.part"
    temporary.write_bytes(b"temporary")
    assert main([*_base(root), "cleanup-plan", "--target-bytes", "0"]) == 0
    planned = json.loads(capsys.readouterr().out)  # type: ignore[attr-defined]
    assert planned["report"]["automatic_deletion_performed"] is False
    assert planned["report"]["candidates"]
    assert temporary.read_bytes() == b"temporary"


def test_cli_fails_closed_with_structured_error(tmp_path: Path, capsys: object) -> None:
    root = tmp_path / "data"
    root.mkdir()
    (root / "raw").symlink_to(tmp_path, target_is_directory=True)
    assert main([*_base(root), "usage"]) == 1
    error = json.loads(capsys.readouterr().out)  # type: ignore[attr-defined]
    assert error["command"] == "usage"
    assert error["error"]["code"] == "SYMLINK_DETECTED"
    assert error["audit"]["outcome"] == "REJECTED"


def test_policy_migration_cli_is_dry_run_by_default_and_explicit(
    tmp_path: Path, capsys: object
) -> None:
    root = tmp_path / "legacy-data"
    root.mkdir()
    for area in STORAGE_AREAS:
        (root / area).mkdir()
    (root / ".admission.lock").touch(mode=0o600)
    marker_path = root / ".aegis-data-root.json"
    marker_path.write_text(
        json.dumps(
            {
                "data_root_kind": "AEGIS_MX_BOUNDED_FORECASTING_POC",
                "policy_sha256": LEGACY_STORAGE_POLICY_V2_SHA256,
                "schema_version": "2.0.0",
            },
            separators=(",", ":"),
            sort_keys=True,
        )
        + "\n"
    )
    marker_path.chmod(0o400)
    arguments = [
        *_base(root),
        "migrate-policy",
        "--expected-current-policy-sha256",
        LEGACY_STORAGE_POLICY_V2_SHA256,
    ]
    assert main(arguments) == 0
    planned = json.loads(capsys.readouterr().out)  # type: ignore[attr-defined]
    assert planned["report"]["execute_requested"] is False
    assert planned["report"]["migrated"] is False

    assert main([*arguments, "--execute"]) == 0
    migrated = json.loads(capsys.readouterr().out)  # type: ignore[attr-defined]
    assert migrated["report"]["execute_requested"] is True
    assert migrated["report"]["migrated"] is True
    assert main([*_base(root), "usage"]) == 0
    usage = json.loads(capsys.readouterr().out)  # type: ignore[attr-defined]
    assert usage["audit"]["policy_sha256"] == migrated["report"]["to_policy_sha256"]
    assert json.loads(marker_path.read_text())["schema_version"] == "3.0.0"
