"""Train and export deterministic, bounded microstructure model artifacts.

The datasets produced by this module are synthetic infrastructure fixtures.
Metrics prove plumbing, export, and inference parity only; they do not establish
predictive or economic value.
"""

from __future__ import annotations

import csv
import hashlib
import math
from dataclasses import dataclass
from types import MappingProxyType
from typing import TYPE_CHECKING, Final, Literal, cast

if TYPE_CHECKING:
    from collections.abc import Mapping
    from pathlib import Path

PPM: Final = 1_000_000
MAX_ABSOLUTE_VALUE: Final = 10_000_000
MAX_FEATURES: Final = 16
MAX_DATASET_BYTES: Final = 1_000_000_000
MINIMUM_TRAINING_ROWS: Final = 4

Family = Literal["logistic_regression", "linear_regression"]


@dataclass(frozen=True, slots=True)
class ModelSpec:
    """Immutable training contract for one model role."""

    role: str
    family: Family
    features: tuple[str, ...]
    target: str


@dataclass(frozen=True, slots=True)
class Dataset:
    """Validated integer rows from the synthetic exchange generator."""

    columns: tuple[str, ...]
    rows: tuple[Mapping[str, int], ...]
    sha256: str


@dataclass(frozen=True, slots=True)
class TrainingConfig:
    """Explicit deterministic trainer and artifact settings."""

    seed: int = 20_260_829
    epochs: int = 80
    learning_rate_ppm: int = 100_000
    l2_penalty_ppm: int = 1_000
    expires_wall_clock_utc_ns: int = 1_900_000_000_000_000_000
    horizon_ns: int = 1_000_000
    forecast_ttl_ns: int = 1_000_000
    ood_reject_threshold_ppm: int = 250_000


@dataclass(frozen=True, slots=True)
class FeatureTransform:
    """Fixed normalization and coefficient for one ordered feature."""

    name: str
    center: int
    scale: int
    training_minimum: int
    training_maximum: int
    weight_ppm: int


@dataclass(frozen=True, slots=True)
class ModelArtifact:
    """In-memory representation of the stable native artifact."""

    spec: ModelSpec
    transforms: tuple[FeatureTransform, ...]
    intercept_ppm: int
    platt_slope_ppm: int
    platt_intercept_ppm: int
    training_sample_count: int
    calibration_error_ppm: int
    expires_wall_clock_utc_ns: int
    horizon_ns: int
    forecast_ttl_ns: int
    ood_reject_threshold_ppm: int
    ordinal: int


@dataclass(frozen=True, slots=True)
class TrainingReport:
    """Auditable evidence emitted for one deterministic training run."""

    role: str
    seed: int
    sample_count: int
    initial_loss_ppm: int
    final_loss_ppm: int
    artifact_sha256: str
    economic_value_claim: bool = False


@dataclass(frozen=True, slots=True)
class NativePrediction:
    """Python implementation of the C++ native predictor output subset."""

    expected_return_ppm: int
    probability_up_ppm: int
    probability_down_ppm: int
    ood_score_ppm: int
    fee_cost_ppm: int = 0
    spread_cost_ppm: int = 0
    slippage_cost_ppm: int = 0
    market_impact_ppm: int = 0
    adverse_selection_cost_ppm: int = 0


_QUEUE_FEATURES: Final = (
    "quantity_ahead_units",
    "add_rate_millihertz",
    "cancel_rate_millihertz",
    "execute_rate_millihertz",
    "order_age_ns",
    "price_distance_ticks",
    "venue_number",
    "session_progress_ppm",
)

