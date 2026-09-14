"""Alpaca standard-API reference download and real minute Parquet promotion."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import signal
import sys
import tempfile
from dataclasses import dataclass
from datetime import UTC, date, datetime
from pathlib import Path
from typing import TYPE_CHECKING, Final, cast
from zoneinfo import ZoneInfo

from aegis_mx_research.alpaca_historical import (
    AlpacaApiClient,
    AlpacaCredentials,
    AlpacaDataError,
    load_source_authorization,
)
from aegis_mx_research.canonical_minute import (
    CURRENCY_NANOS_PER_UNIT,
    CanonicalMinuteNormalizer,
    CanonicalMinuteSpool,
    MinuteNormalizationCode,
    MinuteNormalizationError,
    ResolvedMinuteReference,
    iter_source_minutes,
    publish_partition,
)
from aegis_mx_research.data_repository import (
    BINARY_TIB,
    AdmissionLease,
    DataRepository,
    ManifestLineage,
    ManifestTimeRange,
    QuotaEvidence,
    SourceManifest,
    StorageRequest,
)
from aegis_mx_research.forecast_contracts import (
    UnresolvedInstrumentResolver,
    load_ticker_universe,
)
from aegis_mx_research.reference_data import alpaca_instrument_id

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence

DEFAULT_DATA_ROOT: Final = Path("/scratch/djy8hg/aegis_mx_poc_data")
DEFAULT_BACKFILL_REPORT: Final = (
    DEFAULT_DATA_ROOT / "reports/alpaca-iex-minute/backfill/report.json"
)
DEFAULT_PARENT_POLICY: Final = (
    DEFAULT_DATA_ROOT / "manifests/approvals/alpaca-iex-academic-policy-v1.json"
)
DEFAULT_PARENT_APPROVAL: Final = (
    DEFAULT_DATA_ROOT / "manifests/approvals/alpaca-iex-academic-approval-v1.json"
)
DEFAULT_ACTION_AUTHORIZATION: Final = (
    DEFAULT_DATA_ROOT
    / "manifests/approvals/alpaca-reference-academic-authorization-v1.json"
)
DEFAULT_ACTION_REPORT: Final = (
    DEFAULT_DATA_ROOT / "reports/alpaca-reference/corporate-actions-v1.json"
)
DEFAULT_PROMOTION_REPORT: Final = (
    DEFAULT_DATA_ROOT / "reports/canonical-minute/alpaca-backfill-v1.json"
)
MAX_DOCUMENT_BYTES: Final = 16_000_000
MAX_ACTION_PAGES: Final = 32
MAX_OPERATOR_ID_BYTES: Final = 128
MAX_PAGE_TOKEN_BYTES: Final = 4096
ACTION_DOWNLOAD_BUDGET_BYTES: Final = 10_000_000
CANONICAL_SPOOL_LIMIT_BYTES: Final = 18_000_000_000
PRICE_QUANTUM_CURRENCY_NANOS: Final = 1
NANOSECONDS_PER_DAY: Final = 86_400_000_000_000
_SESSION_ZONE: Final = ZoneInfo("America/New_York")
_STOP_REQUESTED = False


def _canonical_bytes(value: object) -> bytes:
    return json.dumps(
        value, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode("ascii")


def _digest(value: object) -> str:
    return hashlib.sha256(_canonical_bytes(value)).hexdigest()


def _document(body: Mapping[str, object]) -> dict[str, object]:
    value = dict(body)
    value["document_sha256"] = _digest(value)
    return value


def _read_document(path: Path, description: str) -> dict[str, object]:
    try:
        status = path.lstat()
    except OSError as error:
        raise MinuteNormalizationError(
            MinuteNormalizationCode.INVALID_SOURCE,
            f"{description} is unavailable",
        ) from error
    if path.is_symlink() or not path.is_file() or status.st_size > MAX_DOCUMENT_BYTES:
        raise MinuteNormalizationError(
            MinuteNormalizationCode.INVALID_SOURCE,
            f"{description} has unsafe file metadata",
        )
    try:
        payload = path.read_bytes()
        value = json.loads(payload)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise MinuteNormalizationError(
            MinuteNormalizationCode.INVALID_SOURCE,
            f"{description} is unavailable or malformed",
        ) from error
    if not isinstance(value, dict):
        raise MinuteNormalizationError(
            MinuteNormalizationCode.INVALID_SOURCE,
            f"{description} is not a JSON object",
        )
    document = cast("dict[str, object]", value)
    supplied = document.pop("document_sha256", None)
    actual = _digest(document)
    document["document_sha256"] = supplied
    if supplied != actual:
        raise MinuteNormalizationError(
            MinuteNormalizationCode.HASH_MISMATCH,
            f"{description} document hash does not match",
        )
    return document


def _write_immutable_document(path: Path, body: Mapping[str, object]) -> str:
    document = _document(body)
    payload = _canonical_bytes(document) + b"\n"
    path.parent.mkdir(parents=True, mode=0o700, exist_ok=True)
    if path.exists() or path.is_symlink():
        if not path.is_symlink() and path.read_bytes() == payload:
            return cast("str", document["document_sha256"])
        raise MinuteNormalizationError(
            MinuteNormalizationCode.HASH_MISMATCH,
            "immutable report path contains different bytes",
        )
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", dir=path.parent
    )
    temporary = Path(temporary_name)
    try:
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, "wb", closefd=True) as output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
        temporary.replace(path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise
    return cast("str", document["document_sha256"])


def _utc_ns(value: datetime) -> int:
    if value.tzinfo is None:
        raise MinuteNormalizationError(
            MinuteNormalizationCode.TIMESTAMP_DISORDER,
            "timestamp is not timezone aware",
        )
    delta = value.astimezone(UTC) - datetime(1970, 1, 1, tzinfo=UTC)
    return (
        delta.days * NANOSECONDS_PER_DAY
        + delta.seconds * CURRENCY_NANOS_PER_UNIT
        + delta.microseconds * 1_000
    )


def _parse_utc_ns(value: object, field: str) -> int:
    if not isinstance(value, str):
        raise MinuteNormalizationError(
            MinuteNormalizationCode.INVALID_SOURCE, f"{field} is not text"
        )
    try:
        parsed = datetime.fromisoformat(value)
    except ValueError as error:
        raise MinuteNormalizationError(
            MinuteNormalizationCode.INVALID_SOURCE, f"{field} is malformed"
        ) from error
    if parsed.tzinfo is None:
        raise MinuteNormalizationError(
            MinuteNormalizationCode.INVALID_SOURCE, f"{field} lacks a UTC offset"
        )
    return _utc_ns(parsed)


def _quota_evidence(root: Path) -> QuotaEvidence:
    status = os.statvfs(root)
    total = status.f_bsize * status.f_blocks
    free = status.f_bsize * status.f_bfree
    limit = 10 * BINARY_TIB if total == 12 * BINARY_TIB else total
    return QuotaEvidence(
        limit_bytes=limit,
        used_bytes=total - free,
        source="/opt/rci/bin/hdquota -s exact statvfs calculation",
        observed_at_utc=datetime.now(UTC).isoformat(),
        authoritative=True,
    )


def authorize_corporate_actions(  # noqa: PLR0913
    *,
    data_root: Path,
    parent_policy: Path,
    parent_approval: Path,
    output_path: Path,
    operator_id: str,
    expires_at_utc: datetime,
) -> dict[str, object]:
    """Create a narrow child authorization after validating the approved parent."""
    universe = load_ticker_universe(UnresolvedInstrumentResolver())
    parent = load_source_authorization(
        parent_policy,
        parent_approval,
        snapshot=universe,
        data_root=data_root,
    )
    if (
        not operator_id
        or len(operator_id) > MAX_OPERATOR_ID_BYTES
        or expires_at_utc.tzinfo is None
        or expires_at_utc <= datetime.now(UTC)
        or expires_at_utc > parent.expires_at_utc
    ):
        raise MinuteNormalizationError(
            MinuteNormalizationCode.INVALID_SOURCE,
            "reference authorization identity or expiry is invalid",
        )
    body: dict[str, object] = {
        "authorization_basis": (
            "operator-directed standard Alpaca API retrieval required to prevent "
            "corporate-action leakage in the internal academic POC"
        ),
        "data_root": str(data_root),
        "decision": "APPROVED",
        "distribution": "INTERNAL_SINGLE_USER_ONLY",
        "endpoint": "GET https://data.alpaca.markets/v1/corporate-actions",
        "expires_at_utc": expires_at_utc.astimezone(UTC).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "live_trading_capable": False,
        "model_training_authorized": True,
        "network_method": "GET_ONLY",
        "operator_id": operator_id,
        "parent_approval_sha256": parent.approval_sha256,
        "parent_policy_sha256": parent.policy_sha256,
        "purpose": "REFERENCE_VALIDATION_AND_LABEL_INVALIDATION",
        "schema_version": "1.0.0",
        "ticker_source_sha256": universe.source_file_sha256.hex(),
    }
    _write_immutable_document(output_path, body)
    return _read_document(output_path, "corporate-action authorization")


def _validate_action_authorization(
    path: Path,
    *,
    data_root: Path,
    parent_policy: Path,
    parent_approval: Path,
) -> dict[str, object]:
    universe = load_ticker_universe(UnresolvedInstrumentResolver())
    parent = load_source_authorization(
        parent_policy,
        parent_approval,
        snapshot=universe,
        data_root=data_root,
    )
    document = _read_document(path, "corporate-action authorization")
    required = {
        "data_root": str(data_root),
        "decision": "APPROVED",
        "distribution": "INTERNAL_SINGLE_USER_ONLY",
        "endpoint": "GET https://data.alpaca.markets/v1/corporate-actions",
        "live_trading_capable": False,
        "model_training_authorized": True,
        "network_method": "GET_ONLY",
        "parent_approval_sha256": parent.approval_sha256,
        "parent_policy_sha256": parent.policy_sha256,
        "purpose": "REFERENCE_VALIDATION_AND_LABEL_INVALIDATION",
        "schema_version": "1.0.0",
        "ticker_source_sha256": universe.source_file_sha256.hex(),
    }
    if any(document.get(key) != value for key, value in required.items()):
        raise MinuteNormalizationError(
            MinuteNormalizationCode.INVALID_SOURCE,
            "corporate-action authorization scope does not match",
        )
    expires_ns = _parse_utc_ns(document.get("expires_at_utc"), "authorization expiry")
    if expires_ns <= _utc_ns(datetime.now(UTC)):
        raise MinuteNormalizationError(
            MinuteNormalizationCode.INVALID_SOURCE,
            "corporate-action authorization expired",
        )
    return document


def _action_response(
    value: object,
) -> tuple[Mapping[str, Sequence[object]], str | None]:
    if not isinstance(value, dict) or not isinstance(
        value.get("corporate_actions"), dict
    ):
        raise MinuteNormalizationError(
            MinuteNormalizationCode.INVALID_SOURCE,
            "Alpaca corporate-action response is malformed",
        )
    groups = cast("Mapping[str, Sequence[object]]", value["corporate_actions"])
    if any(
        not isinstance(name, str) or not isinstance(items, list)
        for name, items in groups.items()
    ):
        raise MinuteNormalizationError(
            MinuteNormalizationCode.INVALID_SOURCE,
            "Alpaca corporate-action groups are malformed",
        )
    token = value.get("next_page_token")
    if token is not None and (
        not isinstance(token, str) or not token or len(token) > MAX_PAGE_TOKEN_BYTES
    ):
        raise MinuteNormalizationError(
            MinuteNormalizationCode.INVALID_SOURCE,
            "Alpaca corporate-action page token is malformed",
        )
    return groups, token


def _action_event_date(record: Mapping[str, object]) -> date | None:
    for field in ("ex_date", "effective_date", "process_date"):
        raw = record.get(field)
        if isinstance(raw, str):
            try:
                return date.fromisoformat(raw)
            except ValueError:
                return None
    return None


def _stage_and_publish(
    repository: DataRepository,
    lease: AdmissionLease,
    payload: bytes,
    final_relative: str,
) -> tuple[str, int]:
    digest = hashlib.sha256(payload).hexdigest()
    staged_relative = f"tmp/alpaca-reference-{digest}.partial"
    staged = repository.root / staged_relative
    if not staged.exists():
        descriptor = os.open(
            staged,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
            0o600,
        )
        with os.fdopen(descriptor, "wb", closefd=True) as output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
    elif staged.read_bytes() != payload:
        raise MinuteNormalizationError(
            MinuteNormalizationCode.HASH_MISMATCH, "staged source object conflicts"
        )
    repository.publish_staged_object(
        staged_relative,
        final_relative,
        expected_sha256=digest,
        expected_size_bytes=len(payload),
        lease=lease,
    )
    return digest, len(payload)


def download_corporate_actions(  # noqa: PLR0913, PLR0915
    *,
    data_root: Path,
    backfill_report_path: Path,
    parent_policy: Path,
    parent_approval: Path,
    action_authorization: Path,
    secrets_path: Path,
    output_report_path: Path,
    transport: object | None = None,
) -> dict[str, object]:
    """Download and persist the bounded POC action catalog through Alpaca GET."""
    if output_report_path.exists():
        return _read_document(output_report_path, "corporate-action report")
    authorization = _validate_action_authorization(
        action_authorization,
        data_root=data_root,
        parent_policy=parent_policy,
        parent_approval=parent_approval,
    )
    backfill = _read_document(backfill_report_path, "Alpaca backfill report")
    if backfill.get("status") != "COMPLETED" or backfill.get("mode") != "BACKFILL":
        raise MinuteNormalizationError(
            MinuteNormalizationCode.INVALID_SOURCE, "backfill report is not complete"
        )
    universe = load_ticker_universe(UnresolvedInstrumentResolver())
    if backfill.get("ticker_source_sha256") != universe.source_file_sha256.hex():
        raise MinuteNormalizationError(
            MinuteNormalizationCode.HASH_MISMATCH, "backfill universe changed"
        )
    start = date.fromisoformat(cast("str", backfill["start_date"]))
    end = date.fromisoformat(cast("str", backfill["end_date"]))
    repository = DataRepository(data_root)
    repository.initialize()
    credentials = AlpacaCredentials.load(secrets_path)
    client = AlpacaApiClient(credentials, transport=transport)  # type: ignore[arg-type]
    request = StorageRequest(
        "alpaca-corporate-actions-poc-v1",
        output_bytes=ACTION_DOWNLOAD_BUDGET_BYTES,
        temporary_bytes=ACTION_DOWNLOAD_BUDGET_BYTES,
    )
    source_ids: list[str] = []
    counts: dict[str, int] = {}
    received_values: list[int] = []
    page_token: str | None = None
    with repository.acquire_admission(request, _quota_evidence(data_root)) as lease:
        for page_index in range(MAX_ACTION_PAGES):
            result = client.corporate_actions(
                universe.canonical_symbols, start, end, page_token
            )
            groups, returned_token = _action_response(result.value)
            record_count = 0
            event_dates: list[date] = []
            for group, records in groups.items():
                counts[group] = counts.get(group, 0) + len(records)
                record_count += len(records)
                for record in records:
                    if not isinstance(record, dict):
                        raise MinuteNormalizationError(
                            MinuteNormalizationCode.INVALID_SOURCE,
                            "corporate-action record is not an object",
                        )
                    action_date = _action_event_date(
                        cast("Mapping[str, object]", record)
                    )
                    if action_date is None:
                        raise MinuteNormalizationError(
                            MinuteNormalizationCode.INVALID_SOURCE,
                            "corporate-action record lacks a valid action date",
                        )
                    event_dates.append(action_date)
            received_ns = _utc_ns(result.received_at_utc)
            received_values.append(received_ns)
            digest = hashlib.sha256(result.body).hexdigest()
            relative = f"raw/alpaca-corporate-actions/{digest[:2]}/{digest}.json"
            _, size = _stage_and_publish(repository, lease, result.body, relative)
            event_min = min(event_dates, default=start)
            event_max = max(event_dates, default=end)
            event_min_ns = _utc_ns(
                datetime.combine(event_min, datetime.min.time(), UTC)
            )
            event_max_ns = _utc_ns(
                datetime.combine(event_max, datetime.min.time(), UTC)
            )
            manifest = SourceManifest(
                source_name="alpaca-corporate-actions-v1",
                source_version="standard-api-v1",
                source_object_id=(
                    f"range:{start.isoformat()}:{end.isoformat()}:page:{page_index:04d}"
                ),
                storage_path=relative,
                object_sha256=digest,
                size_bytes=size,
                record_count=record_count,
                schema_name="alpaca-corporate-actions-v1-response",
                schema_version="1.0.0",
                times=ManifestTimeRange(
                    event_time_min_ns=event_min_ns,
                    event_time_max_ns=event_max_ns,
                    publication_time_min_ns=received_ns,
                    publication_time_max_ns=received_ns,
                    receive_time_min_ns=received_ns,
                    receive_time_max_ns=received_ns,
                    processing_time_min_ns=received_ns,
                    processing_time_max_ns=received_ns,
                    revision_time_min_ns=received_ns,
                    revision_time_max_ns=received_ns,
                ),
                universe_snapshot_sha256=cast(
                    "str", backfill["universe_snapshot_sha256"]
                ),
                lineage=ManifestLineage(),
            )
            repository.publish_manifest(manifest, lease)
            source_ids.append(manifest.manifest_id)
            page_token = returned_token
            if page_token is None:
                break
        else:
            raise MinuteNormalizationError(
                MinuteNormalizationCode.SOURCE_TOO_LARGE,
                "corporate-action response exceeded its page bound",
            )
    finished_ns = max(received_values)
    body = {
        "authorization_sha256": authorization["document_sha256"],
        "backfill_report_sha256": backfill["document_sha256"],
        "date_range": {"end": end.isoformat(), "start": start.isoformat()},
        "finished_at_ns": finished_ns,
        "group_counts": dict(sorted(counts.items())),
        "historical_provider_publication_times_observed": False,
        "live_trading_capable": False,
        "network_access_performed": True,
        "provider_request_count": client.request_count,
        "provider_retry_count": client.retry_count,
        "schema_version": "1.0.0",
        "source_manifest_ids": sorted(source_ids),
        "status": "COMPLETED",
        "ticker_source_sha256": universe.source_file_sha256.hex(),
    }
    _write_immutable_document(output_report_path, body)
    return _read_document(output_report_path, "corporate-action report")


@dataclass(frozen=True, slots=True)
class _ActionPoint:
    action_id: str
    effective_ns: int


class AlpacaBackfillReferenceResolver:
    """Explicit now-known retrospective reference resolver for offline research."""

    def __init__(  # noqa: C901, PLR0912
        self,
        backfill: Mapping[str, object],
        action_report: Mapping[str, object],
        repository: DataRepository,
    ) -> None:
        """Bind immutable backfill, current asset, calendar, and action evidence."""
        raw_assets = backfill.get("assets")
        raw_sessions = backfill.get("sessions")
        if not isinstance(raw_assets, dict) or not isinstance(raw_sessions, list):
            raise MinuteNormalizationError(
                MinuteNormalizationCode.MISSING_REFERENCE,
                "backfill reference evidence is incomplete",
            )
        self._assets = cast("Mapping[str, Mapping[str, object]]", raw_assets)
        self._sessions: dict[date, tuple[int, int]] = {}
        for raw in raw_sessions:
            if not isinstance(raw, dict):
                raise MinuteNormalizationError(
                    MinuteNormalizationCode.MISSING_CALENDAR,
                    "backfill calendar row is malformed",
                )
            session_date = date.fromisoformat(cast("str", raw["date"]))
            self._sessions[session_date] = (
                _parse_utc_ns(raw["open_utc"], "session open"),
                _parse_utc_ns(raw["close_utc"], "session close"),
            )
        self.processing_time_ns = cast("int", action_report["finished_at_ns"])
        if self.processing_time_ns < _parse_utc_ns(
            backfill["finished_at_utc"], "backfill completion"
        ):
            raise MinuteNormalizationError(
                MinuteNormalizationCode.TIMESTAMP_DISORDER,
                "reference download predates its source backfill",
            )
        self._actions: dict[str, list[_ActionPoint]] = {}
        structural_groups = {
            "cash_mergers",
            "forward_splits",
            "name_changes",
            "reverse_splits",
            "spin_offs",
            "stock_and_cash_mergers",
            "stock_dividends",
            "stock_mergers",
        }
        for manifest_id in cast("Sequence[str]", action_report["source_manifest_ids"]):
            manifest = repository.load_manifest(manifest_id)
            if not isinstance(manifest, SourceManifest):
                raise MinuteNormalizationError(
                    MinuteNormalizationCode.INVALID_SOURCE,
                    "action report references a non-source manifest",
                )
            payload = (repository.root / manifest.storage_path).read_bytes()
            if hashlib.sha256(payload).hexdigest() != manifest.object_sha256:
                raise MinuteNormalizationError(
                    MinuteNormalizationCode.HASH_MISMATCH,
                    "corporate-action source object changed",
                )
            groups, _ = _action_response(json.loads(payload))
            for group, records in groups.items():
                if group not in structural_groups:
                    continue
                for raw_record in records:
                    record = cast("Mapping[str, object]", raw_record)
                    action_id = record.get("id")
                    action_date = _action_event_date(record)
                    if not isinstance(action_id, str) or action_date is None:
                        raise MinuteNormalizationError(
                            MinuteNormalizationCode.INVALID_SOURCE,
                            "structural action identity is malformed",
                        )
                    symbols = {
                        value
                        for key, value in record.items()
                        if key
                        in {
                            "symbol",
                            "old_symbol",
                            "new_symbol",
                            "source_symbol",
                            "acquiree_symbol",
                            "acquirer_symbol",
                        }
                        and isinstance(value, str)
                        and value in self._assets
                    }
                    effective_ns = _utc_ns(
                        datetime.combine(
                            action_date, datetime.min.time(), _SESSION_ZONE
                        )
                    )
                    for symbol in symbols:
                        self._actions.setdefault(symbol, []).append(
                            _ActionPoint(f"alpaca:{group}:{action_id}", effective_ns)
                        )
        for points in self._actions.values():
            points.sort(key=lambda item: (item.effective_ns, item.action_id))
        self.reference_sha256 = _digest(
            {
                "action_report_sha256": action_report["document_sha256"],
                "backfill_report_sha256": backfill["document_sha256"],
                "price_quantum_currency_nanos": PRICE_QUANTUM_CURRENCY_NANOS,
                "semantics": "NOW_KNOWN_RETROSPECTIVE_CURRENT_UNIVERSE",
            }
        )

    def resolve(
        self, symbol: str, *, event_time_ns: int, known_at_ns: int
    ) -> ResolvedMinuteReference:
        """Resolve one historical bar using evidence available at build time."""
        asset = self._assets.get(symbol)
        if (
            asset is None
            or asset.get("status") != "RESOLVED"
            or not isinstance(asset.get("asset_id"), str)
        ):
            raise MinuteNormalizationError(
                MinuteNormalizationCode.MISSING_REFERENCE,
                f"{symbol} is not an eligible US-equity reference",
            )
        if known_at_ns < self.processing_time_ns:
            raise MinuteNormalizationError(
                MinuteNormalizationCode.MISSING_REFERENCE,
                "canonical build cutoff predates reference acquisition",
            )
        trading_date = (
            datetime.fromtimestamp(event_time_ns // CURRENCY_NANOS_PER_UNIT, tz=UTC)
            .astimezone(_SESSION_ZONE)
            .date()
        )
        session = self._sessions.get(trading_date)
        if session is None:
            raise MinuteNormalizationError(
                MinuteNormalizationCode.OUTSIDE_SESSION,
                "minute is outside the retained market calendar",
            )
        opened, closed = session
        if event_time_ns < opened or event_time_ns + 60_000_000_000 > closed:
            raise MinuteNormalizationError(
                MinuteNormalizationCode.OUTSIDE_SESSION,
                "minute is outside the regular market session",
            )
        instrument_id = alpaca_instrument_id(cast("str", asset["asset_id"])).hex()
        actions = tuple(
            item.action_id
            for item in self._actions.get(symbol, ())
            if item.effective_ns <= event_time_ns
        )
        session_id = _digest(
            {
                "calendar": "ALPACA-RETROSPECTIVE-US-EQUITIES",
                "close_ns": closed,
                "date": trading_date.isoformat(),
                "open_ns": opened,
            }
        )[:32]
        exchange = asset.get("exchange")
        market_id = exchange if isinstance(exchange, str) and exchange else "UNKNOWN"
        return ResolvedMinuteReference(
            instrument_id=instrument_id,
            market_id=market_id,
            mapping_id=f"alpaca-retrospective:{symbol}:{instrument_id}",
            instrument_revision_id=f"alpaca-current-asset:{instrument_id}",
            tick_revision_id="research-price-quantum:USD_NANOS:1",
            tick_value_currency_nanos=PRICE_QUANTUM_CURRENCY_NANOS,
            currency="USD",
            session_id=session_id,
            trading_date=trading_date,
            session_open_ns=opened,
            session_close_ns=closed,
            corporate_action_ids=actions,
            reference_sha256=self.reference_sha256,
            quality_reasons=(
                "HALT_HISTORY_INCOMPLETE",
                "HISTORICAL_PROVIDER_PUBLICATION_TIME_UNOBSERVED",
                "NOW_KNOWN_CURRENT_UNIVERSE_MAPPING",
                "PRICE_QUANTUM_IS_NOT_VENUE_TICK",
            ),
        )


def promote_backfill_to_parquet(  # noqa: C901, PLR0913
    *,
    data_root: Path,
    backfill_report_path: Path,
    action_report_path: Path,
    output_report_path: Path,
    spool_path: Path | None = None,
    maximum_source_objects: int | None = None,
) -> dict[str, object]:
    """Promote verified Alpaca source pages into accepted Prompt 56 Parquet."""
    if output_report_path.exists():
        return _read_document(output_report_path, "canonical promotion report")
    backfill = _read_document(backfill_report_path, "Alpaca backfill report")
    actions = _read_document(action_report_path, "corporate-action report")
    repository = DataRepository(data_root)
    repository.initialize()
    resolver = AlpacaBackfillReferenceResolver(backfill, actions, repository)
    normalizer = CanonicalMinuteNormalizer(resolver)
    raw_source_ids = cast("Sequence[str]", backfill.get("source_manifest_ids"))
    source_ids = tuple(sorted(raw_source_ids))
    if maximum_source_objects is not None:
        if maximum_source_objects <= 0:
            raise MinuteNormalizationError(
                MinuteNormalizationCode.INVALID_SOURCE,
                "maximum_source_objects must be positive",
            )
        source_ids = source_ids[:maximum_source_objects]
    build_key = _digest(
        {
            "action_report": actions["document_sha256"],
            "backfill_report": backfill["document_sha256"],
            "canonical_schema": "1.0.0",
            "spool_schema": "1.2.0",
            "source_manifest_ids": list(source_ids),
        }
    )
    actual_spool = spool_path or (
        data_root / "tmp/canonical-minute" / f"alpaca-{build_key}.sqlite"
    )
    published = []
    with CanonicalMinuteSpool(
        actual_spool,
        maximum_records=20_000_000,
        maximum_bytes=CANONICAL_SPOOL_LIMIT_BYTES,
    ) as spool:
        for index, source_id in enumerate(source_ids):
            if _STOP_REQUESTED:
                raise MinuteNormalizationError(
                    MinuteNormalizationCode.INVALID_SOURCE,
                    "canonical promotion interrupted after checkpoint",
                )
            manifest = repository.load_manifest(source_id)
            if (
                not isinstance(manifest, SourceManifest)
                or manifest.schema_name != "alpaca-market-data-v2-bars-response"
            ):
                raise MinuteNormalizationError(
                    MinuteNormalizationCode.INVALID_SOURCE,
                    "backfill references a non-Alpaca-bars source manifest",
                )
            spool.ingest(
                manifest,
                iter_source_minutes(
                    data_root,
                    manifest,
                    canonical_processing_time_ns=resolver.processing_time_ns,
                ),
                normalizer,
            )
            if index and index % 100 == 0:  # pragma: no cover - progress only
                sys.stderr.write(
                    f"canonical ingest checkpoint: {index}/{len(source_ids)} sources\n"
                )
        totals = dict(spool.ingest_totals())
        quota = _quota_evidence(data_root)
        partition_keys = spool.partition_keys()
        output_estimate = spool.record_count * 512 + len(partition_keys) * 2_000_000
        request = StorageRequest(
            f"canonical-minute-promotion:{build_key[:32]}",
            output_bytes=output_estimate,
            temporary_bytes=min(output_estimate, 1_000_000_000),
        )
        with repository.acquire_admission(request, quota) as lease:
            for index, (trading_date, bucket) in enumerate(partition_keys):
                if _STOP_REQUESTED:
                    raise MinuteNormalizationError(
                        MinuteNormalizationCode.INVALID_SOURCE,
                        "canonical publication interrupted",
                    )
                result = publish_partition(
                    repository,
                    quota,
                    spool,
                    trading_date,
                    bucket,
                    universe_snapshot_sha256=cast(
                        "str", backfill["universe_snapshot_sha256"]
                    ),
                    admission_lease=lease,
                )
                published.append(result)
                if index and index % 250 == 0:  # pragma: no cover - progress only
                    sys.stderr.write(
                        f"canonical publish checkpoint: {index}/{len(partition_keys)}\n"
                    )
    body = {
        "action_report_sha256": actions["document_sha256"],
        "backfill_report_sha256": backfill["document_sha256"],
        "build_key": build_key,
        "counts": {
            **totals,
            "partition_count": len(published),
            "published_records": sum(item.record_count for item in published),
        },
        "data_quality": {
            "historical_halt_coverage_complete": False,
            "historical_provider_publication_times_observed": False,
            "mapping_semantics": "NOW_KNOWN_CURRENT_UNIVERSE",
            "price_quantum_currency_nanos": PRICE_QUANTUM_CURRENCY_NANOS,
            "price_quantum_is_venue_tick": False,
        },
        "live_trading_capable": False,
        "partition_manifest_ids": sorted(item.manifest_id for item in published),
        "schema_version": "1.0.0",
        "source_manifest_ids": list(source_ids),
        "spool_path": str(actual_spool),
        "status": "COMPLETED",
        "training_use": "RESEARCH_POC_WITH_DISCLOSED_DEGRADED_REFERENCE_FLAGS",
    }
    _write_immutable_document(output_report_path, body)
    return _read_document(output_report_path, "canonical promotion report")


def verify_promotion(*, data_root: Path, report_path: Path) -> dict[str, object]:
    """Verify each accepted Parquet manifest and object named by the report."""
    report = _read_document(report_path, "canonical promotion report")
    repository = DataRepository(data_root)
    checked_records = 0
    checked_bytes = 0
    for manifest_id in cast("Sequence[str]", report["partition_manifest_ids"]):
        manifest = repository.load_manifest(manifest_id)
        path = data_root / manifest.storage_path
        with path.open("rb") as source:
            digest = hashlib.file_digest(source, "sha256").hexdigest()
        if (
            digest != manifest.object_sha256
            or path.stat().st_size != manifest.size_bytes
        ):
            raise MinuteNormalizationError(
                MinuteNormalizationCode.HASH_MISMATCH,
                "canonical Parquet object does not match its manifest",
            )
        checked_records += manifest.record_count
        checked_bytes += manifest.size_bytes
    expected = cast("Mapping[str, int]", report["counts"])["published_records"]
    if checked_records != expected:
        raise MinuteNormalizationError(
            MinuteNormalizationCode.HASH_MISMATCH,
            "verified canonical record count differs from the report",
        )
    return {
        "checked_bytes_decimal": checked_bytes,
        "checked_partitions": len(
            cast("Sequence[str]", report["partition_manifest_ids"])
        ),
        "checked_records": checked_records,
        "report_sha256": report["document_sha256"],
        "status": "PASS",
    }


def _signal_handler(_number: int, _frame: object) -> None:
    global _STOP_REQUESTED  # noqa: PLW0603
    _STOP_REQUESTED = True


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="aegis-real-minute")
    parser.add_argument("--data-root", type=Path, default=DEFAULT_DATA_ROOT)
    commands = parser.add_subparsers(dest="command", required=True)
    authorize = commands.add_parser("authorize-actions")
    authorize.add_argument("--operator-id", required=True)
    authorize.add_argument("--expires-at-utc", required=True)
    authorize.add_argument("--output", type=Path, default=DEFAULT_ACTION_AUTHORIZATION)
    authorize.add_argument("--parent-policy", type=Path, default=DEFAULT_PARENT_POLICY)
    authorize.add_argument(
        "--parent-approval", type=Path, default=DEFAULT_PARENT_APPROVAL
    )
    authorize.add_argument(
        "--acknowledge-internal-academic-use", action="store_true", required=True
    )
    fetch = commands.add_parser("fetch-actions")
    fetch.add_argument("--execute", action="store_true")
    fetch.add_argument("--backfill-report", type=Path, default=DEFAULT_BACKFILL_REPORT)
    fetch.add_argument("--parent-policy", type=Path, default=DEFAULT_PARENT_POLICY)
    fetch.add_argument("--parent-approval", type=Path, default=DEFAULT_PARENT_APPROVAL)
    fetch.add_argument(
        "--authorization", type=Path, default=DEFAULT_ACTION_AUTHORIZATION
    )
    fetch.add_argument("--secrets-file", type=Path, required=True)
    fetch.add_argument("--output", type=Path, default=DEFAULT_ACTION_REPORT)
    promote = commands.add_parser("promote")
    promote.add_argument("--execute", action="store_true")
    promote.add_argument(
        "--backfill-report", type=Path, default=DEFAULT_BACKFILL_REPORT
    )
    promote.add_argument("--action-report", type=Path, default=DEFAULT_ACTION_REPORT)
    promote.add_argument("--output", type=Path, default=DEFAULT_PROMOTION_REPORT)
    promote.add_argument("--spool", type=Path)
    verify = commands.add_parser("verify")
    verify.add_argument("--report", type=Path, default=DEFAULT_PROMOTION_REPORT)
    return parser


def main(arguments: Sequence[str] | None = None) -> int:
    """Run the fail-closed real-minute pipeline without trading capability."""
    parsed = _parser().parse_args(arguments)
    try:
        if parsed.command == "authorize-actions":
            expiry = datetime.fromisoformat(parsed.expires_at_utc)
            output: object = authorize_corporate_actions(
                data_root=parsed.data_root,
                parent_policy=parsed.parent_policy,
                parent_approval=parsed.parent_approval,
                output_path=parsed.output,
                operator_id=parsed.operator_id,
                expires_at_utc=expiry,
            )
        elif parsed.command == "fetch-actions":
            if not parsed.execute:
                output = {
                    "dry_run": True,
                    "network_access_performed": False,
                    "status": "PLANNED",
                }
            else:
                output = download_corporate_actions(
                    data_root=parsed.data_root,
                    backfill_report_path=parsed.backfill_report,
                    parent_policy=parsed.parent_policy,
                    parent_approval=parsed.parent_approval,
                    action_authorization=parsed.authorization,
                    secrets_path=parsed.secrets_file,
                    output_report_path=parsed.output,
                )
        elif parsed.command == "promote":
            if not parsed.execute:
                output = {
                    "dry_run": True,
                    "network_access_performed": False,
                    "status": "PLANNED",
                }
            else:
                output = promote_backfill_to_parquet(
                    data_root=parsed.data_root,
                    backfill_report_path=parsed.backfill_report,
                    action_report_path=parsed.action_report,
                    output_report_path=parsed.output,
                    spool_path=parsed.spool,
                )
        else:
            output = verify_promotion(
                data_root=parsed.data_root, report_path=parsed.report
            )
    except (
        AlpacaDataError,
        MinuteNormalizationError,
        OSError,
        ValueError,
        InterruptedError,
    ) as error:
        code = getattr(getattr(error, "code", None), "value", "PIPELINE_REJECTED")
        sys.stdout.write(
            json.dumps(
                {"error": {"code": code, "message": str(error)}, "status": "REJECTED"},
                sort_keys=True,
            )
            + "\n"
        )
        return 1
    sys.stdout.write(json.dumps(output, indent=2, sort_keys=True) + "\n")
    return 0


def cli_main() -> int:
    """Installed console entry point with checkpoint-preserving signal handling."""
    signal.signal(signal.SIGINT, _signal_handler)
    signal.signal(signal.SIGTERM, _signal_handler)
    return main()


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(cli_main())
