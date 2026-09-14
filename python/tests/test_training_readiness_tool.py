"""Operator command tests for fail-closed training admission."""

from __future__ import annotations

import json
from types import SimpleNamespace
from typing import TYPE_CHECKING

import pytest
from aegis_mx_research.training_readiness import TrainingReadinessError

from tools import check_training_readiness as tool

if TYPE_CHECKING:
    from pathlib import Path


def _arguments(tmp_path: Path) -> list[str]:
    manifest = tmp_path / "manifest.json"
    manifest.write_text("{}")
    return [
        "--data-root",
        str(tmp_path),
        "--repo-root",
        str(tmp_path),
        "--ticker-file",
        str(tmp_path / "ticker.txt"),
        "--manifest",
        str(manifest),
        "--backfill-report",
        str(tmp_path / "backfill.json"),
        "--promotion-report",
        str(tmp_path / "promotion.json"),
        "--source-approval",
        str(tmp_path / "approval.json"),
    ]


def test_manifest_resolution_requires_one_unambiguous_object(tmp_path: Path) -> None:
    explicit = tmp_path / "explicit.json"
    assert tool._resolve_manifest(tmp_path, explicit) == explicit
    with pytest.raises(ValueError, match="exactly one"):
        tool._resolve_manifest(tmp_path, None)
    directory = tmp_path / "datasets/feature-poc/manifests"
    directory.mkdir(parents=True)
    first = directory / "first.json"
    first.write_text("{}")
    assert tool._resolve_manifest(tmp_path, None) == first
    (directory / "second.json").write_text("{}")
    with pytest.raises(ValueError, match="exactly one"):
        tool._resolve_manifest(tmp_path, None)


def test_command_is_offline_dry_run_and_exposes_verification_policy(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    observed: dict[str, object] = {}
    monkeypatch.setattr(tool, "capture_git_provenance", lambda _root: object())

    def assess(**keywords: object) -> dict[str, object]:
        observed.update(keywords)
        return {
            "assessment_id": "1" * 64,
            "status": "READY_FOR_INFRASTRUCTURE_VALIDATION",
        }

    monkeypatch.setattr(tool, "assess_training_readiness", assess)
    assert tool.main([*_arguments(tmp_path), "--skip-object-verification"]) == 0
    output = json.loads(capsys.readouterr().out)
    assert output["network_access_performed"] is False
    assert output["report_published"] is False
    assert observed["verify_objects"] is False
    assert observed["source_approval_path"] == tmp_path / "approval.json"


def test_command_refuses_blocked_publication(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(tool, "capture_git_provenance", lambda _root: object())
    monkeypatch.setattr(
        tool,
        "assess_training_readiness",
        lambda **_kwargs: {"assessment_id": "1" * 64, "status": "BLOCKED"},
    )
    with pytest.raises(TrainingReadinessError, match="cannot be published"):
        tool.main([*_arguments(tmp_path), "--execute"])


def test_command_publishes_only_an_admitted_report(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    report: dict[str, object] = {
        "assessment_id": "1" * 64,
        "status": "READY_FOR_INFRASTRUCTURE_VALIDATION",
    }
    repository = SimpleNamespace(initialize=lambda: None)
    published = tmp_path / "published.json"
    monkeypatch.setattr(tool, "capture_git_provenance", lambda _root: object())
    monkeypatch.setattr(tool, "assess_training_readiness", lambda **_kwargs: report)
    monkeypatch.setattr(tool, "DataRepository", lambda _root: repository)
    monkeypatch.setattr(tool, "_quota_evidence", lambda _root: object())

    def publish(selected_repository: object, _quota: object, selected: object) -> Path:
        assert selected_repository is repository
        assert selected is report
        return published

    monkeypatch.setattr(tool, "publish_training_readiness", publish)
    assert tool.main([*_arguments(tmp_path), "--execute"]) == 0
    output = json.loads(capsys.readouterr().out)
    assert output["report_published"] is True
    assert output["report_path"] == str(published)
    assert tool.cli_main.__doc__