MODEL_SPECS: Final = (
    ModelSpec(
        "mid_price_direction",
        "logistic_regression",
        (
            "top_level_imbalance_ppm",
            "order_flow_imbalance_units",
            "signed_trade_imbalance_ppm",
            "rolling_return_ppm",
            "realized_volatility_ppm",
        ),
        "label_mid_price_up",
    ),
    ModelSpec(
        "spread_widening",
        "logistic_regression",
        (
            "spread_ticks",
            "realized_volatility_ppm",
            "quote_intensity_millihertz",
            "data_quality_code",
            "session_progress_ppm",
        ),
        "label_spread_widening",
    ),
    ModelSpec(
        "queue_depletion",
        "logistic_regression",
        _QUEUE_FEATURES,
        "label_queue_depletion",
    ),
    ModelSpec(
        "passive_fill_probability",
        "logistic_regression",
        (*_QUEUE_FEATURES, "order_quantity_units"),
        "label_passive_fill",
    ),
    ModelSpec(
        "adverse_selection",
        "logistic_regression",
        (
            "order_flow_imbalance_units",
            "signed_trade_imbalance_ppm",
            "rolling_return_ppm",
            "realized_volatility_ppm",
            "spread_ticks",
            "session_progress_ppm",
        ),
        "label_adverse_selection",
    ),
    ModelSpec(
        "transaction_cost",
        "linear_regression",
        (
            "spread_ticks",
            "order_quantity_units",
            "price_distance_ticks",
            "fee_rate_ppm",
            "realized_volatility_ppm",
            "trade_intensity_millihertz",
        ),
        "target_cost_ppm",
    ),
    ModelSpec(
        "market_impact",
        "linear_regression",
        (
            "order_quantity_units",
            "price_distance_ticks",
            "trade_intensity_millihertz",
            "realized_volatility_ppm",
        ),
        "target_impact_ppm",
    ),
)

_REQUIRED_COLUMNS: Final = frozenset(
    {"source_event_hash"}
    | {feature for spec in MODEL_SPECS for feature in spec.features}
    | {spec.target for spec in MODEL_SPECS}
)
_SIGMOID_TABLE: Final = (
    335,
    911,
    2_473,
    6_693,
    17_986,
    47_426,
    119_203,
    268_941,
    500_000,
    731_059,
    880_797,
    952_574,
    982_014,
    993_307,
    997_527,
    999_089,
    999_665,
)


def _trunc_div(numerator: int, denominator: int) -> int:
    """Match C++ signed integer division (truncate toward zero)."""
    if denominator <= 0:
        msg = "denominator must be positive"
        raise ValueError(msg)
    magnitude = abs(numerator) // denominator
    return -magnitude if numerator < 0 else magnitude


def _validate_config(config: TrainingConfig) -> None:
    if (
        config.seed <= 0
        or config.epochs <= 0
        or not 0 < config.learning_rate_ppm <= PPM
        or not 0 <= config.l2_penalty_ppm <= PPM
        or config.expires_wall_clock_utc_ns <= 0
        or config.horizon_ns <= 0
        or config.forecast_ttl_ns <= 0
        or not 0 <= config.ood_reject_threshold_ppm <= PPM
    ):
        msg = "invalid deterministic training configuration"
        raise ValueError(msg)


def _parse_row(
    raw_row: dict[str | None, str | None], columns: tuple[str, ...]
) -> Mapping[str, int]:
    if None in raw_row or any(
        value is None or value == "" for value in raw_row.values()
    ):
        msg = "dataset row shape is invalid"
        raise ValueError(msg)
    parsed: dict[str, int] = {}
    for name in columns:
        # DictReader materializes every declared key; the shape guard above
        # rejects its None sentinel before parsing.
        raw_value = cast("str", raw_row[name])
        try:
            parsed[name] = int(raw_value, 10)
        except ValueError as error:
            msg = "dataset values must be base-10 integers"
            raise ValueError(msg) from error
    if any(abs(value) > (2**63 - 1) for value in parsed.values()):
        msg = "dataset integer is outside int64"
        raise ValueError(msg)
    return MappingProxyType(parsed)


def load_dataset(path: Path, *, maximum_rows: int = 1_000_000) -> Dataset:
    """Load a bounded, exact-header integer CSV as untrusted offline data."""
    if maximum_rows <= 0:
        msg = "maximum_rows must be positive"
        raise ValueError(msg)
    payload = path.read_bytes()
    if not payload or len(payload) > MAX_DATASET_BYTES:
        msg = "dataset size is invalid"
        raise ValueError(msg)
    try:
        text = payload.decode("ascii")
    except UnicodeDecodeError as error:
        msg = "dataset must be ASCII"
        raise ValueError(msg) from error
    reader = csv.DictReader(text.splitlines())
    columns = tuple(reader.fieldnames or ())
    if len(columns) != len(set(columns)) or not _REQUIRED_COLUMNS.issubset(columns):
        msg = "dataset header does not satisfy the model contract"
        raise ValueError(msg)
    rows: list[Mapping[str, int]] = []
    for raw_row in reader:
        if len(rows) >= maximum_rows:
            msg = "dataset row bound exceeded"
            raise ValueError(msg)
        rows.append(_parse_row(raw_row, columns))
    if len(rows) < MINIMUM_TRAINING_ROWS:
        msg = "at least four synthetic rows are required"
        raise ValueError(msg)
    return Dataset(columns, tuple(rows), hashlib.sha256(payload).hexdigest())


