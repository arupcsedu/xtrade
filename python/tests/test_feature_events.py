"""Point-in-time feature-event snapshot compiler tests."""

from __future__ import annotations

import hashlib
import json
import os
from datetime import date
from pathlib import Path
from typing import TYPE_CHECKING, cast

import pytest
from aegis_mx_intelligence import Identifier128, InstrumentId
from aegis_mx_research import feature_events as event_module
from aegis_mx_research.alfred import APPROVED_SERIES
from aegis_mx_research.feature_dataset import (
    EventCoverage,
    FeatureEventKind,
    TemporalFeatureEvent,
)
from aegis_mx_research.feature_events import (
    FeatureEventSnapshotError,
    build_feature_event_snapshot,
    load_feature_event_snapshot,
    publish_feature_event_snapshot,
)
from aegis_mx_research.forecast_contracts import (
    StaticInstrumentResolver,
    UniverseSnapshot,
    parse_ticker_universe,
)
from aegis_mx_research.sec_edgar import historical_submissions_url
from jsonschema import (  # type: ignore[import-untyped]
    Draft202012Validator,
    FormatChecker,
)

if TYPE_CHECKING:
    from collections.abc import Callable

SOURCE_UNIVERSE_SHA = "a" * 64
INSTRUMENT = InstrumentId(Identifier128(7, 11))
INSTRUMENT_HEX = cast("Identifier128", INSTRUMENT).hex()
RECEIPT_NS = 1_789_169_000_000_000_000
ACCEPTANCE_NS = 1_725_465_600_000_000_000
DAY_NS = 86_400_000_000_000


