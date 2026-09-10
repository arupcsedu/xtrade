"""Non-destructive administrative CLI for bounded POC data-root controls."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from collections.abc import Sequence

from aegis_mx_research.data_repository import (
    DEFAULT_DATA_ROOT,
    DataRepository,
    QuotaEvidence,
    StorageAuditContext,
    StorageError,
    StoragePolicy,
    StorageRequest,
    make_audit_record,
)


def _add_quota_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--quota-limit-bytes", type=int)
    parser.add_argument("--quota-used-bytes", type=int)
    parser.add_argument("--quota-source")
    parser.add_argument("--quota-observed-at-utc")
    parser.add_argument("--quota-authoritative", action="store_true")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="aegis-data",
        description=(
            "Fail-closed local controls for the bounded forecasting POC data root."
        ),
    )
    parser.add_argument("--data-root", type=Path, default=DEFAULT_DATA_ROOT)
    subcommands = parser.add_subparsers(dest="command", required=True)

    subcommands.add_parser("usage", help="Report complete logical usage.")

    estimate = subcommands.add_parser(
        "estimate", help="Estimate peak use and request no write authority."
    )
    estimate.add_argument("--operation-id", default="cli-estimate")
    estimate.add_argument("--output-bytes", required=True, type=int)
    estimate.add_argument("--temporary-bytes", default=0, type=int)
    estimate.add_argument("--retry-overhead-bytes", default=0, type=int)
    _add_quota_arguments(estimate)

    subcommands.add_parser("verify", help="Verify every local manifest and object.")

    cleanup = subcommands.add_parser(
        "cleanup-plan", help="Plan operator-reviewed cleanup without deleting data."
    )
    cleanup.add_argument("--target-bytes", type=int)
    return parser


def _quota(arguments: argparse.Namespace) -> QuotaEvidence:
    return QuotaEvidence(
        limit_bytes=arguments.quota_limit_bytes,
        used_bytes=arguments.quota_used_bytes,
        source=arguments.quota_source,
        observed_at_utc=arguments.quota_observed_at_utc,
        authoritative=arguments.quota_authoritative,
    )


def _emit(
    *,
    command: str,
    report: dict[str, Any],
    repository: DataRepository,
    outcome: str,
    reasons: Sequence[str] = (),
) -> None:
    audit = make_audit_record(
        StorageAuditContext(
            operation=f"aegis-data {command}",
            outcome=outcome,
            data_root=repository.root,
            policy_sha256=repository.policy.sha256,
            reason_codes=tuple(reasons),
        ),
        report,
    )
    sys.stdout.write(
        json.dumps(
            {
                "audit": audit.to_dict(),
                "command": command,
                "report": report,
            },
            indent=2,
            sort_keys=True,
        )
        + "\n"
    )


def _execute(
    parsed: argparse.Namespace, repository: DataRepository, command: str
) -> int:
    repository.initialize()
    if command == "usage":
        report = repository.usage().to_dict()
        _emit(
            command=command,
            report=report,
            repository=repository,
            outcome="SUCCEEDED",
        )
        return 0
    if command == "estimate":
        request = StorageRequest(
            parsed.operation_id,
            parsed.output_bytes,
            parsed.temporary_bytes,
            parsed.retry_overhead_bytes,
        )
        decision = repository.estimate(request, _quota(parsed))
        report = decision.to_dict()
        reasons = tuple(reason.value for reason in decision.reasons)
        _emit(
            command=command,
            report=report,
            repository=repository,
            outcome="SUCCEEDED" if decision.admitted else "REJECTED",
            reasons=reasons,
        )
        return 0 if decision.admitted else 2
    if command == "verify":
        verification = repository.verify()
        report = verification.to_dict()
        reasons = tuple(sorted({issue.code for issue in verification.errors}))
        _emit(
            command=command,
            report=report,
            repository=repository,
            outcome="SUCCEEDED" if verification.passed else "REJECTED",
            reasons=reasons,
        )
        return 0 if verification.passed else 1
    plan = repository.cleanup_plan(target_bytes=parsed.target_bytes)
    _emit(
        command=command,
        report=plan.to_dict(),
        repository=repository,
        outcome="SUCCEEDED",
    )
    return 0


def main(arguments: Sequence[str] | None = None) -> int:
    """Run one bounded, non-networked, non-deleting data-root operation."""
    parsed = _parser().parse_args(arguments)
    repository = DataRepository(parsed.data_root, policy=StoragePolicy())
    command = str(parsed.command)
    try:
        status = _execute(parsed, repository, command)
    except StorageError as error:
        report = {"error": {"code": error.code.value, "message": str(error)}}
        sys.stdout.write(
            json.dumps(
                {
                    "audit": make_audit_record(
                        StorageAuditContext(
                            operation=f"aegis-data {command}",
                            outcome="REJECTED",
                            data_root=repository.root,
                            policy_sha256=repository.policy.sha256,
                            reason_codes=(error.code.value,),
                        ),
                        report,
                    ).to_dict(),
                    "command": command,
                    "error": report["error"],
                },
                indent=2,
                sort_keys=True,
            )
            + "\n"
        )
        return 1
    return status


def cli_main() -> int:
    """Installed ``aegis-data`` entry point."""
    return main()


if __name__ == "__main__":
    raise SystemExit(cli_main())
