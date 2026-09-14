"""Build Prompt 57 features from an accepted real canonical-minute promotion."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import TYPE_CHECKING, Final, cast

from aegis_mx_intelligence import ConfigurationVersion, Identifier128
from aegis_mx_research.data_repository import DataRepository, PartitionManifest
from aegis_mx_research.feature_dataset import (
    CanonicalPartitionInput,
    FeatureDatasetBuilder,
    ParquetCanonicalSource,
    build_and_publish_feature_dataset,
)
from aegis_mx_research.feature_events import load_feature_event_snapshot
from aegis_mx_research.forecast_contracts import (
    ExchangeCalendar,
    InstrumentResolutionCode,
    StaticInstrumentResolver,
    TradingSession,
    UniverseSnapshot,
    UnresolvedInstrumentResolver,
    load_ticker_universe,
)
from aegis_mx_research.real_minute_pipeline import (
    _parse_utc_ns,
    _quota_evidence,
    _read_document,
)
from aegis_mx_research.reference_data import alpaca_instrument_id

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence

DEFAULT_DATA_ROOT: Final = Path("/scratch/djy8hg/aegis_mx_poc_data")
DEFAULT_BACKFILL_REPORT: Final = (
    DEFAULT_DATA_ROOT / "reports/alpaca-iex-minute/backfill/report.json"
)
DEFAULT_PROMOTION_REPORT: Final = (
    DEFAULT_DATA_ROOT / "reports/canonical-minute/alpaca-backfill-v1.json"
)
MAX_PARTITIONS: Final = 10_000
SHA256_DIGEST_BYTES: Final = 32


def _digest(value: object) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode("ascii")
    return hashlib.sha256(encoded).hexdigest()


def _configuration_version(value: str) -> ConfigurationVersion:
    raw = bytes.fromhex(value)
    if len(raw) != SHA256_DIGEST_BYTES:
        msg = "calendar SHA-256 is malformed"
        raise ValueError(msg)
    return ConfigurationVersion(
        Identifier128(int.from_bytes(raw[:8], "big"), int.from_bytes(raw[8:16], "big"))
    )


def _session_id(row: Mapping[str, object]) -> str:
    opened = _parse_utc_ns(row["open_utc"], "session open")
    closed = _parse_utc_ns(row["close_utc"], "session close")
    return _digest(
        {
            "calendar": "ALPACA-RETROSPECTIVE-US-EQUITIES",
            "close_ns": closed,
            "date": row["date"],
            "open_ns": opened,
        }
    )[:32]


def _universe_and_calendar(
    backfill: Mapping[str, object],
) -> tuple[UniverseSnapshot, ExchangeCalendar]:
    raw_assets = backfill.get("assets")
    raw_sessions = backfill.get("sessions")
    calendar_sha256 = backfill.get("calendar_sha256")
    if (
        not isinstance(raw_assets, dict)
        or not isinstance(raw_sessions, list)
        or not isinstance(calendar_sha256, str)
    ):
        msg = "backfill reference or calendar evidence is malformed"
        raise TypeError(msg)
    assets = cast("Mapping[str, Mapping[str, object]]", raw_assets)
    resolved = {
        symbol: alpaca_instrument_id(cast("str", asset["asset_id"]))
        for symbol, asset in assets.items()
        if asset.get("status") == "RESOLVED" and isinstance(asset.get("asset_id"), str)
    }
    unresolved = {
        symbol: InstrumentResolutionCode.UNSUPPORTED
        for symbol, asset in assets.items()
        if asset.get("status") != "RESOLVED"
    }
    requested = load_ticker_universe(UnresolvedInstrumentResolver())
    if requested.source_file_sha256.hex() != backfill.get(
        "ticker_source_sha256"
    ) or requested.universe_snapshot_sha256.hex() != backfill.get(
        "universe_snapshot_sha256"
    ):
        msg = "authoritative ticker request snapshot does not match the backfill"
        raise ValueError(msg)
    universe = load_ticker_universe(StaticInstrumentResolver(resolved, unresolved))
    sessions = []
    for raw in raw_sessions:
        if not isinstance(raw, dict):
            msg = "backfill calendar row is malformed"
            raise TypeError(msg)
        row = cast("Mapping[str, object]", raw)
        sessions.append(
            TradingSession(
                _session_id(row),
                _parse_utc_ns(row["open_utc"], "session open"),
                _parse_utc_ns(row["close_utc"], "session close"),
            )
        )
    return universe, ExchangeCalendar(
        "ALPACA-RETRO-XNYS",
        _configuration_version(calendar_sha256),
        tuple(sessions),
    )


def _partition_inputs(
    repository: DataRepository,
    promotion: Mapping[str, object],
    requested_universe_sha256: str,
) -> tuple[CanonicalPartitionInput, ...]:
    identifiers = promotion.get("partition_manifest_ids")
    if not isinstance(identifiers, list) or not 0 < len(identifiers) <= MAX_PARTITIONS:
        msg = "canonical promotion partition list is malformed or out of bounds"
        raise ValueError(msg)
    result = []
    for identifier in identifiers:
        if not isinstance(identifier, str):
            msg = "canonical partition manifest identity is malformed"
            raise TypeError(msg)
        manifest = repository.load_manifest(identifier)
        if (
            not isinstance(manifest, PartitionManifest)
            or manifest.dataset_name != "canonical-minute-poc"
            or manifest.schema_name != "canonical-minute-record"
            or manifest.schema_version != "1.0.0"
            or manifest.universe_snapshot_sha256 != requested_universe_sha256
        ):
            msg = "promotion references a noncanonical partition"
            raise ValueError(msg)
        result.append(
            CanonicalPartitionInput(
                repository.root / manifest.storage_path,
                manifest.object_sha256,
                manifest.manifest_id,
            )
        )
    return tuple(result)


def build_real_feature_dataset(
    data_root: Path,
    backfill_report: Path,
    promotion_report: Path,
    event_snapshot: Path | None = None,
) -> dict[str, object]:
    """Verify real inputs and publish the deterministic Prompt 57 dataset."""
    backfill = _read_document(backfill_report, "Alpaca backfill report")
    promotion = _read_document(promotion_report, "canonical promotion report")
    if promotion.get("status") != "COMPLETED":
        msg = "canonical promotion is not complete"
        raise ValueError(msg)
    repository = DataRepository(data_root)
    repository.initialize()
    universe, calendar = _universe_and_calendar(backfill)
    requested_universe_sha256 = cast("str", backfill["universe_snapshot_sha256"])
    source = ParquetCanonicalSource(
        _partition_inputs(repository, promotion, requested_universe_sha256)
    )
    expected = cast("Mapping[str, object]", promotion["counts"]).get(
        "published_records"
    )
    if source.record_count != expected:
        msg = "canonical source count does not match its promotion report"
        raise ValueError(msg)
    events = (
        None
        if event_snapshot is None
        else load_feature_event_snapshot(
            event_snapshot,
            expected_universe_sha256=universe.universe_snapshot_sha256.hex(),
            expected_source_universe_sha256=requested_universe_sha256,
            expected_instrument_ids=tuple(source.instrument_ids),
        )
    )
    builder = FeatureDatasetBuilder(universe, calendar, events=events)
    published = build_and_publish_feature_dataset(
        repository, _quota_evidence(data_root), builder, source
    )
    return {
        "coverage_rows": len(published.coverage),
        "dataset_id": published.dataset_id,
        "leakage_status": published.leakage.status,
        "event_snapshot_sha256": builder.events.sha256,
        "event_feature_coverage": list(builder.events.coverage_document),
        "live_trading_capable": False,
        "manifest_path": str(published.manifest_path),
        "object_count": published.object_count,
        "sample_count": published.sample_count,
        "session_summary_count": published.session_summary_count,
        "status": "COMPLETED",
    }


def main(arguments: Sequence[str] | None = None) -> int:
    """Plan by default and require an explicit flag for dataset publication."""
    parser = argparse.ArgumentParser(prog="aegis-real-features")
    parser.add_argument("--data-root", type=Path, default=DEFAULT_DATA_ROOT)
    parser.add_argument("--backfill-report", type=Path, default=DEFAULT_BACKFILL_REPORT)
    parser.add_argument(
        "--promotion-report", type=Path, default=DEFAULT_PROMOTION_REPORT
    )
    parser.add_argument("--event-snapshot", type=Path)
    parser.add_argument("--execute", action="store_true")
    parsed = parser.parse_args(arguments)
    if not parsed.execute:
        output = {
            "dry_run": True,
            "live_trading_capable": False,
            "network_access_performed": False,
            "status": "PLANNED",
        }
    else:
        output = build_real_feature_dataset(
            parsed.data_root,
            parsed.backfill_report,
            parsed.promotion_report,
            parsed.event_snapshot,
        )
    print(json.dumps(output, indent=2, sort_keys=True))
    return 0


def cli_main() -> int:
    """Console entry point."""
    return main()


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(cli_main())
