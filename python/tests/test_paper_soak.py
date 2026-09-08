"""PAPER soak reporting rejects incomplete evidence and validates its schema."""

from __future__ import annotations

import json
from argparse import Namespace
from pathlib import Path

import pytest

from tools import paper_soak


def _worker(worker_id: int = 0, *, passed: bool = True) -> dict[str, object]:
    checks = {
        "no_invalid_state_transitions": passed,
        "no_duplicate_order_emission": passed,
        "zero_acknowledged_state_loss": passed,
        "all_certification_probes_passed": passed,
        "journal_integrity": passed,
        "clock_state_recovered_safely": passed,
        "model_freshness_enforced": passed,
        "stale_snapshots_not_consumed": passed,
        "position_and_pnl_consistent": passed,
    }
    load_checks = {
        "deterministic_replay": passed,
        "memory_stable": passed,
        "file_descriptors_stable": passed,
        "tail_latency_stable": passed,
    }
    return {
        "schema_version": "1.0",
        "kind": "paper_soak_worker",
        "mode": "PAPER",
        "worker_id": worker_id,
        "host": f"host-{worker_id}",
        "passed": passed,
        "load": {
            "mode": "PAPER",
            "live_trading_capable": False,
            "source_revision": "a" * 40,
            "primary_events": 1_000,
            "replay_events": 1_000,
            "realtime_events": 10,
            "primary_elapsed_ns": 1_000_000,
            "replay_elapsed_ns": 1_000_000,
            "accelerated_throughput_events_per_second": 1_000_000,
            "accelerated_cpu_utilization_ppm": 900_000,
            "resources": {
                "rss_growth_after_warmup_bytes": 1_024,
                "maximum_rss_growth_bytes": 67_108_864,
                "file_descriptor_growth": 0,
            },
            "latency_ns": {
                "p50": 100,
                "p95": 200,
                "p99": 300,
                "p99_9": 400,
                "maximum": 500,
            },
            "checks": load_checks,
        },
        "acceptance_cycles_completed": 2,
        "probe_runs": 10,
        "coverage": {
            "session_transitions": 10,
            "earnings_events": 2,
            "macro_releases": 2,
            "auction_periods": 2,
            "halts": 2,
            "feed_recoveries": 2,
            "model_restarts": 2,
            "gateway_restarts": 2,
            "configuration_updates": 2,
            "leader_failovers": 2,
        },
        "paper_metrics": {
            "source_events": 10,
            "orders": 2,
            "risk_approvals": 2,
            "gateway_commands": 2,
            "fills": 1,
            "telemetry_drops": 0,
            "telemetry_final_queue_occupancy": 0,
            "telemetry_maximum_queue_occupancy": 3,
            "final_absolute_position_units": 1,
            "final_pnl_currency_nanos": 10,
        },
        "checks": checks,
    }


def _arguments(tmp_path: Path, workers: list[Path]) -> Namespace:
    return Namespace(
        worker=workers,
        output=tmp_path / "paper-soak-report.json",
        human=tmp_path / "stability-report.md",
        minimum_primary_events=1_000 * len(workers),
        minimum_acceptance_cycles=2 * len(workers),
        throughput_imbalance_limit=2.0,
    )


def test_aggregate_accepts_complete_paper_evidence(tmp_path: Path) -> None:
    workers = []
    for worker_id in range(2):
        path = tmp_path / f"worker-{worker_id}.json"
        path.write_text(json.dumps(_worker(worker_id)))
        workers.append(path)

    arguments = _arguments(tmp_path, workers)
    assert paper_soak.aggregate(arguments) == 0
    report = json.loads(arguments.output.read_text())
    schema = json.loads(Path("schemas/paper-soak-report-v1.schema.json").read_text())
    assert set(schema["required"]).issubset(report)
    assert schema["properties"]["mode"]["const"] == report["mode"]
    assert len(report["evidence"][0]["sha256"]) == 64
    assert report["passed"] is True
    assert report["totals"]["primary_events"] == 2_000
    assert report["checks"]["paper_mode_only"] is True
    assert "synthetic/reference PAPER" in arguments.human.read_text()


def test_aggregate_fails_closed_on_worker_failure(tmp_path: Path) -> None:
    path = tmp_path / "worker-0.json"
    path.write_text(json.dumps(_worker(passed=False)))
    arguments = _arguments(tmp_path, [path])

    assert paper_soak.aggregate(arguments) == 1
    report = json.loads(arguments.output.read_text())
    assert report["passed"] is False
    assert report["checks"]["no_duplicate_order_emission"] is False


def test_aggregate_rejects_duplicate_worker_identity(tmp_path: Path) -> None:
    first = tmp_path / "first.json"
    second = tmp_path / "second.json"
    first.write_text(json.dumps(_worker()))
    second.write_text(json.dumps(_worker()))

    with pytest.raises(paper_soak.SoakError, match="unique"):
        paper_soak.aggregate(_arguments(tmp_path, [first, second]))


def test_slurm_launcher_uses_submit_directory_not_spooled_script_path() -> None:
    """Slurm copies scripts to /var/spool, so BASH_SOURCE is not the checkout."""
    launcher = Path("tools/slurm/paper-soak.sbatch").read_text()
    assert "SLURM_SUBMIT_DIR" in launcher
    assert 'dirname "${BASH_SOURCE[0]}"' not in launcher
    assert "engineering-contract.md" in launcher
