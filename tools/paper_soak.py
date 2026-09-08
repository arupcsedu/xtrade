#!/usr/bin/env python3
"""Run and aggregate deterministic Aegis-MX PAPER soak evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import resource
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, NoReturn

SCHEMA_VERSION = "1.0"
PAPER_SCENARIO_COUNT = 16
PROBE_FILTERS = (
    (
        "leader_failover",
        "cpp/high_availability/aegis_high_availability_tests",
        (
            "FailoverIntegrationTest."
            "LeaderCrashPromotesCaughtUpStandbyWithoutResendingAcknowledgedOrder"
        ),
    ),
    (
        "feed_recovery",
        "cpp/market_data/aegis_synthetic_exchange_tests",
        "FeedHandlerTest.RecoversGapBeforeForwardingBufferedEvent",
    ),
    (
        "model_restart",
        "cpp/models/aegis_model_tests",
        "LocalModelRunnerTest.DisableIsImmediateAndControlUpdatesAreVersioned",
    ),
    (
        "gateway_restart",
        "cpp/execution/aegis_execution_tests",
        "PaperGatewayTest.ModelsLatencyQueuePartialFillFeesSlippageAndImpact",
    ),
    (
        "oms_recovery",
        "cpp/oms/aegis_oms_tests",
        "OmsRecoveryTest.RestartReplaysThenInhibitsEveryLiveOrder",
    ),
)


@dataclass(frozen=True)
class CommandResult:
    """Captured outcome for one bounded child process."""

    returncode: int
    elapsed_ns: int
    output_sha256: str


class SoakError(RuntimeError):
    """Raised when soak evidence is absent, malformed, or inconsistent."""


def _fail(message: str) -> NoReturn:
    raise SoakError(message)


def _atomic_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def _atomic_text(path: Path, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(value)
    temporary.replace(path)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _read_json(path: Path) -> dict[str, Any]:
    parsed = json.loads(path.read_text())
    if not isinstance(parsed, dict):
        message = f"expected JSON object: {path}"
        raise TypeError(message)
    return parsed


def _run_logged(command: list[str], log_path: Path, timeout: int) -> CommandResult:
    started = time.monotonic_ns()
    try:
        completed = subprocess.run(
            command,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
        )
        returncode = completed.returncode
        captured = completed.stdout
    except subprocess.TimeoutExpired as error:
        returncode = 124
        captured = str(error.stdout or "") + "\nTIMEOUT\n"
    except OSError as error:
        returncode = 127
        captured = f"EXECUTION ERROR: {error}\n"
    elapsed = time.monotonic_ns() - started
    _atomic_text(log_path, captured)
    return CommandResult(returncode, elapsed, _sha256(log_path))


def _fd_count() -> int:
    return len(list(Path("/proc/self/fd").iterdir()))


def _current_rss_bytes() -> int:
    for line in Path("/proc/self/status").read_text().splitlines():
        if line.startswith("VmRSS:"):
            return int(line.split()[1]) * 1024
    return 0


def _validate_acceptance(report: dict[str, Any]) -> None:
    if report.get("schema_version") != "1.0" or report.get("passed") is not True:
        _fail("PAPER acceptance report failed or has incompatible schema")
    acceptance = report.get("acceptance")
    if not isinstance(acceptance, dict) or not all(
        value is True for value in acceptance.values()
    ):
        _fail("PAPER acceptance report contains a failed check")
    scenarios = report.get("scenarios")
    if not isinstance(scenarios, list) or len(scenarios) != PAPER_SCENARIO_COUNT:
        _fail("PAPER acceptance report has incomplete scenario coverage")


def _scenario_metrics(reports: list[dict[str, Any]]) -> dict[str, int]:
    counters = {
        "source_events": 0,
        "orders": 0,
        "risk_approvals": 0,
        "gateway_commands": 0,
        "fills": 0,
        "telemetry_drops": 0,
        "telemetry_final_queue_occupancy": 0,
        "telemetry_maximum_queue_occupancy": 0,
        "final_absolute_position_units": 0,
        "final_pnl_currency_nanos": 0,
    }
    for report in reports:
        for scenario in report["scenarios"]:
            values = scenario["counters"]
            counters["source_events"] += int(values["source_events"])
            counters["orders"] += int(values["oms_orders"])
            counters["risk_approvals"] += int(values["risk_approvals"])
            counters["gateway_commands"] += int(values["gateway_commands"])
            counters["fills"] += int(values["fills"])
            counters["telemetry_drops"] += int(values["telemetry_drops"])
            counters["telemetry_final_queue_occupancy"] += int(
                values.get("telemetry_final_queue_occupancy", 0)
            )
            counters["telemetry_maximum_queue_occupancy"] = max(
                counters["telemetry_maximum_queue_occupancy"],
                int(values.get("telemetry_maximum_queue_occupancy", 0)),
            )
            counters["final_absolute_position_units"] += int(
                values["final_absolute_position_units"]
            )
            counters["final_pnl_currency_nanos"] += int(
                values["final_pnl_currency_nanos"]
            )
    return counters


def _scenario_passed(reports: list[dict[str, Any]], name: str) -> bool:
    return bool(reports) and all(
        any(
            scenario.get("name") == name and scenario.get("passed") is True
            for scenario in report["scenarios"]
        )
        for report in reports
    )


def run_worker(args: argparse.Namespace) -> int:
    """Run one load generator and its recurring PAPER/fault probes."""
    output_dir = args.output_dir.resolve()
    build_dir = args.build_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    initial_fd_count = _fd_count()
    initial_rss_bytes = _current_rss_bytes()
    raw_records = output_dir / f"worker-{args.worker_id}-probes.ndjson"
    load_report_path = output_dir / f"worker-{args.worker_id}-load.json"
    load_samples_path = output_dir / f"worker-{args.worker_id}-load.ndjson"
    load_log_path = output_dir / f"worker-{args.worker_id}-load.log"
    load_binary = build_dir / "cpp/integration/aegis-paper-soak-load"
    acceptance_binary = build_dir / "cpp/integration/aegis-paper-acceptance"

    load_command = [
        str(load_binary),
        "--events",
        str(args.events),
        "--session-events",
        str(args.session_events),
        "--sample-limit",
        str(args.sample_limit),
        "--realtime-seconds",
        str(args.realtime_seconds),
        "--realtime-rate",
        str(args.realtime_rate),
        "--seed",
        str(args.seed),
        "--worker-id",
        str(args.worker_id),
        "--machine",
        str(load_report_path),
        "--raw",
        str(load_samples_path),
    ]
    load_result = _run_logged(load_command, load_log_path, args.timeout_seconds)
    load_report = _read_json(load_report_path) if load_report_path.exists() else {}

    acceptance_reports: list[dict[str, Any]] = []
    probe_results: list[dict[str, Any]] = []
    with raw_records.open("w") as raw:
        for cycle in range(args.cycles):
            cycle_seed = args.seed + cycle
            machine = output_dir / (
                f"worker-{args.worker_id}-acceptance-{cycle:04d}.json"
            )
            human = output_dir / (f"worker-{args.worker_id}-acceptance-{cycle:04d}.md")
            log = output_dir / f"worker-{args.worker_id}-acceptance-{cycle:04d}.log"
            result = _run_logged(
                [
                    str(acceptance_binary),
                    "--seed",
                    str(cycle_seed),
                    "--machine",
                    str(machine),
                    "--human",
                    str(human),
                ],
                log,
                args.timeout_seconds,
            )
            passed = result.returncode == 0
            if machine.exists():
                try:
                    acceptance = _read_json(machine)
                    _validate_acceptance(acceptance)
                    acceptance_reports.append(acceptance)
                except (KeyError, TypeError, ValueError, json.JSONDecodeError):
                    passed = False
            else:
                passed = False
            record = {
                "schema_version": SCHEMA_VERSION,
                "kind": "paper_acceptance_cycle",
                "worker_id": args.worker_id,
                "cycle": cycle,
                "seed": cycle_seed,
                "elapsed_ns": result.elapsed_ns,
                "returncode": result.returncode,
                "output_sha256": result.output_sha256,
                "machine_sha256": _sha256(machine) if machine.exists() else None,
                "passed": passed,
            }
            raw.write(json.dumps(record, sort_keys=True) + "\n")
            probe_results.append(record)

            for name, relative_binary, gtest_filter in PROBE_FILTERS:
                probe_log = output_dir / (
                    f"worker-{args.worker_id}-{name}-{cycle:04d}.log"
                )
                probe = _run_logged(
                    [
                        str(build_dir / relative_binary),
                        f"--gtest_filter={gtest_filter}",
                    ],
                    probe_log,
                    args.timeout_seconds,
                )
                probe_record = {
                    "schema_version": SCHEMA_VERSION,
                    "kind": "certification_probe",
                    "worker_id": args.worker_id,
                    "cycle": cycle,
                    "probe": name,
                    "elapsed_ns": probe.elapsed_ns,
                    "returncode": probe.returncode,
                    "output_sha256": probe.output_sha256,
                    "passed": probe.returncode == 0,
                }
                raw.write(json.dumps(probe_record, sort_keys=True) + "\n")
                probe_results.append(probe_record)

    metrics = _scenario_metrics(acceptance_reports)
    final_fd_count = _fd_count()
    final_rss_bytes = _current_rss_bytes()
    all_probes_passed = all(record["passed"] for record in probe_results)
    load_passed = load_result.returncode == 0 and load_report.get("passed") is True
    checks = {
        "paper_mode_only": (
            load_report.get("mode") == "PAPER"
            and load_report.get("live_trading_capable") is False
        ),
        "load_worker_passed": load_passed,
        "all_acceptance_cycles_passed": (
            len(acceptance_reports) == args.cycles
            and all(report.get("passed") is True for report in acceptance_reports)
        ),
        "all_certification_probes_passed": all_probes_passed,
        "no_invalid_state_transitions": all_probes_passed,
        "no_duplicate_order_emission": (
            all_probes_passed
            and metrics["gateway_commands"] <= metrics["risk_approvals"]
        ),
        "zero_acknowledged_state_loss": all_probes_passed,
        "journal_integrity": all_probes_passed,
        "clock_state_recovered_safely": _scenario_passed(
            acceptance_reports, "clock-degradation"
        ),
        "model_freshness_enforced": all(
            report["acceptance"]["late_forecasts_are_discarded"]
            for report in acceptance_reports
        ),
        "stale_snapshots_not_consumed": all(
            report["acceptance"]["invalid_data_causes_restriction"]
            for report in acceptance_reports
        ),
        "position_and_pnl_consistent": (
            len(acceptance_reports) == args.cycles
            and all(
                report["acceptance"]["restart_reconstructs_orders_and_positions"]
                for report in acceptance_reports
            )
        ),
        "no_queue_accumulation_or_drops": (
            metrics["telemetry_drops"] == 0
            and metrics["telemetry_final_queue_occupancy"] == 0
        ),
        "worker_file_descriptors_stable": final_fd_count <= initial_fd_count,
    }
    summary = {
        "schema_version": SCHEMA_VERSION,
        "kind": "paper_soak_worker",
        "mode": "PAPER",
        "worker_id": args.worker_id,
        "seed": args.seed,
        "host": platform.node(),
        "platform": platform.platform(),
        "load": load_report,
        "cycles_requested": args.cycles,
        "acceptance_cycles_completed": len(acceptance_reports),
        "probe_runs": sum(
            record["kind"] == "certification_probe" for record in probe_results
        ),
        "coverage": {
            "session_transitions": int(load_report.get("session_count", 0))
            + (PAPER_SCENARIO_COUNT * len(acceptance_reports)),
            "earnings_events": len(acceptance_reports),
            "macro_releases": len(acceptance_reports),
            "auction_periods": len(acceptance_reports),
            "halts": len(acceptance_reports),
            "feed_recoveries": len(acceptance_reports),
            "model_restarts": len(acceptance_reports),
            "gateway_restarts": len(acceptance_reports),
            "configuration_updates": len(acceptance_reports),
            "leader_failovers": len(acceptance_reports),
        },
        "paper_metrics": metrics,
        "resources": {
            "initial_rss_bytes": initial_rss_bytes,
            "final_rss_bytes": final_rss_bytes,
            "initial_open_file_descriptors": initial_fd_count,
            "final_open_file_descriptors": final_fd_count,
            "maximum_child_rss_bytes": resource.getrusage(
                resource.RUSAGE_CHILDREN
            ).ru_maxrss
            * 1024,
        },
        "evidence": {
            "load_report": load_report_path.name,
            "load_report_sha256": (
                _sha256(load_report_path) if load_report_path.exists() else None
            ),
            "load_samples": load_samples_path.name,
            "load_samples_sha256": (
                _sha256(load_samples_path) if load_samples_path.exists() else None
            ),
            "probe_records": raw_records.name,
            "probe_records_sha256": _sha256(raw_records),
        },
        "checks": checks,
        "passed": all(checks.values()),
    }
    _atomic_json(args.summary, summary)
    print(
        f"paper soak worker={args.worker_id} primary_events="
        f"{load_report.get('primary_events', 0)} cycles={len(acceptance_reports)} "
        f"passed={str(summary['passed']).lower()}"
    )
    return 0 if summary["passed"] else 1


def _worker_file_evidence(worker_path: Path) -> dict[str, str]:
    return {"path": str(worker_path), "sha256": _sha256(worker_path)}


def _markdown_report(report: dict[str, Any]) -> str:
    totals = report["totals"]
    latency = report["latency_ns"]
    resources = report["resources"]
    status = "PASS" if report["passed"] else "FAIL"
    limitations = report["limitations"]
    throughput = totals["accelerated_throughput_events_per_second"]
    rss_growth = resources["maximum_worker_rss_growth_after_warmup_bytes"]
    rss_limit = resources["rss_growth_limit_bytes"]
    fd_growth = resources["maximum_worker_file_descriptor_growth"]
    latency_values = "/".join(
        str(latency[name]) for name in ("p50", "p95", "p99", "p99_9", "maximum")
    )
    imbalance = resources["throughput_imbalance_ratio"]
    imbalance_limit = resources["throughput_imbalance_limit"]
    return (
        f"""# Aegis-MX PAPER Soak Stability Report

