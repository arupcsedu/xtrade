"""Authenticated point-in-time event snapshots for offline feature datasets."""

from __future__ import annotations

import hashlib
import json
import os
import stat
from dataclasses import dataclass
from datetime import UTC, date, datetime, timedelta
from pathlib import Path
from typing import TYPE_CHECKING, Final, cast

from aegis_mx_research.alfred import APPROVED_SERIES, AlfredObjectKind
from aegis_mx_research.feature_dataset import (
    EventCoverage,
    EventIndex,
    FeatureEventKind,
    TemporalFeatureEvent,
)
from aegis_mx_research.forecast_contracts import (
    InstrumentResolutionCode,
    UniverseSnapshot,
)
from aegis_mx_research.sec_edgar import (
    FilingMetadata,
    SecDataset,
    SecEdgarError,
    historical_submission_files,
    historical_submissions_url,
    parse_submissions,
)

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence

FEATURE_EVENT_SNAPSHOT_SCHEMA_VERSION: Final = "1.0.0"
NEWS_EVENT_WINDOW_NS: Final = 86_400_000_000_000
MACRO_EVENT_WINDOW_NS: Final = 86_400_000_000_000
MAX_EVENT_SNAPSHOT_BYTES: Final = 64_000_000
MAX_SOURCE_REPORT_BYTES: Final = 16_000_000
MAX_SOURCE_MANIFEST_BYTES: Final = 65_536
MAX_SOURCE_OBJECT_BYTES: Final = 64_000_000
MAX_SOURCE_MANIFESTS: Final = 20_000
MAX_EVENTS: Final = 1_000_000
MAX_INSTRUMENTS: Final = 10_000
MAX_INT64: Final = (1 << 63) - 1
_SHA256_LENGTH: Final = 64
_INSTRUMENT_ID_LENGTH: Final = 32


class FeatureEventSnapshotError(ValueError):
    """Reject unauthenticated, incomplete, or temporally unsafe event inputs."""


