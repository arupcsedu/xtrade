"""Administrative CLI for bounded POC data-root controls and explicit migration."""

from __future__ import annotations

import argparse
import json
import sys
from datetime import date
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
from aegis_mx_research.forecast_contracts import (
    UnresolvedInstrumentResolver,
    load_ticker_universe,
)
from aegis_mx_research.ingestion import (
    DEFAULT_INGESTION_SEED,
    DataEntitlement,
    DataEntitlementState,
    DataProvider,
    FetchContractError,
    FetchErrorCode,
    FetchRequest,
    FetchStatus,
    FilesystemReplayProvider,
    IngestionCoordinator,
    SyntheticMinuteBarProvider,
)


def _add_quota_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--quota-limit-bytes", type=int)
    parser.add_argument("--quota-used-bytes", type=int)
    parser.add_argument("--quota-source")
    parser.add_argument("--quota-observed-at-utc")
    parser.add_argument("--quota-authoritative", action="store_true")


def _date_argument(value: str) -> date:
    try:
        return date.fromisoformat(value)
    except ValueError as error:
        message = "date must use YYYY-MM-DD"
        raise argparse.ArgumentTypeError(message) from error


def _add_fetch_arguments(
    parser: argparse.ArgumentParser, *, permit_execute: bool
) -> None:
    parser.add_argument(
        "--provider", choices=("filesystem", "synthetic"), required=True
    )
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--start", required=True, type=_date_argument)
    parser.add_argument("--end", required=True, type=_date_argument)
    parser.add_argument("--ticker", action="append", default=[])
    parser.add_argument("--filesystem-root", type=Path)
    parser.add_argument("--request-id", default="cli-ingestion")
    parser.add_argument("--maximum-object-bytes", type=int, default=50_000_000)
    parser.add_argument("--maximum-total-bytes", type=int, default=10_000_000_000)
    parser.add_argument("--maximum-concurrency", type=int, default=2)
    parser.add_argument("--chunk-bytes", type=int, default=1_048_576)
    parser.add_argument("--request-timeout-ns", type=int, default=30_000_000_000)
    parser.add_argument("--seed", type=int, default=DEFAULT_INGESTION_SEED)
    if permit_execute:
        parser.add_argument(
            "--execute",
            action="store_true",
            help="Permit admitted local fixture writes; remote adapters remain absent.",
        )
    _add_quota_arguments(parser)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="aegis-data",
        description=(
            "Fail-closed local controls for the bounded forecasting POC data root."
        ),
    )
    parser.add_argument("--data-root", type=Path, default=DEFAULT_DATA_ROOT)
    subcommands = parser.add_subparsers(dest="command", required=True)

    migrate = subcommands.add_parser(
        "migrate-policy",
        help="Plan by default; explicitly migrate the reviewed v2 marker to v3.",
    )
    migrate.add_argument("--expected-current-policy-sha256", required=True)
    migrate.add_argument("--execute", action="store_true")

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

    plan_fetch = subcommands.add_parser(
        "plan-fetch", help="Plan a finite local/mock fetch without fetching bytes."
    )
    _add_fetch_arguments(plan_fetch, permit_execute=False)

    fetch = subcommands.add_parser(
        "fetch", help="Plan by default; require --execute before writing objects."
    )
    _add_fetch_arguments(fetch, permit_execute=True)

    resume = subcommands.add_parser(
        "resume-fetch", help="Verify and resume a durable local fixture checkpoint."
    )
    _add_fetch_arguments(resume, permit_execute=True)

    inspect = subcommands.add_parser(
        "inspect-source", help="Authenticate and print one local source manifest."
    )
    inspect.add_argument("--manifest-id", required=True)
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


