"""Full-platform performance evidence and regression-gate tests."""

from __future__ import annotations

import hashlib
import json
from copy import deepcopy
from dataclasses import replace
from pathlib import Path
from typing import Any, cast

import pytest

from tools.performance_benchmark import (
    compare_reports,
    load_policy,
    main,
    summarize,
)

REPORT_SCHEMA = Path("schemas/performance-benchmark-report-v1.schema.json")


def _write_sampler_fixture(
    directory: Path, *, samples_per_case: int = 4
) -> tuple[Path, Path]:
    policy = load_policy()
    samples_path = directory / "samples.ndjson"
    metadata_path = directory / "sampler.json"
    cases: list[dict[str, Any]] = []
    lines: list[str] = []
    blocked = policy.safety_blocks
    for scenario_index, scenario in enumerate(policy.scenarios):
        for stage_index, stage in enumerate(policy.stages):
            status = "SAFETY_BLOCKED" if (scenario, stage) in blocked else "MEASURED"
            observed = samples_per_case if status == "MEASURED" else 0
            latency_base = 100 + scenario_index + stage_index
            lines.extend(
                (
                    json.dumps(
                        {
                            "latency_ns": latency_base + offset,
                            "scenario": scenario,
                            "stage": stage,
                        },
                        sort_keys=True,
                    )
                )
                for offset in range(observed)
            )
            cases.append(
                {
                    "allocations": 0,
                    "cache_misses": None,
                    "cache_misses_unavailable_reason": "perf_event denied in fixture",
                    "cpu_time_ns": latency_base * observed,
                    "elapsed_ns": latency_base * observed,
                    "notes": ["test fixture"],
                    "operation_count": observed,
                    "packet_drops": 0,
                    "queue_capacity": 4096,
                    "queue_high_watermark": 16,
                    "scenario": scenario,
                    "stage": stage,
                    "status": status,
                }
            )
    samples_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    policy_hash = hashlib.sha256(
        Path("infra/benchmarks/performance-policy-v1.json").read_bytes()
    ).hexdigest()
    metadata = {
        "cases": cases,
        "code_commit": "0123456789abcdef",
        "configuration_sha256": policy_hash,
        "environment": {
            "build_type": "Release",
            "compiler": "clang 18 fixture",
            "cpu_governor": None,
            "cpu_model": "fixture-cpu",
            "hostname": "fixture-host",
            "kernel": "fixture-kernel",
            "logical_cpu_count": 8,
            "numa_node": None,
            "operating_system": "Fixture OS",
            "pinned_cpu": 3,
            "thread_pinning_verified": True,
        },
        "measurement_source": "IN_PROCESS_SYNTHETIC",
        "methodology": {
            "allocation_probe_enabled": True,
            "caches_warmed": True,
            "clock": "CLOCK_MONOTONIC_RAW",
            "hardware_cache_misses_available": False,
            "hardware_cache_misses_unavailable_reason": "perf_event denied in fixture",
            "samples_per_latency_case": samples_per_case,
            "warmup_iterations": 2,
        },
        "mode": "PAPER",
        "sampler_schema_version": 1,
        "seed": 20_260_906,
        "suite_id": "fixture-suite",
    }
    metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
    return metadata_path, samples_path


def test_summarizer_builds_complete_hashed_report_and_marks_smoke(
    tmp_path: Path,
) -> None:
    metadata, samples = _write_sampler_fixture(tmp_path)
    output = tmp_path / "report.json"
    policy = load_policy()

    report = summarize(metadata, samples, policy, output)

    assert json.loads(output.read_text(encoding="utf-8")) == report
    assert report["mode"] == "PAPER"
    assert report["measurement_source"] == "IN_PROCESS_SYNTHETIC"
    assert report["methodology"]["qualified_sample_count"] is False
    assert len(report["measurements"]) == len(policy.scenarios) * len(policy.stages)
    assert (
        report["raw_samples"]["sha256"]
        == hashlib.sha256(samples.read_bytes()).hexdigest()
    )
    assert report["raw_samples"]["record_count"] > 0
    halt_send = next(
        item
        for item in report["measurements"]
        if item["scenario"] == "halt" and item["stage"] == "tick-to-paper-send"
    )
    assert halt_send["status"] == "SAFETY_BLOCKED"
    assert halt_send["latency_ns"] is None
    assert halt_send["throughput_events_per_second"] is None

    schema = json.loads(REPORT_SCHEMA.read_text(encoding="utf-8"))
    assert set(schema["required"]) == set(report)
    assert schema["properties"]["mode"]["enum"] == ["SIMULATION", "PAPER"]


def test_regression_gate_passes_identical_qualified_reports(tmp_path: Path) -> None:
    metadata, samples = _write_sampler_fixture(tmp_path)
    policy = replace(load_policy(), minimum_samples=4)
    report = summarize(metadata, samples, policy, tmp_path / "report.json")

    result = compare_reports(report, deepcopy(report), policy)

    assert result["passed"] is True
    assert result["failure_count"] == 0
    assert len(result["result_sha256"]) == 64


