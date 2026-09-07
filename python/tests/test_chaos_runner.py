"""Deterministic chaos orchestration and report-contract tests."""

from __future__ import annotations

import hashlib
import json
from dataclasses import replace
from pathlib import Path
from typing import cast

import pytest

from tools.chaos_runner import (
    DEFAULT_CATALOG,
    MAX_ITERATIONS,
    FaultKind,
    ScenarioCatalog,
    load_catalog,
    main,
    run_catalog,
    write_report,
)

REPORT_SCHEMA = Path("schemas/chaos-result-report-v1.schema.json")


def canonical_sha256(value: object) -> str:
    payload = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(payload).hexdigest()


def test_catalog_defines_every_required_fault_and_safety_contract() -> None:
    catalog = load_catalog()

    assert catalog.catalog_schema_version == 1
    assert {scenario.fault for scenario in catalog.scenarios} == set(FaultKind)
    assert len(catalog.scenarios) == 23
    assert len(catalog.combinations) == 5
    assert len(catalog.sha256) == 64
    for scenario in catalog.scenarios:
        assert scenario.maximum_detection_latency_ns > 0
        assert scenario.expected_detection
        assert scenario.expected_automated_response
        assert scenario.recovery_criteria
        assert scenario.required_audit_events[0] == "CHAOS_FAULT_INJECTED"
        assert scenario.production_hook
        assert set(scenario.profiles) == {"fast", "nightly"}
        assert "LIVE" not in scenario.production_hook.upper()


def test_fast_profile_is_complete_deterministic_and_content_hashed() -> None:
    catalog = load_catalog()
    first = run_catalog(catalog, profile="fast", seed=20_260_828, iterations=3)
    second = run_catalog(catalog, profile="fast", seed=20_260_828, iterations=3)

    assert first == second
    assert first["mode"] == "SIMULATION"
    assert first["combinations"] == []
    summary = cast("dict[str, object]", first["summary"])
    assert summary == {
        "failed": 0,
        "passed": True,
        "scenario_attempts": 69,
        "scenario_count": 23,
        "simultaneous_combination_attempts": 0,
        "simultaneous_combination_count": 0,
    }
    without_hash = dict(first)
    result_hash = without_hash.pop("result_sha256")
    assert result_hash == canonical_sha256(without_hash)


def test_nightly_profile_runs_simultaneous_faults_and_bounded_soak() -> None:
    report = run_catalog(
        load_catalog(), profile="nightly", seed=0xA361_3526, iterations=25
    )
    summary = cast("dict[str, object]", report["summary"])
    combinations = cast("list[dict[str, object]]", report["combinations"])

    assert summary["passed"] is True
    assert summary["scenario_attempts"] == 575
    assert summary["simultaneous_combination_attempts"] == 125
    assert {item["expected_safety_state"] for item in combinations} == {
        "ANALYTICS_DEGRADED",
        "ORDERS_BLOCKED",
    }
    assert all(item["passed"] is True for item in combinations)


def test_expected_contract_cannot_mask_an_independent_detector_mismatch() -> None:
    catalog = load_catalog()
    first = catalog.scenarios[0]
    tampered = replace(first, expected_detection="ALWAYS_PASS")
    changed = ScenarioCatalog(
        catalog.catalog_id,
        catalog.catalog_schema_version,
        (tampered, *catalog.scenarios[1:]),
        catalog.combinations,
        catalog.sha256,
    )

    report = run_catalog(
        changed,
        profile="fast",
        seed=20_260_828,
        iterations=1,
        selected_scenarios=(first.scenario_id,),
    )
    result = cast("list[dict[str, object]]", report["scenarios"])[0]
    summary = cast("dict[str, object]", report["summary"])
    assert result["passed"] is False
    assert result["failures"] == 1
    assert summary["failed"] == 1
    assert summary["passed"] is False


@pytest.mark.parametrize(
    ("profile", "seed", "iterations", "selected", "message"),
    [
        ("unknown", 1, 1, (), "profile"),
        ("fast", 0, 1, (), "seed"),
        ("fast", 1, 0, (), "iterations"),
        ("fast", 1, MAX_ITERATIONS + 1, (), "iterations"),
        ("fast", 1, 1, ("does-not-exist",), "selected scenario"),
    ],
)
def test_runner_rejects_unbounded_or_unknown_requests(
    profile: str,
    seed: int,
    iterations: int,
    selected: tuple[str, ...],
    message: str,
) -> None:
    with pytest.raises(ValueError, match=message):
        run_catalog(
            load_catalog(),
            profile=profile,
            seed=seed,
            iterations=iterations,
            selected_scenarios=selected,
        )


def test_catalog_rejects_corruption_duplicates_and_incomplete_fault_sets(
    tmp_path: Path,
) -> None:
    value = json.loads(DEFAULT_CATALOG.read_text(encoding="utf-8"))

    wrong_version = dict(value)
    wrong_version["catalog_schema_version"] = 2
    path = tmp_path / "wrong-version.json"
    path.write_text(json.dumps(wrong_version), encoding="utf-8")
    with pytest.raises(ValueError, match="schema version"):
        load_catalog(path)

    duplicate = json.loads(json.dumps(value))
    duplicate["scenarios"][1]["id"] = duplicate["scenarios"][0]["id"]
    path = tmp_path / "duplicate.json"
    path.write_text(json.dumps(duplicate), encoding="utf-8")
    with pytest.raises(ValueError, match="unique"):
        load_catalog(path)

    incomplete = json.loads(json.dumps(value))
    incomplete["scenarios"].pop()
    path = tmp_path / "incomplete.json"
    path.write_text(json.dumps(incomplete), encoding="utf-8")
    with pytest.raises(ValueError, match="every supported fault"):
        load_catalog(path)

    path = tmp_path / "empty.json"
    path.write_bytes(b"")
    with pytest.raises(ValueError, match="size"):
        load_catalog(path)


def test_report_matches_checked_in_schema_shape_and_atomic_writer(
    tmp_path: Path,
) -> None:
    report = run_catalog(
        load_catalog(),
        profile="nightly",
        seed=20_260_828,
        iterations=2,
        selected_scenarios=("split-brain",),
    )
    schema = json.loads(REPORT_SCHEMA.read_text(encoding="utf-8"))
    assert set(schema["required"]) == set(report)
    assert schema["properties"]["mode"]["const"] == report["mode"]
    assert (
        schema["properties"]["report_schema_version"]["const"]
        == report["report_schema_version"]
    )
    assert report["combinations"] == []

    destination = tmp_path / "nested" / "chaos.json"
    write_report(destination, report)
    assert json.loads(destination.read_text(encoding="utf-8")) == report
    assert list(destination.parent.glob("*.tmp")) == []


def test_cli_writes_report_and_returns_machine_failure_status(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    output = tmp_path / "fast.json"
    assert (
        main(
            [
                "--profile",
                "fast",
                "--iterations",
                "2",
                "--scenario",
                "trading-halt",
                "--output",
                str(output),
            ]
        )
        == 0
    )
    assert json.loads(output.read_text(encoding="utf-8"))["mode"] == "SIMULATION"
    assert json.loads(capsys.readouterr().out)["passed"] is True

    malformed = tmp_path / "malformed.json"
    malformed.write_text("{", encoding="utf-8")
    assert main(["--catalog", str(malformed), "--output", str(output)]) == 2
    assert capsys.readouterr().out.startswith("CHAOS ERROR:")
