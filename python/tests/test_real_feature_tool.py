"""Tests for the operator-facing real Prompt 57 dataset command."""

from __future__ import annotations

from types import SimpleNamespace
from typing import TYPE_CHECKING

import pytest

from tools import build_real_feature_dataset as tool

if TYPE_CHECKING:
    from pathlib import Path


def test_cli_is_dry_run_by_default(capsys: pytest.CaptureFixture[str]) -> None:
    assert tool.main([]) == 0
    output = capsys.readouterr().out
    assert '"status": "PLANNED"' in output
    assert '"network_access_performed": false' in output


def test_real_feature_wiring_and_fail_closed_count(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    backfill = {"kind": "backfill", "universe_snapshot_sha256": "11" * 32}
    promotion = {"status": "COMPLETED", "counts": {"published_records": 2}}
    monkeypatch.setattr(
        tool,
        "_read_document",
        lambda _path, label: backfill if "backfill" in label else promotion,
    )
    universe = SimpleNamespace(
        universe_snapshot_sha256=SimpleNamespace(hex=lambda: "33" * 32)
    )
    calendar = object()
    monkeypatch.setattr(
        tool, "_universe_and_calendar", lambda _body: (universe, calendar)
    )
    monkeypatch.setattr(
        tool, "_partition_inputs", lambda _repository, _body, _universe: ()
    )
    source = SimpleNamespace(record_count=2, instrument_ids=("instrument-1",))
    monkeypatch.setattr(tool, "ParquetCanonicalSource", lambda _partitions: source)
    event_index = SimpleNamespace(
        sha256="22" * 32,
        coverage_document=({"kind": "NEWS"},),
    )
    event_path = tmp_path / "events.json"

    def load_events(path: Path, **expected: object) -> object:
        assert path == event_path
        assert expected == {
            "expected_instrument_ids": ("instrument-1",),
            "expected_source_universe_sha256": "11" * 32,
            "expected_universe_sha256": "33" * 32,
        }
        return event_index

    monkeypatch.setattr(tool, "load_feature_event_snapshot", load_events)
    builder = SimpleNamespace(events=event_index)

    def make_builder(*_args: object, events: object | None = None) -> object:
        assert events is event_index
        return builder

    monkeypatch.setattr(tool, "FeatureDatasetBuilder", make_builder)
    monkeypatch.setattr(tool, "_quota_evidence", lambda _root: object())
    published = SimpleNamespace(
        coverage=tuple(range(948)),
        dataset_id="dataset-id",
        leakage=SimpleNamespace(status="PASS"),
        manifest_path=tmp_path / "manifest.json",
        object_count=4,
        sample_count=100,
        session_summary_count=90,
    )
    monkeypatch.setattr(
        tool, "build_and_publish_feature_dataset", lambda *_args: published
    )
    result = tool.build_real_feature_dataset(
        tmp_path,
        tmp_path / "backfill.json",
        tmp_path / "promotion.json",
        event_path,
    )
    assert result["coverage_rows"] == 948
    assert result["event_feature_coverage"] == [{"kind": "NEWS"}]
    assert result["event_snapshot_sha256"] == "22" * 32
    assert result["leakage_status"] == "PASS"
    assert result["live_trading_capable"] is False

    source.record_count = 1
    with pytest.raises(ValueError, match="count does not match"):
        tool.build_real_feature_dataset(
            tmp_path, tmp_path / "backfill.json", tmp_path / "promotion.json"
        )


def test_configuration_version_rejects_malformed_digest() -> None:
    with pytest.raises(ValueError, match="malformed"):
        tool._configuration_version("00")