def _execute(  # noqa: PLR0911
    parsed: argparse.Namespace, repository: DataRepository, command: str
) -> int:
    if command == "migrate-policy":
        report = repository.migrate_policy_v2_to_v3(
            expected_current_policy_sha256=parsed.expected_current_policy_sha256,
            execute=parsed.execute,
        )
        _emit(
            command=command,
            report=report,
            repository=repository,
            outcome="SUCCEEDED",
        )
        return 0
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
    if command in {"plan-fetch", "fetch", "resume-fetch"}:
        return _execute_ingestion(parsed, repository, command)
    if command == "inspect-source":
        manifest = repository.load_manifest(parsed.manifest_id)
        report = {
            "manifest_id": manifest.manifest_id,
            "manifest_type": type(manifest).__name__,
            "payload": manifest.to_payload(),
        }
        _emit(
            command=command,
            report=report,
            repository=repository,
            outcome="SUCCEEDED",
        )
        return 0
    plan = repository.cleanup_plan(target_bytes=parsed.target_bytes)
    _emit(
        command=command,
        report=plan.to_dict(),
        repository=repository,
        outcome="SUCCEEDED",
    )
    return 0


def _execute_ingestion(
    parsed: argparse.Namespace, repository: DataRepository, command: str
) -> int:
    snapshot = load_ticker_universe(UnresolvedInstrumentResolver())
    available = tuple(entry.symbol for entry in snapshot.ordered_entries)
    requested = tuple(parsed.ticker) if parsed.ticker else available
    if not set(requested).issubset(available):
        unknown = sorted(set(requested) - set(available))
        raise FetchContractError(
            code=FetchErrorCode.INVALID_REQUEST,
            message=f"ticker filter is outside ticker.txt: {unknown}",
        )
    execute = bool(getattr(parsed, "execute", False))
    provider: DataProvider
    if parsed.provider == "synthetic":
        provider = SyntheticMinuteBarProvider(dataset_id=parsed.dataset)
    else:
        if parsed.filesystem_root is None:
            raise FetchContractError(
                code=FetchErrorCode.INVALID_REQUEST,
                message="filesystem provider requires --filesystem-root",
            )
        provider = FilesystemReplayProvider(
            root=parsed.filesystem_root,
            dataset_id=parsed.dataset,
        )
    request = FetchRequest(
        request_id=parsed.request_id,
        provider_id=provider.provider_id,
        dataset_id=provider.dataset_id,
        start_date=parsed.start,
        end_date=parsed.end,
        tickers=requested,
        universe_snapshot_sha256=snapshot.universe_snapshot_sha256.hex(),
        execute=execute,
        maximum_object_bytes=parsed.maximum_object_bytes,
        maximum_total_bytes=parsed.maximum_total_bytes,
        maximum_concurrency=parsed.maximum_concurrency,
        chunk_bytes=parsed.chunk_bytes,
        request_timeout_ns=parsed.request_timeout_ns,
        deterministic_seed=parsed.seed,
    )
    coordinator = IngestionCoordinator(
        repository,
        provider,
        DataEntitlement(
            source_id=provider.provider_id,
            dataset_id=provider.dataset_id,
            state=DataEntitlementState.LOCAL_TEST_ONLY,
        ),
    )
    quota = _quota(parsed)
    plan = coordinator.plan(request, quota)
    if command == "plan-fetch" or not execute:
        report = plan.to_dict()
        outcome = "SUCCEEDED" if plan.admission.admitted else "REJECTED"
        reasons = tuple(reason.value for reason in plan.admission.reasons)
        _emit(
            command=command,
            report=report,
            repository=repository,
            outcome=outcome,
            reasons=reasons,
        )
        return 0 if plan.admission.admitted else 2
    result = coordinator.execute(plan, quota, resume=command == "resume-fetch")
    report = result.to_dict()
    succeeded = result.status is FetchStatus.COMPLETED
    reasons = tuple(
        sorted(
            {
                item.error_code.value
                for item in result.objects
                if item.error_code is not None
            }
        )
    )
    _emit(
        command=command,
        report=report,
        repository=repository,
        outcome="SUCCEEDED" if succeeded else "REJECTED",
        reasons=reasons,
    )
    return 0 if succeeded else 1


def main(arguments: Sequence[str] | None = None) -> int:
    """Run one bounded operation; migration requires an explicit execution flag."""
    parsed = _parser().parse_args(arguments)
    repository = DataRepository(parsed.data_root, policy=StoragePolicy())
    command = str(parsed.command)
    try:
        status = _execute(parsed, repository, command)
    except (FetchContractError, StorageError) as error:
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


if __name__ == "__main__":  # pragma: no cover - exercised through cli_main
    raise SystemExit(cli_main())
