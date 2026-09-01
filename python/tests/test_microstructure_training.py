"""Deterministic training, export, validation, and parity tests."""

from __future__ import annotations

import hashlib
import json
import math
import sys
from dataclasses import replace
from typing import TYPE_CHECKING

import aegis_mx_training.microstructure as training
import pytest
from aegis_mx_training import (
    MODEL_SPECS,
    Dataset,
    ModelArtifact,
    ModelSpec,
    TrainingConfig,
    encode_artifact,
    export_suite,
    load_dataset,
    predict,
    train_suite,
)
from aegis_mx_training.__main__ import main as training_main
from aegis_mx_training.microstructure import (
    FeatureTransform,
    deterministic_sigmoid_ppm,
)

if TYPE_CHECKING:
    from pathlib import Path


_COLUMNS = (
    "source_event_hash",
    "top_level_imbalance_ppm",
    "spread_ticks",
    "order_flow_imbalance_units",
    "signed_trade_imbalance_ppm",
    "add_rate_millihertz",
    "cancel_rate_millihertz",
    "execute_rate_millihertz",
    "queue_depletion_rate_units_per_second",
    "rolling_return_ppm",
    "realized_volatility_ppm",
    "trade_intensity_millihertz",
    "quote_intensity_millihertz",
    "data_quality_code",
    "session_progress_ppm",
    "quantity_ahead_units",
    "order_quantity_units",
    "order_age_ns",
    "price_distance_ticks",
    "venue_number",
    "fee_rate_ppm",
    "label_mid_price_up",
    "label_spread_widening",
    "label_queue_depletion",
    "label_passive_fill",
    "label_adverse_selection",
    "target_cost_ppm",
    "target_impact_ppm",
)


def _dataset_text(row_count: int = 32) -> str:
    lines = [",".join(_COLUMNS)]
    for index in range(row_count):
        direction = index % 2
        spread = (index % 5) + 1
        cancel = (index % 4) * 1_000
        execute = (index % 3) * 1_000
        values = (
            index + 1,
            (-500_000) + (index * 30_000),
            spread,
            (index - 16) * 25,
            (direction * 2 - 1) * 10_000,
            (index + 1) * 1_000,
            cancel,
            execute,
            cancel + execute,
            (direction * 2 - 1) * 200,
            (index % 7) * 1_000,
            execute,
            (index + 1) * 1_000,
            1,
            (index + 1) * 25_000,
            100 + index,
            10 + index,
            index * 1_000,
            index % 3,
            (index % 2) + 1,
            30,
            direction,
            int(spread > 2),
            int(cancel + execute > 0),
            int(execute > 0),
            int(execute > 0 and direction == 0),
            30 + (spread * 100) + index,
            20 + (index * 2),
        )
        lines.append(",".join(str(value) for value in values))
    return "\n".join(lines) + "\n"


@pytest.fixture
def synthetic_dataset(tmp_path: Path) -> Dataset:
    path = tmp_path / "synthetic.csv"
    path.write_text(_dataset_text(), encoding="ascii")
    return load_dataset(path)


def test_train_export_is_deterministic_infrastructure_validation(
    synthetic_dataset: Dataset, tmp_path: Path
) -> None:
    config = TrainingConfig(seed=20_260_829, epochs=30)
    first = train_suite(synthetic_dataset, config)
    second = train_suite(synthetic_dataset, config)
    assert len(first) == len(MODEL_SPECS) == 7
    assert [encode_artifact(item[0]) for item in first] == [
        encode_artifact(item[0]) for item in second
    ]
    assert all(report.final_loss_ppm <= report.initial_loss_ppm for _, report in first)
    assert all(not report.economic_value_claim for _, report in first)
    for artifact, report in first:
        encoded = encode_artifact(artifact)
        body, signature = encoded.rsplit(b"signature_sha256=", maxsplit=1)
        assert hashlib.sha256(body).hexdigest() == signature.strip().decode("ascii")
        assert report.artifact_sha256 == signature.strip().decode("ascii")
        assert predict(artifact, synthetic_dataset.rows[0]) == predict(
            artifact, synthetic_dataset.rows[0]
        )

    reports = export_suite(synthetic_dataset, tmp_path / "models", config)
    assert len(reports) == 7
    assert sorted(path.stem for path in (tmp_path / "models").glob("*.amdl")) == sorted(
        spec.role for spec in MODEL_SPECS
    )