def _canonical_bytes(value: object) -> bytes:
    return json.dumps(
        value, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode("ascii")


def _sha256(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def _is_sha256(value: object) -> bool:
    return (
        isinstance(value, str)
        and len(value) == _SHA256_LENGTH
        and all(character in "0123456789abcdef" for character in value)
    )


def _read_regular(path: Path, maximum_bytes: int, label: str) -> bytes:
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
        with os.fdopen(descriptor, "rb", closefd=True) as source:
            status = os.fstat(source.fileno())
            payload = source.read(maximum_bytes + 1)
    except OSError as error:
        raise FeatureEventSnapshotError(f"{label} is unavailable or unsafe") from error
    if (
        not stat.S_ISREG(status.st_mode)
        or status.st_size <= 0
        or status.st_size > maximum_bytes
        or len(payload) != status.st_size
    ):
        raise FeatureEventSnapshotError(f"{label} is empty, truncated, or oversized")
    return payload


def _read_json(path: Path, maximum_bytes: int, label: str) -> dict[str, object]:
    try:
        value = json.loads(_read_regular(path, maximum_bytes, label))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise FeatureEventSnapshotError(f"{label} is malformed JSON") from error
    if not isinstance(value, dict):
        raise FeatureEventSnapshotError(f"{label} must be a JSON object")
    return cast("dict[str, object]", value)


def _verify_self_hash(
    document: Mapping[str, object], hash_field: str, label: str
) -> str:
    claimed = document.get(hash_field)
    if not _is_sha256(claimed):
        raise FeatureEventSnapshotError(f"{label} has no valid {hash_field}")
    body = dict(document)
    del body[hash_field]
    if _sha256(_canonical_bytes(body)) != claimed:
        raise FeatureEventSnapshotError(f"{label} self-hash mismatch")
    return claimed


def _date_start_ns(value: date) -> int:
    epoch = datetime(1970, 1, 1, tzinfo=UTC)
    delta = datetime(value.year, value.month, value.day, tzinfo=UTC) - epoch
    result = delta.days * NEWS_EVENT_WINDOW_NS
    if result <= 0 or result > MAX_INT64:
        raise FeatureEventSnapshotError("event coverage date exceeds int64")
    return result


def _checked_window_end(event_time_ns: int, duration_ns: int) -> int:
    result = event_time_ns + duration_ns
    if event_time_ns <= 0 or duration_ns <= 0 or result > MAX_INT64:
        raise FeatureEventSnapshotError("event validity window exceeds int64")
    return result


def _universe_instruments(universe: UniverseSnapshot) -> dict[str, str]:
    result: dict[str, str] = {}
    for entry in universe.ordered_entries:
        if (
            entry.resolution_code is InstrumentResolutionCode.RESOLVED
            and entry.instrument_id is not None
        ):
            result[entry.symbol] = entry.instrument_id.hex()
    return result


@dataclass(frozen=True, slots=True)
class _Artifact:
    """Verified source manifest and object bytes."""

    manifest_id: str
    document: Mapping[str, object]
    payload: bytes


def _safe_object_path(data_root: Path, relative_text: str, prefix: str) -> Path:
    relative = Path(relative_text)
    if (
        relative.is_absolute()
        or not relative.parts
        or any(part in {"", ".", ".."} for part in relative.parts)
        or relative.as_posix().startswith("/")
        or not relative.as_posix().startswith(prefix)
    ):
        raise FeatureEventSnapshotError("source object path is outside its boundary")
    root = data_root.resolve(strict=True)
    unresolved = root / relative
    try:
        resolved = unresolved.resolve(strict=True)
    except OSError as error:
        raise FeatureEventSnapshotError("source object is missing") from error
    if unresolved.is_symlink() or not resolved.is_relative_to(root):
        raise FeatureEventSnapshotError("source object path is unsafe")
    return resolved


def _load_artifacts(
    data_root: Path,
    manifest_directory: Path,
    *,
    manifest_prefix: str,
    object_prefix: str,
    universe_sha256: str | None,
    kind_field: str,
    kind_value: str,
) -> tuple[_Artifact, ...]:
    try:
        paths = tuple(sorted(manifest_directory.glob(f"{manifest_prefix}-*.json")))
    except OSError as error:
        raise FeatureEventSnapshotError(
            "source manifest directory is unreadable"
        ) from error
    if len(paths) > MAX_SOURCE_MANIFESTS:
        raise FeatureEventSnapshotError("source manifest count exceeds its bound")
    artifacts: list[_Artifact] = []
    for path in paths:
        document = _read_json(path, MAX_SOURCE_MANIFEST_BYTES, "source manifest")
        manifest_id = document.get("manifest_id")
        manifest_hash = document.get("manifest_sha256")
        if (
            not isinstance(manifest_id, str)
            or manifest_id != path.stem
            or not manifest_id.startswith(f"{manifest_prefix}-")
            or not _is_sha256(manifest_hash)
        ):
            raise FeatureEventSnapshotError("source manifest identity is invalid")
        body = dict(document)
        del body["manifest_id"]
        del body["manifest_sha256"]
        body_hash = _sha256(_canonical_bytes(body))
        identified_hash = _sha256(
            _canonical_bytes({**body, "manifest_id": manifest_id})
        )
        expected_manifest_hash = (
            identified_hash if manifest_prefix == "gdelt" else body_hash
        )
        if (
            manifest_id != f"{manifest_prefix}-{body_hash}"
            or manifest_hash != expected_manifest_hash
        ):
            raise FeatureEventSnapshotError("source manifest self-hash mismatch")
        if (
            universe_sha256 is not None
            and document.get("universe_snapshot_sha256") != universe_sha256
        ):
            raise FeatureEventSnapshotError("source manifest universe mismatch")
        if document.get(kind_field) != kind_value:
            continue
        storage_path = document.get("storage_path")
        object_hash = document.get("object_sha256")
        object_size = document.get("size_bytes_decimal")
        if (
            not isinstance(storage_path, str)
            or not _is_sha256(object_hash)
            or not isinstance(object_size, int)
            or object_size <= 0
            or object_size > MAX_SOURCE_OBJECT_BYTES
        ):
            raise FeatureEventSnapshotError(
                "source manifest object metadata is invalid"
            )
        object_path = _safe_object_path(data_root, storage_path, object_prefix)
        payload = _read_regular(object_path, MAX_SOURCE_OBJECT_BYTES, "source object")
        if len(payload) != object_size or _sha256(payload) != object_hash:
            raise FeatureEventSnapshotError("source object size or SHA-256 mismatch")
        artifacts.append(_Artifact(manifest_id, document, payload))
    return tuple(artifacts)


def _forms(filings: Sequence[FilingMetadata]) -> dict[str, int]:
    result: dict[str, int] = {}
    for raw in filings:
        form = raw.form
        result[form] = result.get(form, 0) + 1
    return dict(sorted(result.items()))


def _sec_events(
    data_root: Path,
    universe: UniverseSnapshot,
    source_universe_sha256: str,
    coverage_report_path: Path,
) -> tuple[tuple[TemporalFeatureEvent, ...], EventCoverage, tuple[str, ...], str]:
    report = _read_json(coverage_report_path, MAX_SOURCE_REPORT_BYTES, "SEC report")
    report_hash = _verify_self_hash(report, "report_sha256", "SEC report")
    if (
        report.get("schema_version") != "1.1.0"
        or report.get("universe_sha256") != source_universe_sha256
        or report.get("network_access_performed") is not True
    ):
        raise FeatureEventSnapshotError("SEC report is incompatible or incomplete")
    try:
        start_date = date.fromisoformat(cast("str", report["filing_start_date"]))
        end_date = date.fromisoformat(cast("str", report["filing_end_date"]))
        generated_at_ns = cast("int", report["generated_at_utc_ns"])
    except (KeyError, TypeError, ValueError) as error:
        raise FeatureEventSnapshotError(
            "SEC report date coverage is malformed"
        ) from error
    if start_date > end_date or generated_at_ns <= 0:
        raise FeatureEventSnapshotError("SEC report date coverage is invalid")
    artifacts = _load_artifacts(
        data_root,
        data_root / "manifests/sec-edgar",
        manifest_prefix="sec",
        object_prefix="raw/sec-edgar/submissions/",
        universe_sha256=source_universe_sha256,
        kind_field="dataset",
        kind_value=SecDataset.SUBMISSIONS.value,
    )
    if not artifacts:
        raise FeatureEventSnapshotError("SEC submissions artifacts are unavailable")
    current_by_cik: dict[str, list[_Artifact]] = {}
    by_url_hash: dict[str, list[_Artifact]] = {}
    for artifact in artifacts:
        source_url_hash = artifact.document.get("source_url_sha256")
        if isinstance(source_url_hash, str):
            by_url_hash.setdefault(source_url_hash, []).append(artifact)
        try:
            value = json.loads(artifact.payload)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise FeatureEventSnapshotError(
                "SEC submissions object is malformed"
            ) from error
        if isinstance(value, dict) and "filings" in value:
            raw_cik = value.get("cik")
            if isinstance(raw_cik, (str, int)):
                current_by_cik.setdefault(str(raw_cik).zfill(10), []).append(artifact)
    instruments = _universe_instruments(universe)
    raw_rows = report.get("issuer_coverage")
    if not isinstance(raw_rows, list):
        raise FeatureEventSnapshotError("SEC issuer coverage is malformed")
    events: list[TemporalFeatureEvent] = []
    selected_manifests: set[str] = set()
    seen_accessions: set[str] = set()
    resolved_rows = 0
    for raw_row in raw_rows:
        if not isinstance(raw_row, dict):
            raise FeatureEventSnapshotError("SEC issuer coverage row is malformed")
        ticker = raw_row.get("ticker")
        cik = raw_row.get("cik")
        status = raw_row.get("resolution_status")
        if status == "UNRESOLVED":
            continue
        if (
            status != "RESOLVED_CURRENT_ONLY"
            or not isinstance(ticker, str)
            or ticker not in instruments
            or not isinstance(cik, str)
        ):
            raise FeatureEventSnapshotError(
                "SEC issuer coverage does not match universe"
            )
        resolved_rows += 1
        expected_count = raw_row.get("filing_count")
        expected_amendments = raw_row.get("amendment_count")
        expected_forms = raw_row.get("forms")
        expected_shards = raw_row.get("historical_shard_count")
        if not all(
            isinstance(value, int)
            for value in (expected_count, expected_amendments, expected_shards)
        ) or not isinstance(expected_forms, dict):
            raise FeatureEventSnapshotError("SEC issuer counts are malformed")
        candidates: list[
            tuple[int, str, tuple[FilingMetadata, ...], tuple[str, ...]]
        ] = []
        for current in current_by_cik.get(cik, ()):
            receipt_ns = current.document.get("receipt_time_utc_ns")
            processing_ns = current.document.get("processing_time_utc_ns")
            record_count = current.document.get("record_count")
            if (
                not isinstance(receipt_ns, int)
                or not isinstance(processing_ns, int)
                or not isinstance(record_count, int)
                or receipt_ns > generated_at_ns
            ):
                continue
            try:
                filings = list(
                    parse_submissions(
                        current.payload,
                        expected_cik=cik,
                        received_at_utc_ns=receipt_ns,
                        processing_time_utc_ns=processing_ns,
                        filing_start_date=start_date,
                        filing_end_date=end_date,
                    )
                )
                if record_count != len(filings):
                    continue
                source_manifest_ids = [current.manifest_id]
                historical_files = historical_submission_files(current.payload)
                selected_files = []
                for filename in historical_files:
                    url_hash = _sha256(historical_submissions_url(filename).encode())
                    choices = [
                        item
                        for item in by_url_hash.get(url_hash, ())
                        if isinstance(item.document.get("receipt_time_utc_ns"), int)
                        and cast("int", item.document["receipt_time_utc_ns"])
                        <= generated_at_ns
                    ]
                    if not choices:
                        continue
                    historical = max(
                        choices,
                        key=lambda item: cast(
                            "int", item.document["receipt_time_utc_ns"]
                        ),
                    )
                    historical_receipt = cast(
                        "int", historical.document["receipt_time_utc_ns"]
                    )
                    historical_processing = cast(
                        "int", historical.document["processing_time_utc_ns"]
                    )
                    parsed = parse_submissions(
                        historical.payload,
                        expected_cik=cik,
                        received_at_utc_ns=historical_receipt,
                        processing_time_utc_ns=historical_processing,
                        filing_start_date=start_date,
                        filing_end_date=end_date,
                    )
                    if historical.document.get("record_count") != len(parsed):
                        continue
                    if parsed:
                        selected_files.append(filename)
                        filings.extend(parsed)
                        source_manifest_ids.append(historical.manifest_id)
                if len(selected_files) != expected_shards:
                    continue
            except (KeyError, SecEdgarError, TypeError, ValueError):
                continue
            if (
                len(filings) == expected_count
                and sum(item.is_amendment for item in filings) == expected_amendments
                and _forms(filings) == expected_forms
            ):
                candidates.append(
                    (
                        receipt_ns,
                        current.manifest_id,
                        tuple(filings),
                        tuple(source_manifest_ids),
                    )
                )
        if not candidates:
            raise FeatureEventSnapshotError(
                f"SEC filing artifacts do not reproduce coverage for {ticker}"
            )
        _, _, selected_filings, manifest_ids = max(
            candidates, key=lambda item: item[:2]
        )
        selected_manifests.update(manifest_ids)
        for raw_filing in selected_filings:
            accession = raw_filing.accession_number
            if accession in seen_accessions:
                raise FeatureEventSnapshotError("SEC filing accession is duplicated")
            seen_accessions.add(accession)
            acceptance_ns = raw_filing.acceptance_time_utc_ns
            events.append(
                TemporalFeatureEvent(
                    record_id=f"sec:{accession}",
                    revision_id=f"sec:{accession}",
                    kind=FeatureEventKind.NEWS,
                    instrument_id=instruments[ticker],
                    event_time_ns=acceptance_ns,
                    available_at_ns=acceptance_ns,
                    valid_until_ns=_checked_window_end(
                        acceptance_ns, NEWS_EVENT_WINDOW_NS
                    ),
                    source_sha256=raw_filing.source_sha256,
                )
            )
    summary = report.get("summary")
    if (
        not isinstance(summary, dict)
        or summary.get("resolved_ticker_count") != resolved_rows
        or summary.get("filing_count") != len(events)
    ):
        raise FeatureEventSnapshotError("SEC aggregate coverage does not reproduce")
    coverage = EventCoverage(
        FeatureEventKind.NEWS,
        "SEC_EDGAR_ACCEPTED_FILINGS",
        _date_start_ns(start_date),
        _date_start_ns(end_date + timedelta(days=1)),
        report_hash,
    )
    return tuple(events), coverage, tuple(sorted(selected_manifests)), report_hash


def _alfred_events(
    data_root: Path,
    run_report_path: Path,
) -> tuple[tuple[TemporalFeatureEvent, ...], EventCoverage, tuple[str, ...], str]:
    report = _read_json(run_report_path, MAX_SOURCE_REPORT_BYTES, "ALFRED report")
    report_hash = _verify_self_hash(report, "report_sha256", "ALFRED report")
    raw_coverage = report.get("coverage")
    manifest_ids = report.get("manifest_ids")
    snapshot_hashes = report.get("snapshot_sha256s")
    approved_series = {item.series_id for item in APPROVED_SERIES}
    if (
        report.get("schema_version") != "1.0.0"
        or report.get("network_access_performed") is not True
        or not isinstance(raw_coverage, dict)
        or set(cast("Mapping[str, object]", raw_coverage.get("series", {})))
        != approved_series
        or not isinstance(manifest_ids, list)
        or not isinstance(snapshot_hashes, list)
    ):
        raise FeatureEventSnapshotError("ALFRED report coverage is incomplete")
    artifacts = {
        item.manifest_id: item
        for item in _load_artifacts(
            data_root,
            data_root / "manifests/alfred",
            manifest_prefix="alfred",
            object_prefix="canonical/alfred/",
            universe_sha256=None,
            kind_field="object_kind",
            kind_value=AlfredObjectKind.CANONICAL_SNAPSHOT.value,
        )
    }
    snapshots: list[dict[str, object]] = []
    used_manifests: list[str] = []
    for manifest_id in manifest_ids:
        if not isinstance(manifest_id, str) or manifest_id not in artifacts:
            continue
        artifact = artifacts[manifest_id]
        try:
            snapshot = cast(
                "dict[str, object]",
                json.loads(artifact.payload),
            )
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise FeatureEventSnapshotError(
                "ALFRED snapshot is malformed JSON"
            ) from error
        snapshot_hash = _verify_self_hash(
            snapshot, "snapshot_sha256", "ALFRED snapshot"
        )
        if snapshot_hash not in snapshot_hashes:
            raise FeatureEventSnapshotError(
                "ALFRED snapshot is not bound to its report"
            )
        snapshots.append(snapshot)
        used_manifests.append(manifest_id)
    if len(snapshots) != len(approved_series):
        raise FeatureEventSnapshotError("ALFRED canonical snapshots are incomplete")
    events: list[TemporalFeatureEvent] = []
    observed_series: set[str] = set()
    for snapshot in snapshots:
        contract = snapshot.get("series_contract")
        releases = snapshot.get("releases")
        if not isinstance(contract, dict) or not isinstance(releases, list):
            raise FeatureEventSnapshotError("ALFRED snapshot content is malformed")
        series_id = contract.get("series_id")
        if not isinstance(series_id, str) or series_id not in approved_series:
            raise FeatureEventSnapshotError("ALFRED snapshot series is not approved")
        observed_series.add(series_id)
        source_hash = cast("str", snapshot["snapshot_sha256"])
        for raw_release in releases:
            if not isinstance(raw_release, dict):
                raise FeatureEventSnapshotError("ALFRED release row is malformed")
            known_at_ns = raw_release.get("known_at_utc_ns")
            record_hash = raw_release.get("record_sha256")
            release_date = raw_release.get("release_date")
            if (
                not isinstance(known_at_ns, int)
                or not _is_sha256(record_hash)
                or not isinstance(release_date, str)
            ):
                raise FeatureEventSnapshotError("ALFRED release identity is malformed")
            events.append(
                TemporalFeatureEvent(
                    record_id=f"alfred:{series_id}:{release_date}",
                    revision_id=cast("str", record_hash),
                    kind=FeatureEventKind.MACRO,
                    instrument_id=None,
                    event_time_ns=known_at_ns,
                    available_at_ns=known_at_ns,
                    valid_until_ns=_checked_window_end(
                        known_at_ns, MACRO_EVENT_WINDOW_NS
                    ),
                    source_sha256=source_hash,
                )
            )
    if observed_series != approved_series:
        raise FeatureEventSnapshotError("ALFRED approved series set is incomplete")
    try:
        start = date.fromisoformat(cast("str", report["vintage_start"]))
        end = date.fromisoformat(cast("str", report["vintage_end"]))
    except (KeyError, TypeError, ValueError) as error:
        raise FeatureEventSnapshotError(
            "ALFRED coverage dates are malformed"
        ) from error
    coverage = EventCoverage(
        FeatureEventKind.MACRO,
        "ALFRED_SELECTED_FEDERAL_SERIES",
        _date_start_ns(start),
        _date_start_ns(end + timedelta(days=1)),
        report_hash,
    )
    return tuple(events), coverage, tuple(sorted(used_manifests)), report_hash


def _event_document(event: TemporalFeatureEvent) -> dict[str, object]:
    return {
        "available_at_ns": event.available_at_ns,
        "event_time_ns": event.event_time_ns,
        "instrument_id": event.instrument_id,
        "kind": event.kind.value,
        "record_id": event.record_id,
        "revision_id": event.revision_id,
        "source_sha256": event.source_sha256,
        "valid_until_ns": event.valid_until_ns,
    }


def build_feature_event_snapshot(
    data_root: Path,
    universe: UniverseSnapshot,
    *,
    source_universe_sha256: str,
    sec_coverage_report: Path | None = None,
    alfred_run_report: Path | None = None,
) -> dict[str, object]:
    """Compile authorized sources into one immutable feature-event snapshot."""
    events: list[TemporalFeatureEvent] = []
    coverage: list[EventCoverage] = []
    manifest_ids: set[str] = set()
    report_hashes: set[str] = set()
    if not _is_sha256(source_universe_sha256):
        raise FeatureEventSnapshotError("source universe identity is malformed")
    if sec_coverage_report is not None:
        source_events, source_coverage, sources, report_hash = _sec_events(
            data_root, universe, source_universe_sha256, sec_coverage_report
        )
        events.extend(source_events)
        coverage.append(source_coverage)
        manifest_ids.update(sources)
        report_hashes.add(report_hash)
    if alfred_run_report is not None:
        source_events, source_coverage, sources, report_hash = _alfred_events(
            data_root, alfred_run_report
        )
        events.extend(source_events)
        coverage.append(source_coverage)
        manifest_ids.update(sources)
        report_hashes.add(report_hash)
    if not coverage:
        raise FeatureEventSnapshotError("at least one authenticated source is required")
    if len(events) > MAX_EVENTS or len({item.record_id for item in events}) != len(
        events
    ):
        raise FeatureEventSnapshotError("event snapshot count or identity is invalid")
    ordered_events = tuple(sorted(events, key=lambda item: item.record_id))
    ordered_coverage = tuple(
        sorted(coverage, key=lambda item: (item.kind.value, item.source_id))
    )
    EventIndex(ordered_events, ordered_coverage)
    body: dict[str, object] = {
        "coverage": [item.document() for item in ordered_coverage],
        "economic_value_claimed": False,
        "events": [_event_document(item) for item in ordered_events],
        "live_trading_capable": False,
        "schema_version": FEATURE_EVENT_SNAPSHOT_SCHEMA_VERSION,
        "source_manifest_ids": sorted(manifest_ids),
        "source_report_sha256s": sorted(report_hashes),
        "source_universe_snapshot_sha256": source_universe_sha256,
        "universe_snapshot_sha256": universe.universe_snapshot_sha256.hex(),
    }
    return {**body, "snapshot_sha256": _sha256(_canonical_bytes(body))}


def publish_feature_event_snapshot(
    data_root: Path, snapshot: Mapping[str, object]
) -> Path:
    """Atomically publish a content-addressed event snapshot without overwrite."""
    snapshot_hash = snapshot.get("snapshot_sha256")
    if not _is_sha256(snapshot_hash):
        raise FeatureEventSnapshotError("snapshot identity is malformed")
    encoded = _canonical_bytes(dict(snapshot)) + b"\n"
    if len(encoded) > MAX_EVENT_SNAPSHOT_BYTES:
        raise FeatureEventSnapshotError("event snapshot exceeds its size bound")
    directory = data_root / "reports/feature-events"
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    final = directory / f"snapshot-{snapshot_hash}.json"
    temporary = directory / f".{final.name}.{os.getpid()}.partial"
    if final.exists():
        if final.is_symlink() or final.read_bytes() != encoded:
            raise FeatureEventSnapshotError("published event snapshot conflicts")
        return final
    try:
        descriptor = os.open(
            temporary,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
            0o400,
        )
    except OSError as error:
        raise FeatureEventSnapshotError(
            "event snapshot staging file is unavailable"
        ) from error
    try:
        try:
            try:
                offset = 0
                while offset < len(encoded):
                    written = os.write(descriptor, encoded[offset:])
                    if written <= 0:
                        raise FeatureEventSnapshotError(
                            "event snapshot write was partial"
                        )
                    offset += written
                os.fsync(descriptor)
            except OSError as error:
                raise FeatureEventSnapshotError(
                    "event snapshot write failed"
                ) from error
        finally:
            os.close(descriptor)
        try:
            os.link(temporary, final, follow_symlinks=False)
        except FileExistsError:
            if final.is_symlink() or final.read_bytes() != encoded:
                raise FeatureEventSnapshotError(
                    "published event snapshot conflicts"
                ) from None
        except OSError as error:
            raise FeatureEventSnapshotError(
                "event snapshot publication failed"
            ) from error
    finally:
        temporary.unlink(missing_ok=True)
    directory_descriptor = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory_descriptor)
    finally:
        os.close(directory_descriptor)
    return final


def load_feature_event_snapshot(
    path: Path,
    *,
    expected_universe_sha256: str,
    expected_source_universe_sha256: str,
    expected_instrument_ids: Sequence[str] | None = None,
) -> EventIndex:
    """Authenticate a canonical snapshot and construct the immutable event index."""
    document = _read_json(path, MAX_EVENT_SNAPSHOT_BYTES, "feature event snapshot")
    snapshot_sha256 = _verify_self_hash(
        document, "snapshot_sha256", "feature event snapshot"
    )
    if (
        document.get("schema_version") != FEATURE_EVENT_SNAPSHOT_SCHEMA_VERSION
        or document.get("universe_snapshot_sha256") != expected_universe_sha256
        or document.get("source_universe_snapshot_sha256")
        != expected_source_universe_sha256
        or document.get("live_trading_capable") is not False
        or document.get("economic_value_claimed") is not False
    ):
        raise FeatureEventSnapshotError("feature event snapshot contract mismatch")
    raw_coverage = document.get("coverage")
    raw_events = document.get("events")
    if not isinstance(raw_coverage, list) or not isinstance(raw_events, list):
        raise FeatureEventSnapshotError("feature event snapshot arrays are malformed")
    coverage: list[EventCoverage] = []
    for raw in raw_coverage:
        if not isinstance(raw, dict):
            raise FeatureEventSnapshotError("event coverage row is malformed")
        try:
            coverage.append(
                EventCoverage(
                    FeatureEventKind(cast("str", raw["kind"])),
                    cast("str", raw["source_id"]),
                    cast("int", raw["start_time_ns"]),
                    cast("int", raw["end_time_ns"]),
                    cast("str", raw["source_snapshot_sha256"]),
                )
            )
        except (KeyError, TypeError, ValueError) as error:
            raise FeatureEventSnapshotError("event coverage row is invalid") from error
    events: list[TemporalFeatureEvent] = []
    carried_instruments = set(_snapshot_instruments(raw_events))
    valid_instruments = (
        carried_instruments
        if expected_instrument_ids is None
        else set(expected_instrument_ids)
    )
    for raw in raw_events:
        if not isinstance(raw, dict):
            raise FeatureEventSnapshotError("event row is malformed")
        try:
            instrument_id = cast("str | None", raw["instrument_id"])
            event = TemporalFeatureEvent(
                cast("str", raw["record_id"]),
                cast("str", raw["revision_id"]),
                FeatureEventKind(cast("str", raw["kind"])),
                instrument_id,
                cast("int", raw["event_time_ns"]),
                cast("int", raw["available_at_ns"]),
                cast("int", raw["valid_until_ns"]),
                cast("str", raw["source_sha256"]),
            )
        except (KeyError, TypeError, ValueError) as error:
            raise FeatureEventSnapshotError("event row is invalid") from error
        if instrument_id is not None and instrument_id not in valid_instruments:
            raise FeatureEventSnapshotError("event instrument is outside the universe")
        if not any(
            item.kind is event.kind and item.covers(event.event_time_ns)
            for item in coverage
        ):
            raise FeatureEventSnapshotError(
                "event lies outside asserted source coverage"
            )
        events.append(event)
    return EventIndex(
        tuple(events),
        tuple(coverage),
        authenticated_snapshot_sha256=snapshot_sha256,
    )


def _snapshot_instruments(raw_events: Sequence[object]) -> tuple[str, ...]:
    """Return bounded instrument identities carried by authenticated events."""
    result = {
        cast("str", raw["instrument_id"])
        for raw in raw_events
        if isinstance(raw, dict) and isinstance(raw.get("instrument_id"), str)
    }
    if len(result) > MAX_INSTRUMENTS or any(
        len(item) != _INSTRUMENT_ID_LENGTH
        or any(character not in "0123456789abcdef" for character in item)
        for item in result
    ):
        raise FeatureEventSnapshotError("event instrument set is invalid")
    return tuple(sorted(result))
