#!/usr/bin/env python3
"""Emit fail-closed evidence that the operational package remains non-live."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

from tools import edge_deployment

SCHEMA_VERSION = "1.0"
BLOCKER_PATTERN = re.compile(r"^\| (PRD-B[0-9]{3}) \|", re.MULTILINE)


class ReadinessError(RuntimeError):
    """Raised when required evidence is missing or malformed."""


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        message = f"cannot read JSON evidence {path}: {error}"
        raise ReadinessError(message) from error
    if not isinstance(value, dict):
        message = f"JSON evidence is not an object: {path}"
        raise ReadinessError(message)
    return value


def _open_blockers(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8")
    start = text.find("## Open BLOCKERs")
    end = text.find("## Closed findings")
    if start < 0 or end <= start:
        message = "production blocker sections are missing"
        raise ReadinessError(message)
    return BLOCKER_PATTERN.findall(text[start:end])


def evaluate(
    repository: Path, operator_report_path: Path, build_dir: Path
) -> dict[str, Any]:
    """Evaluate independent repository and runtime evidence without mutation."""
    operator = _load_json(operator_report_path)
    audit_path = operator_report_path.parent / str(operator["audit_extract"]["path"])
    build_metadata_path = build_dir / "build-metadata.json"
    cache_path = build_dir / "CMakeCache.txt"
    build_metadata = _load_json(build_metadata_path)
    cache = cache_path.read_text(encoding="utf-8")
    cmake = (repository / "CMakeLists.txt").read_text(encoding="utf-8")
    gateway_unit = (
        repository / "infra/edge/systemd/aegis-paper-gateway.service"
    ).read_text(encoding="utf-8")
    config_validation = (repository / "control/config_service/validation.go").read_text(
        encoding="utf-8"
    )

    profile_evidence: list[dict[str, Any]] = []
    profiles_safe = True
    for name in edge_deployment.PROFILE_NAMES:
        profile_path = repository / "infra/edge/profiles" / f"{name}.json"
        profile = _load_json(profile_path)
        safe = (
            profile.get("trading_mode") in {"SIMULATION", "PAPER"}
            and profile.get("live_transmission_enabled") is False
            and profile.get("automatic_activation") is False
        )
        profiles_safe = profiles_safe and safe
        profile_evidence.append(
            {
                "name": name,
                "sha256": _sha256(profile_path),
                "trading_mode": profile.get("trading_mode"),
                "live_transmission_enabled": profile.get("live_transmission_enabled"),
                "automatic_activation": profile.get("automatic_activation"),
                "safe": safe,
            }
        )

    audit_hash_matches = (
        audit_path.is_file()
        and _sha256(audit_path) == operator["audit_extract"]["sha256"]
    )
    default_live_option = (
        "option(\n"
        "  AEGIS_ENABLE_LIVE_TRADING\n"
        '  "Compile the live-only adapter boundary; does not enable transmission"\n'
        "  OFF\n"
        ")"
    )
    checks = {
        "cmake_default_live_option_off": (default_live_option in cmake),
        "configured_build_live_option_off": (
            "AEGIS_ENABLE_LIVE_TRADING:BOOL=OFF" in cache
        ),
        "build_metadata_live_capability_false": (
            build_metadata.get("live_trading_capable") is False
        ),
        "all_edge_profiles_non_live": profiles_safe,
        "gateway_unit_has_disabled_live_guard": (
            'ExecStartPre=/usr/bin/test "${AEGIS_LIVE_TRANSMISSION}" = "DISABLED"'
            in gateway_unit
            and "aegis-paper-gateway" in gateway_unit
        ),
        "control_plane_rejects_live_mode": (
            "venue.Mode == TradingModeLive" in config_validation
            and "ErrLiveModeUnavailable" in config_validation
        ),
        "operator_simulation_paper_only": (
            operator.get("mode") == "PAPER"
            and operator.get("live_trading_compiled") is False
            and operator.get("production_activation_attempted") is False
            and operator.get("passed") is True
        ),
        "operator_audit_extract_hash_matches": audit_hash_matches,
    }
    blockers = _open_blockers(repository / "docs/reviews/blockers.md")
    passed = all(checks.values()) and bool(blockers)
    return {
        "schema_version": SCHEMA_VERSION,
        "kind": "live_mode_disabled_evidence",
        "generated_at_wall_utc": datetime.now(UTC).isoformat(),
        "source_revision": build_metadata.get("source_revision", "unknown"),
        "build": {
            "metadata_path": str(build_metadata_path),
            "metadata_sha256": _sha256(build_metadata_path),
            "build_type": build_metadata.get("build_type"),
            "live_trading_capable": build_metadata.get("live_trading_capable"),
        },
        "operator_simulation": {
            "report_path": str(operator_report_path),
            "report_sha256": _sha256(operator_report_path),
            "audit_path": str(audit_path),
            "audit_sha256": _sha256(audit_path) if audit_path.is_file() else "",
        },
        "edge_profiles": profile_evidence,
        "checks": checks,
        "open_production_blockers": blockers,
        "live_mode_enabled": False,
        "production_activation_performed": False,
        "production_ready": False,
        "activation_status": "PROHIBITED",
        "limitations": [
            (
                "This evidence covers repository-owned builds, profiles, guards, "
                "and the synthetic PAPER operator drill; it does not certify a "
                "target site or external integration."
            ),
            (
                "Open production blockers remain authoritative. A passing report "
                "proves default-off behavior, not production readiness or "
                "permission to activate live trading."
            ),
        ],
        "passed": passed,
    }


def _markdown(report: dict[str, Any]) -> str:
    status = "PASS" if report["passed"] else "FAIL"
    checks = "\n".join(
        f"- [{'x' if value else ' '}] `{name}`"
        for name, value in report["checks"].items()
    )
    blockers = ", ".join(report["open_production_blockers"])
    activation_performed = str(report["production_activation_performed"]).lower()
    return f"""# Live-mode-disabled evidence

Evidence result: **{status}**

- Activation status: **{report["activation_status"]}**
- Live mode enabled: `{str(report["live_mode_enabled"]).lower()}`
- Production activation performed: `{activation_performed}`
- Production ready: `{str(report["production_ready"]).lower()}`
- Source revision: `{report["source_revision"]}`
- Open production blockers: {blockers}

## Verified controls

{checks}

## Interpretation

This is positive evidence that the inspected build and checked-in deployment
profiles remain non-live. It is not an activation record and grants no trading
authority. Open production blockers preserve the `STOP / NO-GO` decision.
"""


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository", type=Path, default=Path.cwd())
    parser.add_argument("--operator-report", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--human", type=Path, required=True)
    return parser


def main() -> int:
    """Generate machine and human evidence and fail closed on any false check."""
    args = _parser().parse_args()
    try:
        report = evaluate(
            args.repository.resolve(),
            args.operator_report.resolve(),
            args.build_dir.resolve(),
        )
    except (KeyError, OSError, ReadinessError, TypeError, ValueError) as error:
        message = f"operational readiness evidence failed closed: {error}"
        raise SystemExit(message) from error
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.human.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    args.human.write_text(_markdown(report), encoding="utf-8")
    print(
        "live-mode evidence "
        f"passed={str(report['passed']).lower()} "
        f"activation_status={report['activation_status']} "
        f"blockers={len(report['open_production_blockers'])}"
    )
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
