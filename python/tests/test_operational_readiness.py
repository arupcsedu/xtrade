"""Final readiness evidence proves non-live state without claiming readiness."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, cast

from tools import operational_readiness


def test_live_mode_evidence_is_fail_closed_and_preserves_blockers() -> None:
    report = operational_readiness.evaluate(
        Path.cwd(),
        Path("build/dev/cpp/integration/operator-simulation.json"),
        Path("build/dev"),
    )
    schema = cast(
        "dict[str, Any]",
        json.loads(
            Path("schemas/live-mode-disabled-evidence-v1.schema.json").read_text()
        ),
    )
    assert set(report) == set(schema["required"])
    assert report["passed"] is True
    assert all(report["checks"].values())
    assert report["live_mode_enabled"] is False
    assert report["production_activation_performed"] is False
    assert report["production_ready"] is False
    assert report["activation_status"] == "PROHIBITED"
    assert report["open_production_blockers"] == [
        "PRD-B002",
        "PRD-B003",
        "PRD-B004",
        "PRD-B005",
        "PRD-B006",
    ]
    assert len(report["edge_profiles"]) == 6
    assert all(profile["safe"] is True for profile in report["edge_profiles"])


def test_tampered_operator_evidence_fails_the_gate(tmp_path: Path) -> None:
    source_report = Path("build/dev/cpp/integration/operator-simulation.json")
    report = json.loads(source_report.read_text())
    source_audit = source_report.parent / report["audit_extract"]["path"]
    copied_audit = tmp_path / source_audit.name
    copied_audit.write_bytes(source_audit.read_bytes() + b"{}\n")
    copied_report = tmp_path / source_report.name
    copied_report.write_text(json.dumps(report))

    result = operational_readiness.evaluate(
        Path.cwd(), copied_report, Path("build/dev")
    )
    assert result["checks"]["operator_audit_extract_hash_matches"] is False
    assert result["passed"] is False


def test_operational_package_indexes_every_required_non_live_procedure() -> None:
    package = Path("docs/operations/operational-readiness-package.md").read_text()
    required_targets = (
        "../architecture/system-context.md",
        "../architecture/component-boundaries.md",
        "component-ownership.md",
        "startup-shutdown-checklist.md",
        "daily-paper-trading-checklist.md",
        "clock-failure-runbook.md",
        "market-data-recovery-runbook.md",
        "trading-halt-runbook.md",
        "gateway-failure-runbook.md",
        "edge-failover-runbook.md",
        "model-deployment-rollback-runbook.md",
        "risk-kill-switch-runbook.md",
        "portfolio-reconciliation-runbook.md",
        "../security/incident-response.md",
        "disaster-recovery-runbook.md",
        "../compliance/licensed-integration-checklist.md",
        "../compliance/regulatory-review-checklist.md",
        "production-activation-checklist.md",
        "../testing/operator-simulation.md",
    )
    assert all(target in package for target in required_targets)
    assert "STOP / NO-GO" in package
    assert "Live activation | **PROHIBITED" in package

    activation = Path("docs/operations/production-activation-checklist.md").read_text()
    assert "Current status: PROHIBITED" in activation
    assert "No production activation was performed" in activation
    assert "no action to run from this repository" in activation