def _normalization(dataset: Dataset, feature: str) -> tuple[int, int, int, int]:
    values = [row[feature] for row in dataset.rows]
    minimum = min(values)
    maximum = max(values)
    center = _trunc_div(minimum + maximum, 2)
    scale = max(1, abs(minimum - center), abs(maximum - center))
    if any(
        abs(value) > MAX_ABSOLUTE_VALUE for value in (minimum, maximum, center, scale)
    ):
        msg = f"feature outside native range: {feature}"
        raise ValueError(msg)
    return center, scale, minimum, maximum


def _normalized_rows(
    dataset: Dataset, spec: ModelSpec
) -> tuple[tuple[tuple[float, ...], float], ...]:
    normalization = [_normalization(dataset, feature) for feature in spec.features]
    normalized: list[tuple[tuple[float, ...], float]] = []
    for row in dataset.rows:
        features = tuple(
            (row[name] - center) / scale
            for name, (center, scale, _minimum, _maximum) in zip(
                spec.features, normalization, strict=True
            )
        )
        target = float(row[spec.target])
        if spec.family == "linear_regression":
            target /= PPM
        elif target not in (0.0, 1.0):
            msg = f"classification target is not binary: {spec.target}"
            raise ValueError(msg)
        normalized.append((features, target))
    return tuple(normalized)


def _loss(
    samples: tuple[tuple[tuple[float, ...], float], ...],
    weights: list[float],
    intercept: float,
    family: Family,
) -> float:
    total = 0.0
    for features, target in samples:
        score = intercept + sum(
            value * weight for value, weight in zip(features, weights, strict=True)
        )
        if family == "logistic_regression":
            prediction = 1.0 / (1.0 + math.exp(-max(-20.0, min(20.0, score))))
            total -= target * math.log(max(prediction, 1e-12)) + (
                1.0 - target
            ) * math.log(max(1.0 - prediction, 1e-12))
        else:
            total += (score - target) ** 2
    return total / len(samples)


def _train_one(
    dataset: Dataset, spec: ModelSpec, config: TrainingConfig, ordinal: int
) -> tuple[ModelArtifact, int, int]:
    if not 0 < len(spec.features) <= MAX_FEATURES:
        msg = f"invalid feature count for {spec.role}"
        raise ValueError(msg)
    samples = _normalized_rows(dataset, spec)
    weights = [0.0] * len(spec.features)
    intercept = 0.0
    initial_loss = _loss(samples, weights, intercept, spec.family)
    learning_rate = config.learning_rate_ppm / PPM
    penalty = config.l2_penalty_ppm / PPM
    for _epoch in range(config.epochs):
        gradients = [0.0] * len(weights)
        intercept_gradient = 0.0
        for features, target in samples:
            score = intercept + sum(
                value * weight for value, weight in zip(features, weights, strict=True)
            )
            prediction = (
                1.0 / (1.0 + math.exp(-max(-20.0, min(20.0, score))))
                if spec.family == "logistic_regression"
                else score
            )
            residual = prediction - target
            intercept_gradient += residual
            for index, value in enumerate(features):
                gradients[index] += residual * value
        inverse_count = 1.0 / len(samples)
        intercept -= learning_rate * intercept_gradient * inverse_count
        for index, gradient in enumerate(gradients):
            regularized = (gradient * inverse_count) + (penalty * weights[index])
            weights[index] -= learning_rate * regularized
    final_loss = _loss(samples, weights, intercept, spec.family)
    if not math.isfinite(final_loss) or final_loss > initial_loss:
        msg = f"training did not converge for {spec.role}"
        raise RuntimeError(msg)

    transforms = tuple(
        FeatureTransform(
            name,
            center,
            scale,
            minimum,
            maximum,
            max(-MAX_ABSOLUTE_VALUE, min(MAX_ABSOLUTE_VALUE, round(weight * PPM))),
        )
        for name, weight, (center, scale, minimum, maximum) in zip(
            spec.features,
            weights,
            (_normalization(dataset, name) for name in spec.features),
            strict=True,
        )
    )
    artifact = ModelArtifact(
        spec=spec,
        transforms=transforms,
        intercept_ppm=max(
            -MAX_ABSOLUTE_VALUE,
            min(MAX_ABSOLUTE_VALUE, round(intercept * PPM)),
        ),
        platt_slope_ppm=PPM,
        platt_intercept_ppm=0,
        training_sample_count=len(samples),
        calibration_error_ppm=0,
        expires_wall_clock_utc_ns=config.expires_wall_clock_utc_ns,
        horizon_ns=config.horizon_ns,
        forecast_ttl_ns=config.forecast_ttl_ns,
        ood_reject_threshold_ppm=config.ood_reject_threshold_ppm,
        ordinal=ordinal,
    )
    calibration_error = (
        round(
            sum(
                abs(
                    predict(artifact, row).probability_up_ppm - (row[spec.target] * PPM)
                )
                for row in dataset.rows
            )
            / len(dataset.rows)
        )
        if spec.family == "logistic_regression"
        else 0
    )
    artifact = ModelArtifact(
        spec=artifact.spec,
        transforms=artifact.transforms,
        intercept_ppm=artifact.intercept_ppm,
        platt_slope_ppm=artifact.platt_slope_ppm,
        platt_intercept_ppm=artifact.platt_intercept_ppm,
        training_sample_count=artifact.training_sample_count,
        calibration_error_ppm=min(PPM, calibration_error),
        expires_wall_clock_utc_ns=artifact.expires_wall_clock_utc_ns,
        horizon_ns=artifact.horizon_ns,
        forecast_ttl_ns=artifact.forecast_ttl_ns,
        ood_reject_threshold_ppm=artifact.ood_reject_threshold_ppm,
        ordinal=artifact.ordinal,
    )
    report_scale = PPM if spec.family == "logistic_regression" else PPM * PPM
    return (
        artifact,
        round(initial_loss * report_scale),
        round(final_loss * report_scale),
    )