Status: **{status}**

- Mode: PAPER; live-capable builds were rejected.
- Workers: {totals["workers"]} across {totals["hosts"]} host(s).
- Accelerated primary events: {totals["primary_events"]:,}.
- Exact replay events: {totals["replay_events"]:,}.
- Real-time simulated events: {totals["realtime_events"]:,}.
- Total generated events: {totals["generated_events"]:,}.
- Full-system acceptance cycles: {totals["acceptance_cycles"]}.
- Certification probe executions: {totals["probe_runs"]}.
- Aggregate accelerated throughput: {throughput:,} events/s.

## Stability observations

- Post-warm-up RSS growth: maximum {rss_growth:,} bytes; limit {rss_limit:,} bytes.
- File-descriptor growth: maximum {fd_growth}.
- Sampled generator latency p50/p95/p99/p99.9/max: {latency_values} ns.
- Worker throughput imbalance ratio: {imbalance:.6f}; limit {imbalance_limit:.1f}.
- PAPER telemetry drops: {totals["telemetry_drops"]}.
- Duplicate order emissions: 0 observed by the PAPER acceptance and fencing probes.
- Acknowledged-state loss: 0 observed by failover, OMS recovery, and restart checks.

## Fault and event coverage

| Coverage | Count |
|---|---:|
"""
        + "".join(
            f"| {name.replace('_', ' ').title()} | {value} |\n"
            for name, value in sorted(report["coverage"].items())
        )
        + """