def _canonical_bytes(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("ascii")


def _sha256(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _self_hash(body: dict[str, object], field: str) -> dict[str, object]:
    return {**body, field: _sha256(_canonical_bytes(body))}


def _rewrite_self_hashed(
    path: Path, field: str, mutate: Callable[[dict[str, object]], None]
) -> dict[str, object]:
    document = cast("dict[str, object]", json.loads(path.read_text()))
    document.pop(field)
    mutate(document)
    rewritten = _self_hash(document, field)
    path.write_bytes(_canonical_bytes(rewritten) + b"\n")
    return rewritten


def _replace_sec_object(
    root: Path,
    payload: bytes,
    *,
    manifest_updates: dict[str, object] | None = None,
) -> None:
    old_manifest = next((root / "manifests/sec-edgar").glob("sec-*.json"))
    old = cast("dict[str, object]", json.loads(old_manifest.read_text()))
    old_manifest.unlink()
    digest = _sha256(payload)
    relative = f"raw/sec-edgar/submissions/{digest}.json"
    target = root / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(payload)
    body = {
        key: value
        for key, value in old.items()
        if key not in {"manifest_id", "manifest_sha256"}
    }
    body.update(
        {
            "object_sha256": digest,
            "size_bytes_decimal": len(payload),
            "storage_path": relative,
        }
    )
    body.update(manifest_updates or {})
    body_hash = _sha256(_canonical_bytes(body))
    document = {
        **body,
        "manifest_id": f"sec-{body_hash}",
        "manifest_sha256": body_hash,
    }
    (root / f"manifests/sec-edgar/sec-{body_hash}.json").write_bytes(
        _canonical_bytes(document) + b"\n"
    )


def _publish_sec_object(
    root: Path,
    payload: bytes,
    *,
    source_url_sha256: str,
    record_count: int,
) -> str:
    digest = _sha256(payload)
    relative = f"raw/sec-edgar/submissions/{digest}.json"
    target = root / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(payload)
    body: dict[str, object] = {
        "dataset": "SUBMISSIONS",
        "object_sha256": digest,
        "processing_time_utc_ns": RECEIPT_NS + 1,
        "receipt_time_utc_ns": RECEIPT_NS,
        "record_count": record_count,
        "size_bytes_decimal": len(payload),
        "source_url_sha256": source_url_sha256,
        "storage_path": relative,
        "universe_snapshot_sha256": SOURCE_UNIVERSE_SHA,
    }
    manifest_hash = _sha256(_canonical_bytes(body))
    manifest_id = f"sec-{manifest_hash}"
    directory = root / "manifests/sec-edgar"
    directory.mkdir(parents=True, exist_ok=True)
    (directory / f"{manifest_id}.json").write_bytes(
        _canonical_bytes(
            {
                **body,
                "manifest_id": manifest_id,
                "manifest_sha256": manifest_hash,
            }
        )
        + b"\n"
    )
    return manifest_id


def _load_sec_artifacts(root: Path) -> tuple[object, ...]:
    return event_module._load_artifacts(
        root,
        root / "manifests/sec-edgar",
        manifest_prefix="sec",
        object_prefix="raw/sec-edgar/submissions/",
        universe_sha256=SOURCE_UNIVERSE_SHA,
        kind_field="dataset",
        kind_value="SUBMISSIONS",
    )


def _replace_one_alfred_snapshot(
    root: Path,
    report_path: Path,
    mutate: Callable[[dict[str, object]], None],
) -> None:
    manifest_path = next((root / "manifests/alfred").glob("alfred-*.json"))
    manifest = cast("dict[str, object]", json.loads(manifest_path.read_text()))
    old_manifest_id = cast("str", manifest["manifest_id"])
    object_path = root / cast("str", manifest["storage_path"])
    snapshot = cast("dict[str, object]", json.loads(object_path.read_text()))
    old_snapshot_hash = cast("str", snapshot.pop("snapshot_sha256"))
    mutate(snapshot)
    rewritten = _self_hash(snapshot, "snapshot_sha256")
    encoded = _canonical_bytes(rewritten) + b"\n"
    object_hash = _sha256(encoded)
    series_id = cast("dict[str, object]", rewritten["series_contract"])["series_id"]
    relative = (
        f"canonical/alfred/{str(series_id).lower()}/canonical_snapshot/"
        f"{object_hash}.json"
    )
    target = root / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(encoded)
    body = {
        key: value
        for key, value in manifest.items()
        if key not in {"manifest_id", "manifest_sha256"}
    }
    body.update(
        {
            "object_sha256": object_hash,
            "size_bytes_decimal": len(encoded),
            "storage_path": relative,
        }
    )
    manifest_hash = _sha256(_canonical_bytes(body))
    new_manifest_id = f"alfred-{manifest_hash}"
    manifest_path.unlink()
    (root / f"manifests/alfred/{new_manifest_id}.json").write_bytes(
        _canonical_bytes(
            {
                **body,
                "manifest_id": new_manifest_id,
                "manifest_sha256": manifest_hash,
            }
        )
        + b"\n"
    )

    def update_report(document: dict[str, object]) -> None:
        manifests = cast("list[str]", document["manifest_ids"])
        manifests[manifests.index(old_manifest_id)] = new_manifest_id
        snapshots = cast("list[str]", document["snapshot_sha256s"])
        snapshots[snapshots.index(old_snapshot_hash)] = cast(
            "str", rewritten["snapshot_sha256"]
        )

    _rewrite_self_hashed(report_path, "report_sha256", update_report)


def _universe() -> UniverseSnapshot:
    return parse_ticker_universe(
        b"AAA\n", StaticInstrumentResolver({"AAA": INSTRUMENT})
    )


def _write_sec_fixture(root: Path) -> Path:
    raw = {
        "cik": "7",
        "filings": {
            "files": [],
            "recent": {
                "accessionNumber": ["0000000007-24-000001"],
                "acceptanceDateTime": ["2024-09-04T16:00:00.000Z"],
                "filingDate": ["2024-09-04"],
                "form": ["8-K"],
                "primaryDocument": ["event.htm"],
                "reportDate": ["2024-09-04"],
            },
        },
    }
    payload = _canonical_bytes(raw)
    digest = _sha256(payload)
    relative = f"raw/sec-edgar/submissions/{digest}.json"
    target = root / relative
    target.parent.mkdir(parents=True)
    target.write_bytes(payload)
    manifest_body: dict[str, object] = {
        "dataset": "SUBMISSIONS",
        "object_sha256": digest,
        "processing_time_utc_ns": RECEIPT_NS + 1,
        "receipt_time_utc_ns": RECEIPT_NS,
        "record_count": 1,
        "size_bytes_decimal": len(payload),
        "source_url_sha256": "b" * 64,
        "storage_path": relative,
        "universe_snapshot_sha256": SOURCE_UNIVERSE_SHA,
    }
    manifest_hash = _sha256(_canonical_bytes(manifest_body))
    manifest = {
        **manifest_body,
        "manifest_id": f"sec-{manifest_hash}",
        "manifest_sha256": manifest_hash,
    }
    manifest_path = root / f"manifests/sec-edgar/sec-{manifest_hash}.json"
    manifest_path.parent.mkdir(parents=True)
    manifest_path.write_bytes(_canonical_bytes(manifest) + b"\n")
    report_body: dict[str, object] = {
        "filing_end_date": "2024-09-30",
        "filing_start_date": "2024-09-01",
        "generated_at_utc_ns": RECEIPT_NS + 10,
        "issuer_coverage": [
            {
                "amendment_count": 0,
                "cik": "0000000007",
                "errors": [],
                "fact_count": 0,
                "filing_count": 1,
                "forms": {"8-K": 1},
                "historical_shard_count": 0,
                "issuer_name": "AAA Corp",
                "resolution_status": "RESOLVED_CURRENT_ONLY",
                "ticker": "AAA",
            }
        ],
        "network_access_performed": True,
        "request_count": 2,
        "schema_version": "1.1.0",
        "summary": {
            "amendment_count": 0,
            "fact_count": 0,
            "filing_count": 1,
            "historical_shard_count": 0,
            "requested_ticker_count": 1,
            "resolved_ticker_count": 1,
            "unresolved_ticker_count": 0,
        },
        "universe_sha256": SOURCE_UNIVERSE_SHA,
    }
    report = _self_hash(report_body, "report_sha256")
    report_path = root / "reports/sec.json"
    report_path.parent.mkdir(parents=True)
    report_path.write_bytes(_canonical_bytes(report) + b"\n")
    return report_path


def _write_alfred_fixture(root: Path) -> Path:
    manifest_ids: list[str] = []
    snapshot_hashes: list[str] = []
    coverage: dict[str, int] = {}
    for offset, spec in enumerate(APPROVED_SERIES):
        release_ns = ACCEPTANCE_NS + offset * DAY_NS
        release_payload = {
            "known_at_utc_ns": release_ns,
            "record_sha256": _sha256(spec.series_id.encode()),
            "release_date": f"2024-09-{4 + offset:02d}",
        }
        snapshot_body: dict[str, object] = {
            "releases": [release_payload],
            "series_contract": {"series_id": spec.series_id},
        }
        snapshot = _self_hash(snapshot_body, "snapshot_sha256")
        payload = _canonical_bytes(snapshot) + b"\n"
        object_hash = _sha256(payload)
        relative = (
            f"canonical/alfred/{spec.series_id.lower()}/canonical_snapshot/"
            f"{object_hash}.json"
        )
        target = root / relative
        target.parent.mkdir(parents=True)
        target.write_bytes(payload)
        manifest_body: dict[str, object] = {
            "object_kind": "CANONICAL_SNAPSHOT",
            "object_sha256": object_hash,
            "size_bytes_decimal": len(payload),
            "storage_path": relative,
        }
        manifest_hash = _sha256(_canonical_bytes(manifest_body))
        manifest_id = f"alfred-{manifest_hash}"
        manifest = {
            **manifest_body,
            "manifest_id": manifest_id,
            "manifest_sha256": manifest_hash,
        }
        manifest_path = root / f"manifests/alfred/{manifest_id}.json"
        manifest_path.parent.mkdir(parents=True, exist_ok=True)
        manifest_path.write_bytes(_canonical_bytes(manifest) + b"\n")
        manifest_ids.append(manifest_id)
        snapshot_hashes.append(cast("str", snapshot["snapshot_sha256"]))
        coverage[spec.series_id] = 1
    report_body: dict[str, object] = {
        "coverage": {"series": coverage},
        "manifest_ids": manifest_ids,
        "network_access_performed": True,
        "schema_version": "1.0.0",
        "snapshot_sha256s": snapshot_hashes,
        "vintage_end": "2024-09-30",
        "vintage_start": "2024-09-01",
    }
    report = _self_hash(report_body, "report_sha256")
    report_path = root / "reports/alfred.json"
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_bytes(_canonical_bytes(report) + b"\n")
    return report_path


def test_compiler_publishes_and_loads_sec_and_alfred_events(tmp_path: Path) -> None:
    universe = _universe()
    sec = _write_sec_fixture(tmp_path)
    alfred = _write_alfred_fixture(tmp_path)
    snapshot = build_feature_event_snapshot(
        tmp_path,
        universe,
        source_universe_sha256=SOURCE_UNIVERSE_SHA,
        sec_coverage_report=sec,
        alfred_run_report=alfred,
    )
    assert len(cast("list[object]", snapshot["events"])) == 1 + len(APPROVED_SERIES)
    schema = json.loads(
        Path("schemas/feature-event-snapshot-v1.schema.json").read_text()
    )
    Draft202012Validator(schema, format_checker=FormatChecker()).validate(snapshot)
    path = publish_feature_event_snapshot(tmp_path, snapshot)
    index = load_feature_event_snapshot(
        path,
        expected_universe_sha256=universe.universe_snapshot_sha256.hex(),
        expected_source_universe_sha256=SOURCE_UNIVERSE_SHA,
        expected_instrument_ids=(INSTRUMENT_HEX,),
    )
    assert index.sha256 == snapshot["snapshot_sha256"]
    assert index.content_sha256 != index.sha256
    assert index.active(INSTRUMENT_HEX, ACCEPTANCE_NS)[:2] == (True, True)
    assert len(index.active(INSTRUMENT_HEX, ACCEPTANCE_NS)[2]) == 2
    assert publish_feature_event_snapshot(tmp_path, snapshot) == path


def test_unavailable_kind_is_none_and_tamper_is_rejected(tmp_path: Path) -> None:
    universe = _universe()
    sec = _write_sec_fixture(tmp_path)
    snapshot = build_feature_event_snapshot(
        tmp_path,
        universe,
        source_universe_sha256=SOURCE_UNIVERSE_SHA,
        sec_coverage_report=sec,
    )
    path = publish_feature_event_snapshot(tmp_path, snapshot)
    index = load_feature_event_snapshot(
        path,
        expected_universe_sha256=universe.universe_snapshot_sha256.hex(),
        expected_source_universe_sha256=SOURCE_UNIVERSE_SHA,
    )
    assert index.active(INSTRUMENT_HEX, ACCEPTANCE_NS)[:2] == (True, None)
    tampered = json.loads(path.read_text())
    tampered["events"][0]["available_at_ns"] += 1
    path.chmod(0o600)
    path.write_bytes(_canonical_bytes(tampered) + b"\n")
    with pytest.raises(FeatureEventSnapshotError, match="self-hash"):
        load_feature_event_snapshot(
            path,
            expected_universe_sha256=universe.universe_snapshot_sha256.hex(),
            expected_source_universe_sha256=SOURCE_UNIVERSE_SHA,
        )


def test_sec_coverage_mismatch_fails_closed(tmp_path: Path) -> None:
    universe = _universe()
    report_path = _write_sec_fixture(tmp_path)
    report = json.loads(report_path.read_text())
    report.pop("report_sha256")
    report["issuer_coverage"][0]["filing_count"] = 2
    report["summary"]["filing_count"] = 2
    report_path.write_bytes(
        _canonical_bytes(_self_hash(report, "report_sha256")) + b"\n"
    )
    with pytest.raises(FeatureEventSnapshotError, match="reproduce coverage"):
        build_feature_event_snapshot(
            tmp_path,
            universe,
            source_universe_sha256=SOURCE_UNIVERSE_SHA,
            sec_coverage_report=report_path,
        )


def test_low_level_file_time_and_path_guards(tmp_path: Path) -> None:
    with pytest.raises(FeatureEventSnapshotError, match="unavailable or unsafe"):
        event_module._read_regular(tmp_path / "missing", 10, "fixture")
    empty = tmp_path / "empty"
    empty.write_bytes(b"")
    with pytest.raises(FeatureEventSnapshotError, match="empty, truncated"):
        event_module._read_regular(empty, 10, "fixture")
    oversized = tmp_path / "oversized"
    oversized.write_bytes(b"xx")
    with pytest.raises(FeatureEventSnapshotError, match="empty, truncated"):
        event_module._read_regular(oversized, 1, "fixture")
    malformed = tmp_path / "malformed.json"
    malformed.write_bytes(b"{")
    with pytest.raises(FeatureEventSnapshotError, match="malformed JSON"):
        event_module._read_json(malformed, 10, "fixture")
    malformed.write_bytes(b"\xff")
    with pytest.raises(FeatureEventSnapshotError, match="malformed JSON"):
        event_module._read_json(malformed, 10, "fixture")
    malformed.write_bytes(b"[]")
    with pytest.raises(FeatureEventSnapshotError, match="JSON object"):
        event_module._read_json(malformed, 10, "fixture")
    with pytest.raises(FeatureEventSnapshotError, match="no valid"):
        event_module._verify_self_hash({}, "sha256", "fixture")
    with pytest.raises(FeatureEventSnapshotError, match="exceeds int64"):
        event_module._date_start_ns(date(1970, 1, 1))
    with pytest.raises(FeatureEventSnapshotError, match="exceeds int64"):
        event_module._checked_window_end(event_module.MAX_INT64, 1)

    root = tmp_path / "root"
    root.mkdir()
    with pytest.raises(FeatureEventSnapshotError, match="outside its boundary"):
        event_module._safe_object_path(root, "../escape", "raw/sec/")
    with pytest.raises(FeatureEventSnapshotError, match="missing"):
        event_module._safe_object_path(root, "raw/sec/missing", "raw/sec/")
    outside = tmp_path / "outside"
    outside.write_bytes(b"outside")
    link = root / "raw/sec/link"
    link.parent.mkdir(parents=True)
    link.symlink_to(outside)
    with pytest.raises(FeatureEventSnapshotError, match="unsafe"):
        event_module._safe_object_path(root, "raw/sec/link", "raw/sec/")

    unresolved = parse_ticker_universe(b"MISSING\n", StaticInstrumentResolver({}))
    assert event_module._universe_instruments(unresolved) == {}


def test_source_manifest_integrity_guards(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    valid = tmp_path / "valid"
    _write_sec_fixture(valid)
    assert len(_load_sec_artifacts(valid)) == 1

    wrong_kind = tmp_path / "wrong-kind"
    _write_sec_fixture(wrong_kind)
    manifest = next((wrong_kind / "manifests/sec-edgar").glob("sec-*.json"))
    _rewrite_self_hashed(
        manifest,
        "manifest_sha256",
        lambda document: document.__setitem__("dataset", "COMPANY_FACTS"),
    )
    # The manifest ID also commits the body hash, so a mutated in-place file is
    # rejected before its nonmatching dataset can be ignored.
    with pytest.raises(FeatureEventSnapshotError, match="self-hash"):
        _load_sec_artifacts(wrong_kind)

    ignored = tmp_path / "ignored"
    _write_sec_fixture(ignored)
    manifest = next((ignored / "manifests/sec-edgar").glob("sec-*.json"))
    document = cast("dict[str, object]", json.loads(manifest.read_text()))
    body = {
        key: value
        for key, value in document.items()
        if key not in {"manifest_id", "manifest_sha256"}
    }
    body["dataset"] = "COMPANY_FACTS"
    digest = _sha256(_canonical_bytes(body))
    replacement = {
        **body,
        "manifest_id": f"sec-{digest}",
        "manifest_sha256": digest,
    }
    manifest.unlink()
    (ignored / f"manifests/sec-edgar/sec-{digest}.json").write_bytes(
        _canonical_bytes(replacement) + b"\n"
    )
    assert _load_sec_artifacts(ignored) == ()

    invalid_identity = tmp_path / "identity"
    directory = invalid_identity / "manifests/sec-edgar"
    directory.mkdir(parents=True)
    (directory / "sec-invalid.json").write_text("{}")
    with pytest.raises(FeatureEventSnapshotError, match="identity"):
        _load_sec_artifacts(invalid_identity)

    wrong_universe = tmp_path / "universe"
    _write_sec_fixture(wrong_universe)
    with pytest.raises(FeatureEventSnapshotError, match="universe mismatch"):
        event_module._load_artifacts(
            wrong_universe,
            wrong_universe / "manifests/sec-edgar",
            manifest_prefix="sec",
            object_prefix="raw/sec-edgar/submissions/",
            universe_sha256="f" * 64,
            kind_field="dataset",
            kind_value="SUBMISSIONS",
        )

    invalid_metadata = tmp_path / "metadata"
    _write_sec_fixture(invalid_metadata)
    payload = next((invalid_metadata / "raw/sec-edgar/submissions").glob("*.json"))
    _replace_sec_object(
        invalid_metadata,
        payload.read_bytes(),
        manifest_updates={"size_bytes_decimal": 0},
    )
    with pytest.raises(FeatureEventSnapshotError, match="metadata"):
        _load_sec_artifacts(invalid_metadata)

    corrupt = tmp_path / "corrupt"
    _write_sec_fixture(corrupt)
    payload = next((corrupt / "raw/sec-edgar/submissions").glob("*.json"))
    payload.write_bytes(payload.read_bytes() + b" ")
    with pytest.raises(FeatureEventSnapshotError, match="size or SHA-256"):
        _load_sec_artifacts(corrupt)

    too_many = tmp_path / "too-many"
    _write_sec_fixture(too_many)
    monkeypatch.setattr(event_module, "MAX_SOURCE_MANIFESTS", 0)
    with pytest.raises(FeatureEventSnapshotError, match="count exceeds"):
        _load_sec_artifacts(too_many)


def test_manifest_directory_read_failure(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    def fail_glob(_path: Path, _pattern: str) -> object:
        message = "synthetic directory failure"
        raise OSError(message)

    monkeypatch.setattr(Path, "glob", fail_glob)
    with pytest.raises(FeatureEventSnapshotError, match="directory is unreadable"):
        _load_sec_artifacts(tmp_path)


@pytest.mark.parametrize(
    ("case", "message"),
    [
        ("schema", "incompatible"),
        ("missing_date", "date coverage is malformed"),
        ("reversed_date", "date coverage is invalid"),
        ("coverage_not_array", "issuer coverage is malformed"),
        ("row_not_object", "coverage row is malformed"),
        ("resolution", "does not match universe"),
        ("counts", "issuer counts are malformed"),
        ("summary", "aggregate coverage does not reproduce"),
    ],
)
def test_sec_report_contract_guards(tmp_path: Path, case: str, message: str) -> None:
    report_path = _write_sec_fixture(tmp_path)

    def mutate(document: dict[str, object]) -> None:
        if case == "schema":
            document["schema_version"] = "0.0.0"
        elif case == "missing_date":
            document.pop("filing_start_date")
        elif case == "reversed_date":
            document["filing_start_date"] = "2024-10-01"
        elif case == "coverage_not_array":
            document["issuer_coverage"] = {}
        elif case == "row_not_object":
            document["issuer_coverage"] = [1]
        elif case == "resolution":
            rows = cast("list[dict[str, object]]", document["issuer_coverage"])
            rows[0]["resolution_status"] = "AMBIGUOUS"
        elif case == "counts":
            rows = cast("list[dict[str, object]]", document["issuer_coverage"])
            rows[0]["filing_count"] = "one"
        else:
            summary = cast("dict[str, object]", document["summary"])
            summary["resolved_ticker_count"] = 2

    _rewrite_self_hashed(report_path, "report_sha256", mutate)
    with pytest.raises(FeatureEventSnapshotError, match=message):
        build_feature_event_snapshot(
            tmp_path,
            _universe(),
            source_universe_sha256=SOURCE_UNIVERSE_SHA,
            sec_coverage_report=report_path,
        )


@pytest.mark.parametrize(
    ("case", "message"),
    [
        ("missing_artifact", "submissions artifacts are unavailable"),
        ("malformed_json", "submissions object is malformed"),
        ("malformed_utf8", "submissions object is malformed"),
        ("not_object", "do not reproduce coverage"),
        ("no_filings", "do not reproduce coverage"),
        ("bad_cik", "do not reproduce coverage"),
        ("bad_receipt", "do not reproduce coverage"),
        ("count_mismatch", "do not reproduce coverage"),
        ("parse_failure", "do not reproduce coverage"),
    ],
)
def test_sec_source_reproduction_guards(
    tmp_path: Path, case: str, message: str
) -> None:
    report_path = _write_sec_fixture(tmp_path)
    manifest = next((tmp_path / "manifests/sec-edgar").glob("sec-*.json"))
    payload_path = next((tmp_path / "raw/sec-edgar/submissions").glob("*.json"))
    payload = payload_path.read_bytes()
    if case == "missing_artifact":
        manifest.unlink()
    elif case == "malformed_json":
        _replace_sec_object(tmp_path, b"{")
    elif case == "malformed_utf8":
        _replace_sec_object(tmp_path, b"\xff")
    elif case == "not_object":
        _replace_sec_object(tmp_path, b"[]")
    elif case == "no_filings":
        _replace_sec_object(tmp_path, b'{"cik":"7"}')
    elif case == "bad_cik":
        document = cast("dict[str, object]", json.loads(payload))
        document["cik"] = []
        _replace_sec_object(tmp_path, _canonical_bytes(document))
    elif case == "bad_receipt":
        _replace_sec_object(
            tmp_path, payload, manifest_updates={"receipt_time_utc_ns": "late"}
        )
    elif case == "count_mismatch":
        _replace_sec_object(tmp_path, payload, manifest_updates={"record_count": 2})
    else:
        document = cast("dict[str, object]", json.loads(payload))
        recent = cast(
            "dict[str, object]",
            cast("dict[str, object]", document["filings"])["recent"],
        )
        recent.pop("form")
        _replace_sec_object(tmp_path, _canonical_bytes(document))
    with pytest.raises(FeatureEventSnapshotError, match=message):
        build_feature_event_snapshot(
            tmp_path,
            _universe(),
            source_universe_sha256=SOURCE_UNIVERSE_SHA,
            sec_coverage_report=report_path,
        )


def test_sec_unresolved_row_and_nontext_url_hash_are_handled(tmp_path: Path) -> None:
    report_path = _write_sec_fixture(tmp_path)
    manifest_path = next((tmp_path / "manifests/sec-edgar").glob("sec-*.json"))
    payload = next((tmp_path / "raw/sec-edgar/submissions").glob("*.json"))
    _replace_sec_object(
        tmp_path, payload.read_bytes(), manifest_updates={"source_url_sha256": 7}
    )
    snapshot = build_feature_event_snapshot(
        tmp_path,
        _universe(),
        source_universe_sha256=SOURCE_UNIVERSE_SHA,
        sec_coverage_report=report_path,
    )
    assert len(cast("list[object]", snapshot["events"])) == 1
    assert not manifest_path.exists()

    def unresolved(document: dict[str, object]) -> None:
        rows = cast("list[dict[str, object]]", document["issuer_coverage"])
        rows[0]["resolution_status"] = "UNRESOLVED"
        summary = cast("dict[str, object]", document["summary"])
        summary["resolved_ticker_count"] = 0
        summary["unresolved_ticker_count"] = 1
        summary["filing_count"] = 0

    _rewrite_self_hashed(report_path, "report_sha256", unresolved)
    snapshot = build_feature_event_snapshot(
        tmp_path,
        _universe(),
        source_universe_sha256=SOURCE_UNIVERSE_SHA,
        sec_coverage_report=report_path,
    )
    assert snapshot["events"] == []


@pytest.mark.parametrize(
    "availability", ["present", "missing", "count-mismatch", "empty"]
)
def test_sec_historical_shard_reproduction(tmp_path: Path, availability: str) -> None:
    report_path = _write_sec_fixture(tmp_path)
    original_payload = next(
        (tmp_path / "raw/sec-edgar/submissions").glob("*.json")
    ).read_bytes()
    current = {
        "cik": "7",
        "filings": {
            "files": [
                {
                    "filingCount": 1,
                    "filingFrom": "2024-09-01",
                    "filingTo": "2024-09-30",
                    "name": "CIK0000000007-submissions-001.json",
                }
            ],
            "recent": {
                "accessionNumber": [],
                "acceptanceDateTime": [],
                "filingDate": [],
                "form": [],
                "primaryDocument": [],
                "reportDate": [],
            },
        },
    }
    _replace_sec_object(
        tmp_path, _canonical_bytes(current), manifest_updates={"record_count": 0}
    )
    if availability != "missing":
        filename = "CIK0000000007-submissions-001.json"
        current_filings = cast("dict[str, object]", current["filings"])
        historical_payload = (
            _canonical_bytes(current_filings["recent"])
            if availability == "empty"
            else original_payload
        )
        _publish_sec_object(
            tmp_path,
            historical_payload,
            source_url_sha256=_sha256(historical_submissions_url(filename).encode()),
            record_count=2
            if availability == "count-mismatch"
            else int(availability != "empty"),
        )

    def update_report(document: dict[str, object]) -> None:
        rows = cast("list[dict[str, object]]", document["issuer_coverage"])
        rows[0]["historical_shard_count"] = 1

    _rewrite_self_hashed(report_path, "report_sha256", update_report)
    if availability != "present":
        with pytest.raises(FeatureEventSnapshotError, match="do not reproduce"):
            build_feature_event_snapshot(
                tmp_path,
                _universe(),
                source_universe_sha256=SOURCE_UNIVERSE_SHA,
                sec_coverage_report=report_path,
            )
        return
    snapshot = build_feature_event_snapshot(
        tmp_path,
        _universe(),
        source_universe_sha256=SOURCE_UNIVERSE_SHA,
        sec_coverage_report=report_path,
    )
    assert len(cast("list[object]", snapshot["events"])) == 1
    assert len(cast("list[str]", snapshot["source_manifest_ids"])) == 2


def test_sec_duplicate_accession_across_issuers_is_rejected(tmp_path: Path) -> None:
    second_instrument = InstrumentId(Identifier128(13, 17))
    universe = parse_ticker_universe(
        b"AAA\nBBB\n",
        StaticInstrumentResolver({"AAA": INSTRUMENT, "BBB": second_instrument}),
    )

    def payload(cik: int) -> bytes:
        return _canonical_bytes(
            {
                "cik": str(cik),
                "filings": {
                    "files": [],
                    "recent": {
                        "accessionNumber": ["0000000007-24-000001"],
                        "acceptanceDateTime": ["2024-09-04T16:00:00.000Z"],
                        "filingDate": ["2024-09-04"],
                        "form": ["8-K"],
                        "primaryDocument": ["event.htm"],
                        "reportDate": ["2024-09-04"],
                    },
                },
            }
        )

    for cik in (7, 8):
        _publish_sec_object(
            tmp_path,
            payload(cik),
            source_url_sha256=_sha256(f"issuer-{cik}".encode()),
            record_count=1,
        )
    rows = [
        {
            "amendment_count": 0,
            "cik": f"{cik:010d}",
            "errors": [],
            "fact_count": 0,
            "filing_count": 1,
            "forms": {"8-K": 1},
            "historical_shard_count": 0,
            "issuer_name": f"Issuer {ticker}",
            "resolution_status": "RESOLVED_CURRENT_ONLY",
            "ticker": ticker,
        }
        for ticker, cik in (("AAA", 7), ("BBB", 8))
    ]
    report_body: dict[str, object] = {
        "filing_end_date": "2024-09-30",
        "filing_start_date": "2024-09-01",
        "generated_at_utc_ns": RECEIPT_NS + 10,
        "issuer_coverage": rows,
        "network_access_performed": True,
        "request_count": 2,
        "schema_version": "1.1.0",
        "summary": {
            "amendment_count": 0,
            "fact_count": 0,
            "filing_count": 2,
            "historical_shard_count": 0,
            "requested_ticker_count": 2,
            "resolved_ticker_count": 2,
            "unresolved_ticker_count": 0,
        },
        "universe_sha256": SOURCE_UNIVERSE_SHA,
    }
    report_path = tmp_path / "reports/sec-duplicate.json"
    report_path.parent.mkdir(parents=True)
    report_path.write_bytes(
        _canonical_bytes(_self_hash(report_body, "report_sha256")) + b"\n"
    )
    with pytest.raises(FeatureEventSnapshotError, match="accession is duplicated"):
        build_feature_event_snapshot(
            tmp_path,
            universe,
            source_universe_sha256=SOURCE_UNIVERSE_SHA,
            sec_coverage_report=report_path,
        )


@pytest.mark.parametrize(
    ("case", "message"),
    [
        ("schema", "coverage is incomplete"),
        ("dry_run", "coverage is incomplete"),
        ("missing_manifest", "snapshots are incomplete"),
        ("unbound_snapshot", "not bound to its report"),
        ("content", "content is malformed"),
        ("series", "series is not approved"),
        ("release_row", "release row is malformed"),
        ("release_identity", "release identity is malformed"),
        ("duplicate_series", "series set is incomplete"),
        ("dates", "coverage dates are malformed"),
    ],
)
def test_alfred_contract_guards(tmp_path: Path, case: str, message: str) -> None:
    report_path = _write_alfred_fixture(tmp_path)
    if case in {
        "schema",
        "dry_run",
        "missing_manifest",
        "unbound_snapshot",
        "dates",
    }:

        def mutate_report(document: dict[str, object]) -> None:
            if case == "schema":
                document["schema_version"] = "0.0.0"
            elif case == "dry_run":
                document["network_access_performed"] = False
            elif case == "missing_manifest":
                manifests = cast("list[str]", document["manifest_ids"])
                manifests[0] = "alfred-" + "f" * 64
            elif case == "unbound_snapshot":
                snapshots = cast("list[str]", document["snapshot_sha256s"])
                snapshots[0] = "f" * 64
            else:
                document["vintage_start"] = "not-a-date"

        _rewrite_self_hashed(report_path, "report_sha256", mutate_report)
    else:

        def mutate_snapshot(document: dict[str, object]) -> None:
            if case == "content":
                document["releases"] = {}
            elif case == "series":
                contract = cast("dict[str, object]", document["series_contract"])
                contract["series_id"] = "UNAPPROVED"
            elif case == "release_row":
                document["releases"] = [1]
            elif case == "release_identity":
                releases = cast("list[dict[str, object]]", document["releases"])
                releases[0]["record_sha256"] = "bad"
            else:
                contract = cast("dict[str, object]", document["series_contract"])
                other = next(
                    item.series_id
                    for item in APPROVED_SERIES
                    if item.series_id != contract["series_id"]
                )
                contract["series_id"] = other

        _replace_one_alfred_snapshot(tmp_path, report_path, mutate_snapshot)
    with pytest.raises(FeatureEventSnapshotError, match=message):
        build_feature_event_snapshot(
            tmp_path,
            _universe(),
            source_universe_sha256=SOURCE_UNIVERSE_SHA,
            alfred_run_report=report_path,
        )


def test_alfred_snapshot_malformed_encoding_is_rejected(tmp_path: Path) -> None:
    report_path = _write_alfred_fixture(tmp_path)
    manifest_path = next((tmp_path / "manifests/alfred").glob("alfred-*.json"))
    manifest = cast("dict[str, object]", json.loads(manifest_path.read_text()))
    old_manifest_id = cast("str", manifest["manifest_id"])
    payload = b"\xff"
    digest = _sha256(payload)
    relative = f"canonical/alfred/bad/canonical_snapshot/{digest}.json"
    target = tmp_path / relative
    target.parent.mkdir(parents=True)
    target.write_bytes(payload)
    body = {
        key: value
        for key, value in manifest.items()
        if key not in {"manifest_id", "manifest_sha256"}
    }
    body.update(
        {
            "object_sha256": digest,
            "size_bytes_decimal": len(payload),
            "storage_path": relative,
        }
    )
    manifest_hash = _sha256(_canonical_bytes(body))
    new_manifest_id = f"alfred-{manifest_hash}"
    manifest_path.unlink()
    (tmp_path / f"manifests/alfred/{new_manifest_id}.json").write_bytes(
        _canonical_bytes(
            {
                **body,
                "manifest_id": new_manifest_id,
                "manifest_sha256": manifest_hash,
            }
        )
        + b"\n"
    )

    def update_report(document: dict[str, object]) -> None:
        manifests = cast("list[str]", document["manifest_ids"])
        manifests[manifests.index(old_manifest_id)] = new_manifest_id

    _rewrite_self_hashed(report_path, "report_sha256", update_report)
    with pytest.raises(FeatureEventSnapshotError, match="malformed JSON"):
        build_feature_event_snapshot(
            tmp_path,
            _universe(),
            source_universe_sha256=SOURCE_UNIVERSE_SHA,
            alfred_run_report=report_path,
        )


def test_snapshot_build_source_and_identity_guards(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    with pytest.raises(FeatureEventSnapshotError, match="source universe"):
        build_feature_event_snapshot(
            tmp_path, _universe(), source_universe_sha256="bad"
        )
    with pytest.raises(FeatureEventSnapshotError, match="at least one"):
        build_feature_event_snapshot(
            tmp_path,
            _universe(),
            source_universe_sha256=SOURCE_UNIVERSE_SHA,
        )
    event = TemporalFeatureEvent(
        "duplicate",
        "revision",
        FeatureEventKind.NEWS,
        INSTRUMENT_HEX,
        ACCEPTANCE_NS,
        ACCEPTANCE_NS,
        ACCEPTANCE_NS + DAY_NS,
        "b" * 64,
    )
    coverage = EventCoverage(
        FeatureEventKind.NEWS,
        "FIXTURE",
        ACCEPTANCE_NS - 1,
        ACCEPTANCE_NS + DAY_NS,
        "c" * 64,
    )
    monkeypatch.setattr(
        event_module,
        "_sec_events",
        lambda *_args: ((event, event), coverage, (), "d" * 64),
    )
    with pytest.raises(FeatureEventSnapshotError, match="count or identity"):
        build_feature_event_snapshot(
            tmp_path,
            _universe(),
            source_universe_sha256=SOURCE_UNIVERSE_SHA,
            sec_coverage_report=tmp_path / "unused",
        )


def _published_sec_snapshot(root: Path) -> tuple[Path, dict[str, object]]:
    report = _write_sec_fixture(root)
    snapshot = build_feature_event_snapshot(
        root,
        _universe(),
        source_universe_sha256=SOURCE_UNIVERSE_SHA,
        sec_coverage_report=report,
    )
    return publish_feature_event_snapshot(root, snapshot), snapshot


def test_snapshot_publication_guards(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    with pytest.raises(FeatureEventSnapshotError, match="identity is malformed"):
        publish_feature_event_snapshot(tmp_path, {})

    report = _write_sec_fixture(tmp_path)
    snapshot = build_feature_event_snapshot(
        tmp_path,
        _universe(),
        source_universe_sha256=SOURCE_UNIVERSE_SHA,
        sec_coverage_report=report,
    )
    monkeypatch.setattr(event_module, "MAX_EVENT_SNAPSHOT_BYTES", 1)
    with pytest.raises(FeatureEventSnapshotError, match="size bound"):
        publish_feature_event_snapshot(tmp_path, snapshot)


def test_snapshot_publication_conflict_and_partial_write(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    conflict_root = tmp_path / "conflict"
    path, snapshot = _published_sec_snapshot(conflict_root)
    path.chmod(0o600)
    path.write_bytes(b"conflict")
    with pytest.raises(FeatureEventSnapshotError, match="conflicts"):
        publish_feature_event_snapshot(conflict_root, snapshot)

    partial_root = tmp_path / "partial"
    report = _write_sec_fixture(partial_root)
    snapshot = build_feature_event_snapshot(
        partial_root,
        _universe(),
        source_universe_sha256=SOURCE_UNIVERSE_SHA,
        sec_coverage_report=report,
    )
    monkeypatch.setattr(os, "write", lambda *_args: 0)
    with pytest.raises(FeatureEventSnapshotError, match="write was partial"):
        publish_feature_event_snapshot(partial_root, snapshot)
    assert not tuple((partial_root / "reports/feature-events").glob("*.partial"))


@pytest.mark.parametrize(
    ("failure", "message"),
    [
        ("staging", "staging file is unavailable"),
        ("write", "snapshot write failed"),
        ("link", "snapshot publication failed"),
        ("race-conflict", "snapshot conflicts"),
    ],
)
def test_snapshot_publication_operating_system_failures(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    failure: str,
    message: str,
) -> None:
    report = _write_sec_fixture(tmp_path)
    snapshot = build_feature_event_snapshot(
        tmp_path,
        _universe(),
        source_universe_sha256=SOURCE_UNIVERSE_SHA,
        sec_coverage_report=report,
    )

    def fail(*_args: object, **_kwargs: object) -> None:
        detail = "synthetic operating-system failure"
        raise OSError(detail)

    if failure == "staging":
        monkeypatch.setattr(os, "open", fail)
    elif failure == "write":
        monkeypatch.setattr(os, "write", fail)
    elif failure == "link":
        monkeypatch.setattr(os, "link", fail)
    else:

        def conflicting_link(
            source: Path,
            destination: Path,
            *,
            follow_symlinks: bool,
        ) -> None:
            del source, follow_symlinks
            destination.write_bytes(b"conflict")
            raise FileExistsError

        monkeypatch.setattr(os, "link", conflicting_link)
    with pytest.raises(FeatureEventSnapshotError, match=message):
        publish_feature_event_snapshot(tmp_path, snapshot)


def test_snapshot_publication_accepts_identical_concurrent_winner(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    report = _write_sec_fixture(tmp_path)
    snapshot = build_feature_event_snapshot(
        tmp_path,
        _universe(),
        source_universe_sha256=SOURCE_UNIVERSE_SHA,
        sec_coverage_report=report,
    )

    def winning_link(
        source: Path,
        destination: Path,
        *,
        follow_symlinks: bool,
    ) -> None:
        del follow_symlinks
        destination.write_bytes(source.read_bytes())
        raise FileExistsError

    monkeypatch.setattr(os, "link", winning_link)
    path = publish_feature_event_snapshot(tmp_path, snapshot)
    assert (
        json.loads(path.read_text())["snapshot_sha256"] == snapshot["snapshot_sha256"]
    )


@pytest.mark.parametrize(
    ("case", "message"),
    [
        ("contract", "contract mismatch"),
        ("arrays", "arrays are malformed"),
        ("coverage_row", "coverage row is malformed"),
        ("coverage_value", "coverage row is invalid"),
        ("event_row", "event row is malformed"),
        ("event_value", "event row is invalid"),
        ("instrument_shape", "event instrument set is invalid"),
        ("outside_coverage", "outside asserted source coverage"),
    ],
)
def test_snapshot_load_contract_guards(tmp_path: Path, case: str, message: str) -> None:
    path, _ = _published_sec_snapshot(tmp_path)

    def mutate(document: dict[str, object]) -> None:
        if case == "contract":
            document["schema_version"] = "0.0.0"
        elif case == "arrays":
            document["events"] = {}
        elif case == "coverage_row":
            document["coverage"] = [1]
        elif case == "coverage_value":
            coverage = cast("list[dict[str, object]]", document["coverage"])
            coverage[0]["kind"] = "INVALID"
        elif case == "event_row":
            document["events"] = [1]
        elif case == "event_value":
            events = cast("list[dict[str, object]]", document["events"])
            events[0]["kind"] = "INVALID"
        elif case == "instrument_shape":
            events = cast("list[dict[str, object]]", document["events"])
            events[0]["instrument_id"] = "bad"
        else:
            coverage = cast("list[dict[str, object]]", document["coverage"])
            coverage[0]["start_time_ns"] = ACCEPTANCE_NS + 1

    path.chmod(0o600)
    _rewrite_self_hashed(path, "snapshot_sha256", mutate)
    with pytest.raises(FeatureEventSnapshotError, match=message):
        load_feature_event_snapshot(
            path,
            expected_universe_sha256=_universe().universe_snapshot_sha256.hex(),
            expected_source_universe_sha256=SOURCE_UNIVERSE_SHA,
        )


def test_snapshot_load_rejects_instrument_outside_expected_set(tmp_path: Path) -> None:
    path, _ = _published_sec_snapshot(tmp_path)
    with pytest.raises(FeatureEventSnapshotError, match="outside the universe"):
        load_feature_event_snapshot(
            path,
            expected_universe_sha256=_universe().universe_snapshot_sha256.hex(),
            expected_source_universe_sha256=SOURCE_UNIVERSE_SHA,
            expected_instrument_ids=("f" * 32,),
        )