def deterministic_sigmoid_ppm(score_ppm: int) -> int:
    """Evaluate the canonical integer sigmoid table used by C++."""
    clipped = max(-8 * PPM, min(8 * PPM, score_ppm))
    shifted = clipped + (8 * PPM)
    lower = shifted // PPM
    if lower >= len(_SIGMOID_TABLE) - 1:
        return _SIGMOID_TABLE[-1]
    remainder = shifted % PPM
    delta = _SIGMOID_TABLE[lower + 1] - _SIGMOID_TABLE[lower]
    return _SIGMOID_TABLE[lower] + ((delta * remainder) // PPM)


def _ood_contribution(raw: int, transform: FeatureTransform) -> int:
    if raw < transform.training_minimum:
        distance = transform.training_minimum - raw
    elif raw > transform.training_maximum:
        distance = raw - transform.training_maximum
    else:
        return 0
    span = transform.training_maximum - transform.training_minimum
    return min(PPM, (distance * PPM) // max(1, span + distance))


def predict(artifact: ModelArtifact, row: Mapping[str, int]) -> NativePrediction:
    """Run the exact fixed-point operations implemented in the C++ wrapper."""
    score = artifact.intercept_ppm
    ood_score = 0
    for transform in artifact.transforms:
        try:
            raw = row[transform.name]
        except KeyError as error:
            msg = f"missing inference feature: {transform.name}"
            raise ValueError(msg) from error
        if abs(raw) > MAX_ABSOLUTE_VALUE:
            msg = f"inference feature outside native range: {transform.name}"
            raise ValueError(msg)
        normalized = _trunc_div((raw - transform.center) * PPM, transform.scale)
        if abs(normalized) > MAX_ABSOLUTE_VALUE:
            msg = f"normalized feature outside native range: {transform.name}"
            raise ValueError(msg)
        score += _trunc_div(normalized * transform.weight_ppm, PPM)
        ood_score = max(ood_score, _ood_contribution(raw, transform))
    if ood_score > artifact.ood_reject_threshold_ppm:
        msg = "out-of-distribution score exceeds artifact threshold"
        raise ValueError(msg)

    if artifact.spec.family == "logistic_regression":
        calibrated = _trunc_div(score * artifact.platt_slope_ppm, PPM)
        calibrated += artifact.platt_intercept_ppm
        probability = deterministic_sigmoid_ppm(calibrated)
        return NativePrediction(
            probability - (PPM // 2), probability, PPM - probability, ood_score
        )

    cost = max(0, min(PPM, score))
    directional = deterministic_sigmoid_ppm(-cost)
    probability_up = (directional * 900_000) // PPM
    probability_down = 900_000 - probability_up
    if artifact.spec.role == "transaction_cost":
        shares = (100_000, 300_000, 200_000, 250_000, 150_000)
        components = tuple((cost * share) // PPM for share in shares)
        return NativePrediction(
            -cost,
            probability_up,
            probability_down,
            ood_score,
            components[0],
            components[1],
            components[2],
            components[3],
            components[4],
        )
    return NativePrediction(
        -cost,
        probability_up,
        probability_down,
        ood_score,
        market_impact_ppm=cost,
    )


def encode_artifact(artifact: ModelArtifact) -> bytes:
    """Encode the canonical LF-delimited native model and append its SHA-256."""
    is_cost = artifact.spec.role == "transaction_cost"
    body_lines = [
        "AEGIS_MX_NATIVE_MODEL_V1",
        f"role={artifact.spec.role}",
        f"family={artifact.spec.family}",
        f"model_id={100 + artifact.ordinal}:{artifact.ordinal}",
        f"model_version={200 + artifact.ordinal}:1",
        "instrument_id=1:1",
        "configuration_version=300:1",
        "feature_definition_version=11:11",
        f"horizon_ns={artifact.horizon_ns}",
        f"forecast_ttl_ns={artifact.forecast_ttl_ns}",
        f"expires_wall_clock_utc_ns={artifact.expires_wall_clock_utc_ns}",
        f"training_sample_count={artifact.training_sample_count}",
        f"calibration_error_ppm={artifact.calibration_error_ppm}",
        f"ood_reject_threshold_ppm={artifact.ood_reject_threshold_ppm}",
        f"intercept_ppm={artifact.intercept_ppm}",
        f"platt_slope_ppm={artifact.platt_slope_ppm}",
        f"platt_intercept_ppm={artifact.platt_intercept_ppm}",
        f"fee_share_ppm={100_000 if is_cost else 0}",
        f"spread_share_ppm={300_000 if is_cost else 0}",
        f"slippage_share_ppm={200_000 if is_cost else 0}",
        f"impact_share_ppm={250_000 if is_cost else 0}",
        f"adverse_selection_share_ppm={150_000 if is_cost else 0}",
        f"feature_count={len(artifact.transforms)}",
    ]
    body_lines.extend(
        "feature="
        f"{transform.name},{transform.center},{transform.scale},"
        f"{transform.training_minimum},{transform.training_maximum},"
        f"{transform.weight_ppm}"
        for transform in artifact.transforms
    )
    body = ("\n".join(body_lines) + "\n").encode("ascii")
    signature = hashlib.sha256(body).hexdigest().encode("ascii")
    return body + b"signature_sha256=" + signature + b"\n"


def train_suite(
    dataset: Dataset, config: TrainingConfig | None = None
) -> tuple[tuple[ModelArtifact, TrainingReport], ...]:
    """Train all seven roles in a stable declared order."""
    resolved_config = config if config is not None else TrainingConfig()
    _validate_config(resolved_config)
    trained: list[tuple[ModelArtifact, TrainingReport]] = []
    for ordinal, spec in enumerate(MODEL_SPECS, start=1):
        artifact, initial_loss, final_loss = _train_one(
            dataset, spec, resolved_config, ordinal
        )
        encoded = encode_artifact(artifact)
        signature = encoded.rsplit(b"signature_sha256=", maxsplit=1)[1].strip()
        report = TrainingReport(
            role=spec.role,
            seed=resolved_config.seed,
            sample_count=len(dataset.rows),
            initial_loss_ppm=initial_loss,
            final_loss_ppm=final_loss,
            artifact_sha256=signature.decode("ascii"),
        )
        trained.append((artifact, report))
    return tuple(trained)


def export_suite(
    dataset: Dataset, output_directory: Path, config: TrainingConfig | None = None
) -> tuple[TrainingReport, ...]:
    """Atomically replace each role artifact after deterministic training."""
    output_directory.mkdir(parents=True, exist_ok=True)
    reports: list[TrainingReport] = []
    for artifact, report in train_suite(dataset, config):
        destination = output_directory / f"{artifact.spec.role}.amdl"
        temporary = destination.with_suffix(".amdl.tmp")
        temporary.write_bytes(encode_artifact(artifact))
        temporary.replace(destination)
        reports.append(report)
    return tuple(reports)