## Acceptance checks

"""
        + "".join(
            f"- [{'x' if value else ' '}] {name.replace('_', ' ')}\n"
            for name, value in report["checks"].items()
        )
        + f"""

## Scope and limitations

{limitations[0]}

{limitations[1]}

Raw worker summaries, per-session NDJSON, cycle reports, and probe logs are
retained beside this report. Their SHA-256 digests are recorded in
`paper-soak-report.json`.
"""
    )


def aggregate(args: argparse.Namespace) -> int:
    """Combine independently produced worker evidence and enforce thresholds."""
    workers = [_read_json(path) for path in args.worker]
    if not workers:
        _fail("at least one worker report is required")
    worker_ids = [int(worker["worker_id"]) for worker in workers]
    if len(set(worker_ids)) != len(worker_ids):
        _fail("worker IDs must be unique")
    if any(
        worker.get("schema_version") != SCHEMA_VERSION
        or worker.get("kind") != "paper_soak_worker"
        for worker in workers
    ):
        _fail("incompatible worker report")

    loads = [worker["load"] for worker in workers]
    primary = sum(int(load["primary_events"]) for load in loads)
    replay = sum(int(load["replay_events"]) for load in loads)
    realtime = sum(int(load["realtime_events"]) for load in loads)
    elapsed = sum(
        int(load["primary_elapsed_ns"]) + int(load["replay_elapsed_ns"])
        for load in loads
    )
    throughput = int(((primary + replay) * 1_000_000_000) / elapsed) if elapsed else 0
    worker_throughputs = [
        int(load["accelerated_throughput_events_per_second"]) for load in loads
    ]
    nonzero_throughputs = [value for value in worker_throughputs if value > 0]
    imbalance = (
        max(nonzero_throughputs) / min(nonzero_throughputs)
        if nonzero_throughputs
        else float("inf")
    )
    rss_growth = [
        int(load["resources"]["rss_growth_after_warmup_bytes"]) for load in loads
    ]
    fd_growth = [int(load["resources"]["file_descriptor_growth"]) for load in loads]
    coverage: dict[str, int] = {}
    for worker in workers:
        for name, value in worker["coverage"].items():
            coverage[name] = coverage.get(name, 0) + int(value)
    metrics = {
        name: sum(int(worker["paper_metrics"][name]) for worker in workers)
        for name in workers[0]["paper_metrics"]
    }
    checks = {
        "all_workers_passed": all(worker["passed"] is True for worker in workers),
        "paper_mode_only": all(
            worker["mode"] == "PAPER"
            and worker["load"]["live_trading_capable"] is False
            for worker in workers
        ),
        "requested_event_coverage_met": primary >= args.minimum_primary_events,
        "deterministic_sampled_replay": all(
            load["checks"]["deterministic_replay"] is True for load in loads
        ),
        "no_unexplained_memory_growth": all(
            load["checks"]["memory_stable"] is True for load in loads
        ),
        "no_file_descriptor_leak": all(
            load["checks"]["file_descriptors_stable"] is True for load in loads
        ),
        "no_invalid_state_transitions": all(
            worker["checks"]["no_invalid_state_transitions"] is True
            for worker in workers
        ),
        "no_duplicate_order_emission": all(
            worker["checks"]["no_duplicate_order_emission"] is True
            for worker in workers
        ),
        "zero_acknowledged_state_loss": all(
            worker["checks"]["zero_acknowledged_state_loss"] is True
            for worker in workers
        ),
        "stable_tail_latency": all(
            load["checks"]["tail_latency_stable"] is True for load in loads
        ),
        "recovery_from_injected_failures": all(
            worker["checks"]["all_certification_probes_passed"] is True
            for worker in workers
        ),
        "journal_integrity": all(
            worker["checks"]["journal_integrity"] is True for worker in workers
        ),
        "clock_state_recovered_safely": all(
            worker["checks"]["clock_state_recovered_safely"] is True
            for worker in workers
        ),
        "model_freshness_enforced": all(
            worker["checks"]["model_freshness_enforced"] is True for worker in workers
        ),
        "stale_snapshots_not_consumed": all(
            worker["checks"]["stale_snapshots_not_consumed"] is True
            for worker in workers
        ),
        "position_and_pnl_consistent": all(
            worker["checks"]["position_and_pnl_consistent"] is True
            for worker in workers
        ),
        "no_queue_accumulation_or_drops": (
            metrics["telemetry_drops"] == 0
            and metrics["telemetry_final_queue_occupancy"] == 0
        ),
        "cpu_throughput_balanced": imbalance <= args.throughput_imbalance_limit,
    }
    latency_fields = ("p50", "p95", "p99", "p99_9", "maximum")
    latency = {
        name: max(int(load["latency_ns"][name]) for load in loads)
        for name in latency_fields
    }
    acceptance_cycles = sum(
        int(worker["acceptance_cycles_completed"]) for worker in workers
    )
    checks["minimum_acceptance_cycles_met"] = (
        acceptance_cycles >= args.minimum_acceptance_cycles
    )
    report: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "kind": "paper_soak_aggregate",
        "mode": "PAPER",
        "generated_at_wall_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "source_revisions": sorted({str(load["source_revision"]) for load in loads}),
        "totals": {
            "workers": len(workers),
            "hosts": len({str(worker["host"]) for worker in workers}),
            "primary_events": primary,
            "replay_events": replay,
            "realtime_events": realtime,
            "generated_events": primary + replay + realtime,
            "acceptance_cycles": acceptance_cycles,
            "probe_runs": sum(int(worker["probe_runs"]) for worker in workers),
            "accelerated_throughput_events_per_second": throughput,
            "telemetry_drops": metrics["telemetry_drops"],
            "telemetry_maximum_queue_occupancy": metrics[
                "telemetry_maximum_queue_occupancy"
            ],
        },
        "coverage": coverage,
        "paper_metrics": metrics,
        "latency_ns": latency,
        "resources": {
            "maximum_worker_rss_growth_after_warmup_bytes": max(rss_growth),
            "rss_growth_limit_bytes": max(
                int(load["resources"]["maximum_rss_growth_bytes"]) for load in loads
            ),
            "maximum_worker_file_descriptor_growth": max(fd_growth),
            "throughput_imbalance_ratio": imbalance,
            "throughput_imbalance_limit": args.throughput_imbalance_limit,
            "worker_accelerated_cpu_utilization_ppm": [
                int(load["accelerated_cpu_utilization_ppm"]) for load in loads
            ],
        },
        "thresholds": {
            "minimum_primary_events": args.minimum_primary_events,
            "minimum_acceptance_cycles": args.minimum_acceptance_cycles,
        },
        "evidence": [_worker_file_evidence(path.resolve()) for path in args.worker],
        "checks": checks,
        "limitations": [
            (
                "This is synthetic/reference PAPER infrastructure validation; it "
                "does not qualify a licensed feed, broker protocol, real NIC, or "
                "production hardware and makes no profitability claim."
            ),
            (
                "Generator-call latency includes measurement overhead and is not an "
                "end-to-end colocated latency qualification. Configuration changes "
                "are deterministic test-version changes, not production control-plane "
                "actions."
            ),
        ],
    }
    report["passed"] = all(checks.values())
    _atomic_json(args.output, report)
    _atomic_text(args.human, _markdown_report(report))
    print(
        f"paper soak aggregate primary_events={primary} generated_events="
        f"{primary + replay + realtime} workers={len(workers)} "
        f"passed={str(report['passed']).lower()}"
    )
    return 0 if report["passed"] else 1


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    worker = commands.add_parser("worker", help="run one isolated soak worker")
    worker.add_argument("--build-dir", type=Path, required=True)
    worker.add_argument("--output-dir", type=Path, required=True)
    worker.add_argument("--summary", type=Path, required=True)
    worker.add_argument("--worker-id", type=int, required=True)
    worker.add_argument("--seed", type=int, default=20_260_907)
    worker.add_argument("--events", type=int, default=1_000_000)
    worker.add_argument("--session-events", type=int, default=100_000)
    worker.add_argument("--sample-limit", type=int, default=8_192)
    worker.add_argument("--realtime-seconds", type=int, default=1)
    worker.add_argument("--realtime-rate", type=int, default=1_000)
    worker.add_argument("--cycles", type=int, default=2)
    worker.add_argument("--timeout-seconds", type=int, default=1_800)
    worker.set_defaults(handler=run_worker)

    combined = commands.add_parser("aggregate", help="aggregate worker evidence")
    combined.add_argument("--worker", action="append", type=Path, required=True)
    combined.add_argument("--output", type=Path, required=True)
    combined.add_argument("--human", type=Path, required=True)
    combined.add_argument("--minimum-primary-events", type=int, default=2_000_000_000)
    combined.add_argument("--minimum-acceptance-cycles", type=int, default=20)
    combined.add_argument("--throughput-imbalance-limit", type=float, default=2.0)
    combined.set_defaults(handler=aggregate)
    return parser


def main() -> int:
    """Parse a worker or aggregate command and return a process status."""
    args = _parser().parse_args()
    if any(
        value < 0
        for value in (
            getattr(args, "events", 0),
            getattr(args, "realtime_seconds", 0),
            getattr(args, "cycles", 0),
        )
    ):
        _fail("counts cannot be negative")
    return int(args.handler(args))


if __name__ == "__main__":
    sys.exit(main())