def test_queue_contract_and_cost_breakdown(synthetic_dataset: Dataset) -> None:
    trained = {
        artifact.spec.role: artifact for artifact, _ in train_suite(synthetic_dataset)
    }
    queue = trained["queue_depletion"]
    assert tuple(transform.name for transform in queue.transforms) == (
        "quantity_ahead_units",
        "add_rate_millihertz",
        "cancel_rate_millihertz",
        "execute_rate_millihertz",
        "order_age_ns",
        "price_distance_ticks",
        "venue_number",
        "session_progress_ppm",
    )
    cost = predict(trained["transaction_cost"], synthetic_dataset.rows[0])
    assert cost.fee_cost_ppm > 0
    assert cost.spread_cost_ppm > 0
    assert cost.slippage_cost_ppm > 0
    assert cost.market_impact_ppm > 0
    assert cost.adverse_selection_cost_ppm > 0


def test_fixed_predictor_rejects_missing_and_ood_data() -> None:
    artifact = ModelArtifact(
        spec=ModelSpec(
            "mid_price_direction", "logistic_regression", ("spread_ticks",), "label"
        ),
        transforms=(FeatureTransform("spread_ticks", 0, 10, 0, 10, 1_000_000),),
        intercept_ppm=0,
        platt_slope_ppm=1_000_000,
        platt_intercept_ppm=0,
        training_sample_count=10,
        calibration_error_ppm=0,
        expires_wall_clock_utc_ns=1_900_000_000_000_000_000,
        horizon_ns=1,
        forecast_ttl_ns=1,
        ood_reject_threshold_ppm=100_000,
        ordinal=1,
    )
    assert deterministic_sigmoid_ppm(500_000) == 615_529
    with pytest.raises(ValueError, match="missing inference feature"):
        predict(artifact, {})
    with pytest.raises(ValueError, match="out-of-distribution"):
        predict(artifact, {"spread_ticks": 20})
    with pytest.raises(ValueError, match="outside native range"):
        predict(artifact, {"spread_ticks": 20_000_000})


@pytest.mark.parametrize(
    ("payload", "maximum_rows", "message"),
    [
        ("", 10, "size"),
        ("wrong\n1\n", 10, "header"),
        (_dataset_text(3), 10, "at least four"),
        (_dataset_text(4), 3, "row bound"),
    ],
)
def test_dataset_validation(
    tmp_path: Path, payload: str, maximum_rows: int, message: str
) -> None:
    path = tmp_path / "invalid.csv"
    path.write_text(payload, encoding="ascii")
    with pytest.raises(ValueError, match=message):
        load_dataset(path, maximum_rows=maximum_rows)


def test_invalid_training_inputs_fail_closed(
    synthetic_dataset: Dataset, tmp_path: Path
) -> None:
    with pytest.raises(ValueError, match="maximum_rows"):
        load_dataset(tmp_path / "missing.csv", maximum_rows=0)
    with pytest.raises(ValueError, match="training configuration"):
        train_suite(synthetic_dataset, TrainingConfig(seed=0))

    invalid_rows = tuple(
        {**row, "label_mid_price_up": 2} for row in synthetic_dataset.rows
    )
    invalid_dataset = replace(synthetic_dataset, rows=invalid_rows)
    with pytest.raises(ValueError, match="not binary"):
        train_suite(invalid_dataset)

    oversized_rows = tuple(
        {**row, "spread_ticks": 20_000_000} for row in synthetic_dataset.rows
    )
    oversized_dataset = replace(synthetic_dataset, rows=oversized_rows)
    with pytest.raises(ValueError, match="outside native range"):
        train_suite(oversized_dataset)