def test_regression_gate_detects_tail_throughput_allocation_and_drop_regressions(
    tmp_path: Path,
) -> None:
    metadata, samples = _write_sampler_fixture(tmp_path)
    policy = replace(load_policy(), minimum_samples=4)
    baseline = summarize(metadata, samples, policy, tmp_path / "baseline.json")
    candidate = deepcopy(baseline)
    measurement = next(
        item
        for item in cast("list[dict[str, Any]]", candidate["measurements"])
        if item["scenario"] == "ordinary" and item["stage"] == "packet-to-normalized"
    )
    measurement["latency_ns"]["p99"] *= 2
    measurement["throughput_events_per_second"] /= 2
    measurement["allocations"] = 1
    measurement["packet_drops"] = 1

    result = compare_reports(baseline, candidate, policy)
    metrics = {failure["metric"] for failure in result["failures"]}

    assert result["passed"] is False
    assert {
        "allocations",
        "latency_ns.p99",
        "packet_drops",
        "throughput_events_per_second",
    }.issubset(metrics)


def test_regression_gate_rejects_cross_hardware_and_unpinned_comparisons(
    tmp_path: Path,
) -> None:
    metadata, samples = _write_sampler_fixture(tmp_path)
    policy = replace(load_policy(), minimum_samples=4)
    baseline = summarize(metadata, samples, policy, tmp_path / "baseline.json")
    candidate = deepcopy(baseline)
    candidate["environment"]["cpu_model"] = "different-cpu"
    candidate["environment"]["thread_pinning_verified"] = False

    result = compare_reports(baseline, candidate, policy)
    metrics = {failure["metric"] for failure in result["failures"]}

    assert "environment.cpu_model" in metrics
    assert "candidate.thread_pinning_verified" in metrics


def test_summarizer_rejects_duplicate_json_unknown_cases_and_false_safety(
    tmp_path: Path,
) -> None:
    metadata, samples = _write_sampler_fixture(tmp_path)
    duplicate = tmp_path / "duplicate.json"
    duplicate.write_text(
        '{"sampler_schema_version":1,"sampler_schema_version":1}', encoding="utf-8"
    )
    with pytest.raises(ValueError, match="duplicate JSON key"):
        summarize(duplicate, samples, load_policy(), tmp_path / "unused.json")

    lines = samples.read_text(encoding="utf-8").splitlines()
    first = json.loads(lines[0])
    first["scenario"] = "unknown"
    lines[0] = json.dumps(first)
    bad_samples = tmp_path / "bad-samples.ndjson"
    bad_samples.write_text("\n".join(lines) + "\n", encoding="utf-8")
    with pytest.raises(ValueError, match="unknown case"):
        summarize(metadata, bad_samples, load_policy(), tmp_path / "unused.json")

    metadata_value = json.loads(metadata.read_text(encoding="utf-8"))
    halt = next(
        case
        for case in metadata_value["cases"]
        if case["scenario"] == "halt" and case["stage"] == "tick-to-paper-send"
    )
    halt["status"] = "UNAVAILABLE"
    bad_metadata = tmp_path / "bad-metadata.json"
    bad_metadata.write_text(json.dumps(metadata_value), encoding="utf-8")
    with pytest.raises(ValueError, match="required safety block"):
        summarize(bad_metadata, samples, load_policy(), tmp_path / "unused.json")


def test_cli_compare_writes_machine_readable_failure(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    metadata, samples = _write_sampler_fixture(tmp_path)
    policy = replace(load_policy(), minimum_samples=4)
    baseline_path = tmp_path / "baseline.json"
    baseline = summarize(metadata, samples, policy, baseline_path)
    candidate = deepcopy(baseline)
    candidate["methodology"]["qualified_sample_count"] = False
    candidate_path = tmp_path / "candidate.json"
    candidate_path.write_text(json.dumps(candidate), encoding="utf-8")
    output = tmp_path / "regression.json"

    assert (
        main(
            [
                "compare",
                "--baseline",
                str(baseline_path),
                "--candidate",
                str(candidate_path),
                "--output",
                str(output),
            ]
        )
        == 1
    )
    assert json.loads(output.read_text(encoding="utf-8"))["passed"] is False
    assert json.loads(capsys.readouterr().out)["passed"] is False


def test_cli_compare_rejects_tampered_raw_evidence(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    metadata, samples = _write_sampler_fixture(tmp_path)
    policy = replace(load_policy(), minimum_samples=4)
    baseline_path = tmp_path / "baseline.json"
    candidate_path = tmp_path / "candidate.json"
    report = summarize(metadata, samples, policy, baseline_path)
    candidate_path.write_text(json.dumps(report), encoding="utf-8")
    samples.write_text(samples.read_text(encoding="utf-8") + "{}\n", encoding="utf-8")

    assert (
        main(
            [
                "compare",
                "--baseline",
                str(baseline_path),
                "--candidate",
                str(candidate_path),
                "--output",
                str(tmp_path / "regression.json"),
            ]
        )
        == 2
    )
    assert "raw evidence hash mismatch" in capsys.readouterr().out
