"""The operator drill is PAPER-only, deterministic, and hash-chain auditable."""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path
from typing import Any, cast

REPORT_PATH = Path("build/dev/cpp/integration/operator-simulation.json")
AUDIT_PATH = Path("build/dev/cpp/integration/operator-audit.ndjson")
SCHEMA_PATH = Path("schemas/operator-simulation-report-v1.schema.json")
AUDIT_SCHEMA_PATH = Path("schemas/operator-drill-audit-record-v1.schema.json")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
SCOPES = ("symbol", "strategy", "venue", "firm")


def _canonical_record(record: dict[str, Any]) -> bytes:
    fields = (
        record["sequence"],
        record["previous_sha256"],
        record["event"],
        record["scope"],
        record["command_sequence"],
        "true" if record["operator_authorized"] else "false",
        record["update_status"],
        record["risk_journal_sequence"],
        record["risk_decision_hash"],
    )
    return "|".join(str(value) for value in fields).encode()


def test_native_operator_simulation_emits_verified_paper_evidence() -> None:
    assert REPORT_PATH.is_file(), "native CTest must generate the report first"
    assert AUDIT_PATH.is_file(), "native CTest must generate the audit first"
    report = cast("dict[str, Any]", json.loads(REPORT_PATH.read_text()))
    schema = cast("dict[str, Any]", json.loads(SCHEMA_PATH.read_text()))

    assert set(report) == set(schema["required"])
    assert report["mode"] == "PAPER"
    assert report["live_trading_compiled"] is False
    assert report["production_activation_attempted"] is False
    assert report["seed"] == 20_260_908
    assert report["passed"] is True
    assert all(report["checks"].values())
    assert tuple(item["scope"] for item in report["scopes"]) == SCOPES
    assert all(item["passed"] is True for item in report["scopes"])

    raw = AUDIT_PATH.read_bytes()
    audit = report["audit_extract"]
    assert hashlib.sha256(raw).hexdigest() == audit["sha256"]
    records = [json.loads(line) for line in raw.splitlines()]
    audit_schema = cast("dict[str, Any]", json.loads(AUDIT_SCHEMA_PATH.read_text()))
    assert len(records) == audit["records"] == 21

    previous = "0" * 64
    for sequence, record in enumerate(records, start=1):
        assert set(record) == set(audit_schema["required"])
        assert record["schema_version"] == "1.0"
        assert record["kind"] == "operator_drill_audit"
        assert record["sequence"] == sequence
        assert record["previous_sha256"] == previous
        assert isinstance(record["risk_decision_hash"], str)
        calculated = hashlib.sha256(_canonical_record(record)).hexdigest()
        assert SHA256.fullmatch(record["record_sha256"])
        assert calculated == record["record_sha256"]
        previous = calculated
    assert previous == audit["chain_final_sha256"]

    risk_sequences = [
        record["risk_journal_sequence"]
        for record in records
        if record["risk_journal_sequence"] != 0
    ]
    assert risk_sequences == list(range(1, 10))
    assert audit["risk_decisions"] == len(risk_sequences)


def test_operator_simulation_cannot_reach_activation_or_gateway_boundaries() -> None:
    source = Path("cpp/integration/src/operator_simulation.cpp").read_text()
    header = Path("cpp/integration/include/aegis/integration/operator_simulation.hpp")
    combined = source + header.read_text()
    assert "aegis/execution" not in combined
    assert "aegis/oms" not in combined
    assert "send_order" not in combined
    assert "activate_live" not in combined

    cmake = Path("cpp/integration/CMakeLists.txt").read_text()
    operator_target = cmake.split("add_library(\n  aegis_operator_simulation_core", 1)[
        1
    ]
    operator_target = operator_target.split("add_executable(aegis_paper_acceptance", 1)[
        0
    ]
    assert "aegis::oms" not in operator_target
    assert "aegis::execution" not in operator_target
    assert "aegis::paper_integration" not in operator_target
    assert "PUBLIC aegis::common aegis::risk" in operator_target

    schema = cast("dict[str, Any]", json.loads(SCHEMA_PATH.read_text()))
    properties = schema["properties"]
    assert properties["mode"] == {"const": "PAPER"}
    assert properties["live_trading_compiled"] == {"const": False}
    assert properties["production_activation_attempted"] == {"const": False}