@pytest.mark.parametrize(
    ("row_suffix", "message"),
    [
        (",1", "shape"),
        ("", "shape"),
        (None, "base-10"),
        (str(2**63), "outside int64"),
    ],
)
def test_dataset_rejects_malformed_rows(
    tmp_path: Path, row_suffix: str | None, message: str
) -> None:
    lines = _dataset_text(4).splitlines()
    if row_suffix == ",1":
        lines[1] += row_suffix
    elif row_suffix == "":
        lines[1] = lines[1].rsplit(",", maxsplit=1)[0]
    else:
        values = lines[1].split(",")
        values[1] = row_suffix if row_suffix is not None else "not-an-integer"
        lines[1] = ",".join(values)
    path = tmp_path / "malformed.csv"
    path.write_text("\n".join(lines) + "\n", encoding="ascii")
    with pytest.raises(ValueError, match=message):
        load_dataset(path)


def test_dataset_rejects_non_ascii(tmp_path: Path) -> None:
    path = tmp_path / "non-ascii.csv"
    path.write_bytes(b"\xff")
    with pytest.raises(ValueError, match="ASCII"):
        load_dataset(path)


def test_defensive_training_failures(
    synthetic_dataset: Dataset, monkeypatch: pytest.MonkeyPatch
) -> None:
    invalid_scale = ModelArtifact(
        spec=ModelSpec("mid_price_direction", "logistic_regression", ("x",), "y"),
        transforms=(FeatureTransform("x", 0, -1, -1, 1, 1),),
        intercept_ppm=0,
        platt_slope_ppm=1_000_000,
        platt_intercept_ppm=0,
        training_sample_count=4,
        calibration_error_ppm=0,
        expires_wall_clock_utc_ns=1,
        horizon_ns=1,
        forecast_ttl_ns=1,
        ood_reject_threshold_ppm=1_000_000,
        ordinal=1,
    )
    with pytest.raises(ValueError, match="denominator"):
        predict(invalid_scale, {"x": 0})

    empty_spec = ModelSpec("empty", "linear_regression", (), "target_cost_ppm")
    monkeypatch.setattr(training, "MODEL_SPECS", (empty_spec,))
    with pytest.raises(ValueError, match="feature count"):
        train_suite(synthetic_dataset)

    monkeypatch.setattr(training, "MODEL_SPECS", MODEL_SPECS)
    monkeypatch.setattr(math, "isfinite", lambda _value: False)
    with pytest.raises(RuntimeError, match="did not converge"):
        train_suite(synthetic_dataset)


def test_predictor_saturation_and_ood_lower_bound() -> None:
    transform = FeatureTransform("x", 0, 1, 0, 10, 0)
    artifact = ModelArtifact(
        spec=ModelSpec("mid_price_direction", "logistic_regression", ("x",), "y"),
        transforms=(transform,),
        intercept_ppm=0,
        platt_slope_ppm=1_000_000,
        platt_intercept_ppm=0,
        training_sample_count=4,
        calibration_error_ppm=0,
        expires_wall_clock_utc_ns=1,
        horizon_ns=1,
        forecast_ttl_ns=1,
        ood_reject_threshold_ppm=1_000_000,
        ordinal=1,
    )
    assert deterministic_sigmoid_ppm(9_000_000) == 999_665
    assert predict(artifact, {"x": -1}).ood_score_ppm > 0

    invalid_normalization = replace(
        artifact,
        transforms=(FeatureTransform("x", -10_000_000, 1, -10_000_000, 10_000_000, 0),),
    )
    with pytest.raises(ValueError, match="normalized feature"):
        predict(invalid_normalization, {"x": 10_000_000})


def test_training_cli_emits_auditable_report(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    dataset_path = tmp_path / "synthetic.csv"
    output_path = tmp_path / "models"
    dataset_path.write_text(_dataset_text(), encoding="ascii")
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "aegis-mx-train",
            "--dataset",
            str(dataset_path),
            "--output-directory",
            str(output_path),
            "--seed",
            "20260829",
            "--expires-wall-clock-utc-ns",
            "1900000000000000000",
        ],
    )
    assert training_main() == 0
    report = json.loads(capsys.readouterr().out)
    assert report["economic_value_claim"] is False
    assert len(report["reports"]) == 7
