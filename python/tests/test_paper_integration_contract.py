"""Full-system PAPER acceptance report and launch-boundary tests."""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any, cast

REPORT_SCHEMA = Path("schemas/paper-trading-acceptance-report-v1.schema.json")
GENERATED_REPORT = Path("build/dev/cpp/integration/acceptance-report.json")
GENERATED_HUMAN_REPORT = Path("build/dev/cpp/integration/system-report.md")
EXPECTED_SCENARIOS = (
    "ordinary-midday",
    "market-open",
    "market-close",
    "earnings-release",
    "cpi-release",
    "breaking-negative-news",
    "false-rumor-correction",
    "index-rebalance",
    "hidden-liquidity-replenishment",
    "trading-halt-reopening",
    "feed-gap",
    "clock-degradation",
    "model-timeout",
    "risk-service-restart",
    "gateway-disconnect",
    "split-brain-attempt",
)
HASH64 = re.compile(r"^0x[0-9a-f]{16}$")


def test_native_acceptance_runner_emits_complete_deterministic_report() -> None:
    """Validate evidence emitted by the preceding native CTest gate."""
    assert GENERATED_REPORT.is_file(), "native CTest must run before pytest"
    report = cast(
        "dict[str, Any]", json.loads(GENERATED_REPORT.read_text(encoding="utf-8"))
    )
    schema = cast(
        "dict[str, Any]", json.loads(REPORT_SCHEMA.read_text(encoding="utf-8"))
    )
    assert set(report) == set(schema["required"])
    assert report["mode"] == "PAPER"
    assert report["live_trading_compiled"] is False
    assert report["passed"] is True
    assert HASH64.fullmatch(report["report_hash"])

    scenarios = cast("list[dict[str, Any]]", report["scenarios"])
    assert tuple(item["name"] for item in scenarios) == EXPECTED_SCENARIOS
    assert [item["ordinal"] for item in scenarios] == list(range(1, 17))
    assert all(item["passed"] is True for item in scenarios)
    assert all(
        item["outcome_hash"] == item["replay_outcome_hash"] for item in scenarios
    )
    assert all(item["checks"]["no_risk_bypass"] is True for item in scenarios)
    assert all(
        item["counters"]["gateway_commands"] <= item["counters"]["risk_approvals"]
        for item in scenarios
    )
    assert all(value is True for value in report["acceptance"].values())

    text = GENERATED_HUMAN_REPORT.read_text(encoding="utf-8")
    assert "Overall: **PASS**" in text
    assert "Live trading compiled: `false`" in text
    assert all(name in text for name in EXPECTED_SCENARIOS)


def test_report_schema_and_slurm_launcher_are_paper_only() -> None:
    """Keep the machine contract and batch launcher fail-closed."""
    schema = cast(
        "dict[str, Any]", json.loads(REPORT_SCHEMA.read_text(encoding="utf-8"))
    )
    properties = cast("dict[str, Any]", schema["properties"])
    assert properties["mode"] == {"const": "PAPER"}
    assert properties["live_trading_compiled"] == {"const": False}
    scenario = schema["$defs"]["scenario"]
    assert tuple(scenario["properties"]["name"]["enum"]) == EXPECTED_SCENARIOS

    launcher = Path("tools/slurm/paper-integration.sbatch").read_text(encoding="utf-8")
    assert "#SBATCH --partition=parallel" in launcher
    assert "make paper-integration" in launcher
    assert "live" not in launcher.lower()


def test_mandatory_explanation_is_accepted_before_gateway_submission() -> None:
    """An order must never outrun its mandatory reproducibility evidence."""
    source = Path("cpp/integration/src/paper_acceptance.cpp").read_text(
        encoding="utf-8"
    )
    decision_path = source[
        source.index("execution::GatewayRequest gateway_request") : source.index(
            "execution::GatewayEvent acknowledgement"
        )
    ]
    assert decision_path.index("publish_explanation") < decision_path.index(
        "gateway->send_order"
    )


def test_live_transmission_boundary_requires_a_bound_capability() -> None:
    """A future live adapter may not expose a frame-only send primitive."""
    header = Path("cpp/execution/include/aegis/execution/interfaces.hpp").read_text(
        encoding="utf-8"
    )
    live_boundary = header[header.index("class ILiveTransmissionAdapter") :]
    assert "VerifiedLiveTransmissionCapability& capability" in live_boundary
    assert "transmit_verified" in live_boundary
    assert "capability.consume" in live_boundary
