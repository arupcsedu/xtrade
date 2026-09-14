"""Compile accepted offline sources into a point-in-time feature-event snapshot."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import TYPE_CHECKING, Final, cast

from aegis_mx_research.feature_events import (
    build_feature_event_snapshot,
    publish_feature_event_snapshot,
)
from aegis_mx_research.real_minute_pipeline import _read_document

from tools.build_real_feature_dataset import _universe_and_calendar

if TYPE_CHECKING:
    from collections.abc import Sequence

DEFAULT_DATA_ROOT: Final = Path("/scratch/djy8hg/aegis_mx_poc_data")
DEFAULT_BACKFILL_REPORT: Final = (
    DEFAULT_DATA_ROOT / "reports/alpaca-iex-minute/backfill/report.json"
)
DEFAULT_SEC_COVERAGE: Final = (
    DEFAULT_DATA_ROOT / "reports/sec-edgar/prompt-53/issuer-filing-coverage-v1_1.json"
)


def cli_main(arguments: Sequence[str] | None = None) -> int:
    """Build deterministically; require explicit execution to publish."""
    parser = argparse.ArgumentParser(prog="aegis-feature-events")
    parser.add_argument("--data-root", type=Path, default=DEFAULT_DATA_ROOT)
    parser.add_argument("--backfill-report", type=Path, default=DEFAULT_BACKFILL_REPORT)
    parser.add_argument("--sec-coverage", type=Path, default=DEFAULT_SEC_COVERAGE)
    parser.add_argument("--without-sec", action="store_true")
    parser.add_argument("--alfred-run-report", type=Path)
    parser.add_argument("--execute", action="store_true")
    parsed = parser.parse_args(arguments)
    backfill = _read_document(parsed.backfill_report, "Alpaca backfill report")
    universe, _ = _universe_and_calendar(backfill)
    snapshot = build_feature_event_snapshot(
        parsed.data_root,
        universe,
        source_universe_sha256=str(backfill["universe_snapshot_sha256"]),
        sec_coverage_report=None if parsed.without_sec else parsed.sec_coverage,
        alfred_run_report=parsed.alfred_run_report,
    )
    output: dict[str, object] = {
        "coverage": snapshot["coverage"],
        "dry_run": not parsed.execute,
        "event_count": len(cast("list[object]", snapshot["events"])),
        "live_trading_capable": False,
        "network_access_performed": False,
        "snapshot_sha256": snapshot["snapshot_sha256"],
        "status": "PLANNED" if not parsed.execute else "COMPLETED",
    }
    if parsed.execute:
        output["snapshot_path"] = str(
            publish_feature_event_snapshot(parsed.data_root, snapshot)
        )
    print(json.dumps(output, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(cli_main())
