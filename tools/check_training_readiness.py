"""Operator command for fail-closed forecasting training admission."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import TYPE_CHECKING, Final

from aegis_mx_research.data_repository import DataRepository
from aegis_mx_research.real_minute_pipeline import _quota_evidence
from aegis_mx_research.training_readiness import (
    MIN_VALID_LABELS_PER_SYMBOL_HORIZON,
    TrainingReadinessCode,
    TrainingReadinessError,
    assess_training_readiness,
    capture_git_provenance,
    publish_training_readiness,
)

if TYPE_CHECKING:
    from collections.abc import Sequence

DEFAULT_DATA_ROOT: Final = Path("/scratch/djy8hg/aegis_mx_poc_data")
DEFAULT_REPO_ROOT: Final = Path("/scratch/djy8hg/xtrade")
DEFAULT_TICKER_PATH: Final = DEFAULT_REPO_ROOT / "ticker.txt"
DEFAULT_BACKFILL_REPORT: Final = (
    DEFAULT_DATA_ROOT / "reports/alpaca-iex-minute/backfill/report.json"
)
DEFAULT_PROMOTION_REPORT: Final = (
    DEFAULT_DATA_ROOT / "reports/canonical-minute/alpaca-backfill-v1.json"
)


def _resolve_manifest(data_root: Path, supplied: Path | None) -> Path:
    if supplied is not None:
        return supplied
    directory = data_root / "datasets/feature-poc/manifests"
    candidates = tuple(sorted(directory.glob("*.json")))
    if len(candidates) != 1:
        msg = "exactly one feature dataset manifest is required; pass --manifest"
        raise ValueError(msg)
    return candidates[0]


def main(arguments: Sequence[str] | None = None) -> int:
    """Assess inputs and publish only with an explicit execution flag."""
    parser = argparse.ArgumentParser(prog="aegis-training-readiness")
    parser.add_argument("--data-root", type=Path, default=DEFAULT_DATA_ROOT)
    parser.add_argument("--repo-root", type=Path, default=DEFAULT_REPO_ROOT)
    parser.add_argument("--ticker-file", type=Path, default=DEFAULT_TICKER_PATH)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--backfill-report", type=Path, default=DEFAULT_BACKFILL_REPORT)
    parser.add_argument(
        "--promotion-report", type=Path, default=DEFAULT_PROMOTION_REPORT
    )
    parser.add_argument("--source-approval", type=Path)
    parser.add_argument(
        "--minimum-valid-labels",
        type=int,
        default=MIN_VALID_LABELS_PER_SYMBOL_HORIZON,
    )
    parser.add_argument("--skip-object-verification", action="store_true")
    parser.add_argument("--execute", action="store_true")
    parsed = parser.parse_args(arguments)
    manifest = _resolve_manifest(parsed.data_root, parsed.manifest)
    provenance = capture_git_provenance(parsed.repo_root)
    report = assess_training_readiness(
        data_root=parsed.data_root,
        dataset_manifest_path=manifest,
        backfill_report_path=parsed.backfill_report,
        promotion_report_path=parsed.promotion_report,
        source_approval_path=(
            parsed.source_approval
            if parsed.source_approval is not None
            else parsed.data_root
            / "manifests/approvals/alpaca-iex-academic-approval-v1.json"
        ),
        ticker_path=parsed.ticker_file,
        provenance=provenance,
        verify_objects=not parsed.skip_object_verification,
        minimum_valid=parsed.minimum_valid_labels,
    )
    output: dict[str, object] = dict(report)
    output["network_access_performed"] = False
    output["report_published"] = False
    if parsed.execute:
        if report["status"] != "READY_FOR_INFRASTRUCTURE_VALIDATION":
            raise TrainingReadinessError(
                code=TrainingReadinessCode.INVALID_CONFIGURATION,
                message="blocked training readiness report cannot be published",
            )
        repository = DataRepository(parsed.data_root)
        repository.initialize()
        path = publish_training_readiness(
            repository, _quota_evidence(parsed.data_root), report
        )
        output["report_path"] = str(path)
        output["report_published"] = True
    print(json.dumps(output, indent=2, sort_keys=True))
    return 0


def cli_main() -> int:
    """Console entry point."""
    return main()


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(cli_main())
