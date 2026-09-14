"""Fail-closed training admission for bounded forecasting POC datasets."""

from __future__ import annotations

import hashlib
import json
import os
import stat
import subprocess
from collections import Counter
from dataclasses import dataclass
from datetime import UTC, datetime
from enum import StrEnum
from pathlib import Path
from typing import TYPE_CHECKING, Final, cast

import pyarrow as pa  # type: ignore[import-untyped]
import pyarrow.compute as pc  # type: ignore[import-untyped]
import pyarrow.parquet as pq  # type: ignore[import-untyped]

from aegis_mx_research.data_repository import (
    DataRepository,
    QuotaEvidence,
    StorageRequest,
)
from aegis_mx_research.feature_dataset import FEATURE_NAMES
from aegis_mx_research.forecast_contracts import (
    REQUIRED_HORIZON_LABELS,
    UnresolvedInstrumentResolver,
    parse_ticker_universe,
)

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence

TRAINING_READINESS_SCHEMA_VERSION: Final = "1.0.0"
MIN_VALID_LABELS_PER_SYMBOL_HORIZON: Final = 100
MIN_POOLED_TRAINING_ROWS_PER_HORIZON: Final = 100_000
MIN_POOLED_TRAINING_SYMBOLS_PER_HORIZON: Final = 50
TRAINING_SCAN_BATCH_ROWS: Final = 32_768
MAX_DOCUMENT_BYTES: Final = 16_000_000
MAX_DATASET_OBJECTS: Final = 40_000
MAX_APPROVAL_ID_BYTES: Final = 256
SHA256_HEX_LENGTH: Final = 64
GIT_COMMIT_HEX_LENGTH: Final = 40
GIT_EXECUTABLE: Final = "/usr/bin/git"
EMPTY_SNAPSHOT_SHA256: Final = hashlib.sha256(b"[]").hexdigest()

REQUIRED_TRAINING_FEATURES: Final = (
    "return_1m_ppm",
    "return_5m_ppm",
    "return_15m_ppm",
    "return_30m_ppm",
    "return_60m_ppm",
    "volume_shares",
    "relative_volume_20m_ppm",
    "realized_volatility_30m_ppm",
    "high_low_range_ppm",
    "intraday_gap_ppm",
    "minute_of_session",
    "session_position_ppm",
    "market_return_1m_ppm",
    "market_relative_return_1m_ppm",
    "rolling_session_return_5d_ppm",
    "rolling_session_volume_5d_shares",
)

EXTERNAL_FEATURES: Final = {
    "sector_return_1m_ppm": "sector",
    "sector_relative_return_1m_ppm": "sector",
    "news_event_flag": "event",
    "macro_event_flag": "event",
}

ALLOWED_DEGRADED_REASONS: Final = (
    "HALT_HISTORY_INCOMPLETE",
    "HISTORICAL_PROVIDER_PUBLICATION_TIME_UNOBSERVED",
    "NOW_KNOWN_CURRENT_UNIVERSE_MAPPING",
    "PRICE_QUANTUM_IS_NOT_VENUE_TICK",
    "SECTOR_AGGREGATE_UNAVAILABLE",
    "SOURCE_PUBLICATION_TIME_UNAVAILABLE",
)

DEFAULT_BUILDER_SOURCES: Final = (
    "python/research/aegis_mx_research/canonical_minute.py",
    "python/research/aegis_mx_research/feature_dataset.py",
    "python/research/aegis_mx_research/forecast_contracts.py",
    "python/research/aegis_mx_research/real_minute_pipeline.py",
    "python/research/aegis_mx_research/reference_data.py",
    "python/research/aegis_mx_research/training_readiness.py",
    "tools/build_real_feature_dataset.py",
    "tools/check_training_readiness.py",
    "schemas/canonical-minute-record-v1.schema.json",
    "schemas/feature-label-row-v1.schema.json",
    "schemas/feature-dataset-manifest-v1.schema.json",
    "schemas/training-readiness-report-v1.schema.json",
    "python/requirements-dev.lock",
)


class TrainingReadinessCode(StrEnum):
    """Stable training-admission failure categories."""

    CORRUPT_INPUT = "CORRUPT_INPUT"
    DIRTY_WORKTREE = "DIRTY_WORKTREE"
    INSUFFICIENT_COVERAGE = "INSUFFICIENT_COVERAGE"
    INVALID_CONFIGURATION = "INVALID_CONFIGURATION"
    LEAKAGE_REJECTED = "LEAKAGE_REJECTED"
    MISSING_CORE_FEATURE = "MISSING_CORE_FEATURE"
    PROVENANCE_MISMATCH = "PROVENANCE_MISMATCH"
    SOURCE_AUTHORIZATION_MISMATCH = "SOURCE_AUTHORIZATION_MISMATCH"


class TrainingReadinessError(RuntimeError):
    """Raised when an assessment cannot safely be constructed or published."""

    def __init__(self, code: TrainingReadinessCode, message: str) -> None:
        """Retain the stable rejection code with a bounded explanation."""
        super().__init__(message)
        self.code = code


@dataclass(frozen=True)
class GitProvenance:
    """Content and repository identity used by the training gate."""

    commit: str
    worktree_dirty: bool
    worktree_status_sha256: str
    source_files: tuple[tuple[str, str], ...]
    source_bundle_sha256: str
    dependency_lock_sha256: str

    def document(self) -> dict[str, object]:
        """Return a canonical JSON-compatible representation."""
        return {
            "dependency_lock_sha256": self.dependency_lock_sha256,
            "git_commit": self.commit,
            "source_bundle_sha256": self.source_bundle_sha256,
            "source_files": [
                {"path": path, "sha256": digest} for path, digest in self.source_files
            ],
            "worktree_dirty": self.worktree_dirty,
            "worktree_status_sha256": self.worktree_status_sha256,
        }


def _canonical_bytes(value: object) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("ascii")


def _digest(value: object) -> str:
    return hashlib.sha256(_canonical_bytes(value)).hexdigest()


def _is_sha256(value: object) -> bool:
    return (
        isinstance(value, str)
        and len(value) == SHA256_HEX_LENGTH
        and all(character in "0123456789abcdef" for character in value)
    )


def _hash_file(path: Path) -> str:
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
        with os.fdopen(descriptor, "rb", closefd=True) as source_file:
            status = os.fstat(source_file.fileno())
            if not stat.S_ISREG(status.st_mode) or status.st_size <= 0:
                raise TrainingReadinessError(
                    TrainingReadinessCode.CORRUPT_INPUT,
                    f"required file is not a nonempty regular file: {path}",
                )
            return hashlib.file_digest(source_file, "sha256").hexdigest()
    except OSError as error:
        raise TrainingReadinessError(
            TrainingReadinessCode.CORRUPT_INPUT,
            f"cannot authenticate required file: {path}",
        ) from error


def _read_document(path: Path, label: str) -> dict[str, object]:
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
        with os.fdopen(descriptor, "rb", closefd=True) as source_file:
            status = os.fstat(source_file.fileno())
            payload = source_file.read(MAX_DOCUMENT_BYTES + 1)
        if (
            not stat.S_ISREG(status.st_mode)
            or status.st_size <= 0
            or status.st_size > MAX_DOCUMENT_BYTES
            or len(payload) != status.st_size
        ):
            raise TrainingReadinessError(
                TrainingReadinessCode.CORRUPT_INPUT,
                f"{label} is missing, malformed, or unsafe",
            )
        value = json.loads(payload)
    except (OSError, json.JSONDecodeError) as error:
        raise TrainingReadinessError(
            TrainingReadinessCode.CORRUPT_INPUT,
            f"{label} is missing, malformed, or unsafe",
        ) from error
    if not isinstance(value, dict):
        raise TrainingReadinessError(
            TrainingReadinessCode.CORRUPT_INPUT,
            f"{label} must be a JSON object",
        )
    return cast("dict[str, object]", value)


def _verify_self_hash(document: Mapping[str, object], field: str, label: str) -> str:
    supplied = document.get(field)
    if not _is_sha256(supplied):
        raise TrainingReadinessError(
            TrainingReadinessCode.CORRUPT_INPUT, f"{label} hash is malformed"
        )
    body = dict(document)
    body.pop(field)
    if supplied != _digest(body):
        raise TrainingReadinessError(
            TrainingReadinessCode.CORRUPT_INPUT, f"{label} self-hash mismatch"
        )
    return supplied


def _run_git(repo_root: Path, arguments: Sequence[str]) -> bytes:
    try:
        result = subprocess.run(  # noqa: S603
            [GIT_EXECUTABLE, "-C", str(repo_root), *arguments],
            check=True,
            capture_output=True,
            timeout=10,
        )
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        raise TrainingReadinessError(
            TrainingReadinessCode.PROVENANCE_MISMATCH,
            "Git provenance cannot be established",
        ) from error
    return result.stdout


def capture_git_provenance(
    repo_root: Path,
    source_paths: Sequence[str] = DEFAULT_BUILDER_SOURCES,
) -> GitProvenance:
    """Capture a bounded exact source identity and fail on unsafe paths."""
    try:
        root = repo_root.resolve(strict=True)
    except OSError as error:
        raise TrainingReadinessError(
            TrainingReadinessCode.PROVENANCE_MISMATCH,
            "repository root cannot be authenticated",
        ) from error
    commit = _run_git(root, ("rev-parse", "HEAD")).decode("ascii").strip()
    if len(commit) != GIT_COMMIT_HEX_LENGTH or any(
        character not in "0123456789abcdef" for character in commit
    ):
        raise TrainingReadinessError(
            TrainingReadinessCode.PROVENANCE_MISMATCH, "Git commit is malformed"
        )
    status = _run_git(root, ("status", "--porcelain=v1", "--untracked-files=all"))
    files: list[tuple[str, str]] = []
    for relative_text in source_paths:
        relative = Path(relative_text)
        if (
            relative.is_absolute()
            or not relative.parts
            or any(part in {"", ".", ".."} for part in relative.parts)
        ):
            raise TrainingReadinessError(
                TrainingReadinessCode.INVALID_CONFIGURATION,
                "builder source path is unsafe",
            )
        try:
            candidate = (root / relative).resolve(strict=True)
        except OSError as error:
            raise TrainingReadinessError(
                TrainingReadinessCode.CORRUPT_INPUT,
                "builder source file is missing",
            ) from error
        if not candidate.is_relative_to(root):
            raise TrainingReadinessError(
                TrainingReadinessCode.INVALID_CONFIGURATION,
                "builder source escapes repository root",
            )
        files.append((relative.as_posix(), _hash_file(candidate)))
    files.sort()
    lock = dict(files).get("python/requirements-dev.lock")
    if lock is None:
        raise TrainingReadinessError(
            TrainingReadinessCode.PROVENANCE_MISMATCH,
            "dependency lock is not part of the source bundle",
        )
    return GitProvenance(
        commit,
        bool(status),
        hashlib.sha256(status).hexdigest(),
        tuple(files),
        _digest([{"path": path, "sha256": digest} for path, digest in files]),
        lock,
    )


def _verify_dataset_objects(  # noqa: C901
    data_root: Path, objects: Sequence[Mapping[str, object]]
) -> tuple[int, int, int]:
    if not 0 < len(objects) <= MAX_DATASET_OBJECTS:
        raise TrainingReadinessError(
            TrainingReadinessCode.CORRUPT_INPUT,
            "dataset object list is empty or out of bounds",
        )
    try:
        root = data_root.resolve(strict=True)
    except OSError as error:
        raise TrainingReadinessError(
            TrainingReadinessCode.CORRUPT_INPUT,
            "data root cannot be authenticated",
        ) from error
    checked_bytes = 0
    feature_records = 0
    summary_records = 0
    seen_paths: set[str] = set()
    for item in objects:
        relative_text = item.get("storage_path")
        expected_hash = item.get("object_sha256")
        expected_size = item.get("size_bytes_decimal")
        record_count = item.get("record_count")
        kind = item.get("kind")
        if (
            not isinstance(relative_text, str)
            or not isinstance(expected_hash, str)
            or not isinstance(expected_size, int)
            or not isinstance(record_count, int)
            or expected_size <= 0
            or record_count <= 0
            or relative_text in seen_paths
        ):
            raise TrainingReadinessError(
                TrainingReadinessCode.CORRUPT_INPUT,
                "dataset object metadata is malformed or duplicated",
            )
        relative = Path(relative_text)
        if (
            relative.is_absolute()
            or not relative.parts
            or relative.parts[0] not in {"datasets", "derived"}
            or any(part in {"", ".", ".."} for part in relative.parts)
        ):
            raise TrainingReadinessError(
                TrainingReadinessCode.CORRUPT_INPUT,
                "dataset object path is unsafe",
            )
        unresolved_candidate = root / relative
        try:
            candidate = unresolved_candidate.resolve(strict=True)
        except OSError as error:
            raise TrainingReadinessError(
                TrainingReadinessCode.CORRUPT_INPUT,
                "dataset object is missing",
            ) from error
        if unresolved_candidate.is_symlink() or not candidate.is_relative_to(root):
            raise TrainingReadinessError(
                TrainingReadinessCode.CORRUPT_INPUT,
                "dataset object escapes the data root",
            )
        if (
            unresolved_candidate.stat().st_size != expected_size
            or _hash_file(unresolved_candidate) != expected_hash
        ):
            raise TrainingReadinessError(
                TrainingReadinessCode.CORRUPT_INPUT,
                "dataset object size or SHA-256 mismatch",
            )
        seen_paths.add(relative_text)
        checked_bytes += expected_size
        if kind == "FEATURE_LABEL":
            feature_records += record_count
        elif kind == "SESSION_SUMMARY":
            summary_records += record_count
        else:
            raise TrainingReadinessError(
                TrainingReadinessCode.CORRUPT_INPUT,
                "dataset object kind is unsupported",
            )
    return checked_bytes, feature_records, summary_records


def _feature_profile(  # noqa: C901
    identity: Mapping[str, object],
) -> tuple[list[str], list[dict[str, object]]]:
    raw_stats = identity.get("normalization")
    if not isinstance(raw_stats, list) or len(raw_stats) != len(FEATURE_NAMES):
        raise TrainingReadinessError(
            TrainingReadinessCode.CORRUPT_INPUT,
            "training normalization metadata is malformed",
        )
    stats: dict[str, Mapping[str, object]] = {}
    for raw in raw_stats:
        if not isinstance(raw, dict) or not isinstance(raw.get("feature_name"), str):
            raise TrainingReadinessError(
                TrainingReadinessCode.CORRUPT_INPUT,
                "training normalization entry is malformed",
            )
        stats[cast("str", raw["feature_name"])] = raw
    if tuple(stats) != FEATURE_NAMES:
        raise TrainingReadinessError(
            TrainingReadinessCode.CORRUPT_INPUT,
            "training feature order does not match the canonical contract",
        )
    sector_empty = identity.get("sector_snapshot_sha256") == EMPTY_SNAPSHOT_SHA256
    event_empty = identity.get("event_snapshot_sha256") == EMPTY_SNAPSHOT_SHA256
    selected: list[str] = []
    excluded: list[dict[str, object]] = []
    for feature in FEATURE_NAMES:
        stat = stats[feature]
        count = stat.get("count")
        total = stat.get("sum")
        squares = stat.get("sum_squares")
        if not all(isinstance(value, int) for value in (count, total, squares)):
            raise TrainingReadinessError(
                TrainingReadinessCode.CORRUPT_INPUT,
                "training normalization values are malformed",
            )
        reasons: list[str] = []
        source = EXTERNAL_FEATURES.get(feature)
        if source == "sector" and sector_empty:
            reasons.append("SECTOR_SNAPSHOT_UNAVAILABLE")
        if source == "event" and event_empty:
            reasons.append("EVENT_SNAPSHOT_UNAVAILABLE")
        typed_count = cast("int", count)
        typed_total = cast("int", total)
        typed_squares = cast("int", squares)
        if typed_count == 0:
            reasons.append("NO_TRAIN_OBSERVATIONS")
        elif typed_count * typed_squares - typed_total * typed_total <= 0:
            reasons.append("ZERO_TRAIN_VARIANCE")
        if reasons:
            excluded.append({"feature_name": feature, "reason_codes": sorted(reasons)})
        else:
            selected.append(feature)
    return selected, excluded


def _coverage_profile(
    coverage: object,
    ticker_symbols: Sequence[str],
    minimum_valid: int,
) -> tuple[dict[str, object], list[str]]:
    if not isinstance(coverage, list):
        raise TrainingReadinessError(
            TrainingReadinessCode.CORRUPT_INPUT, "coverage is not an array"
        )
    expected = {
        (symbol, horizon)
        for symbol in ticker_symbols
        for horizon in REQUIRED_HORIZON_LABELS
    }
    actual: set[tuple[str, str]] = set()
    horizon_valid: Counter[str] = Counter()
    horizon_minimum: dict[str, int] = {}
    eligible_symbols: set[str] = set()
    abstaining_symbols: set[str] = set()
    invalid_labels = 0
    missing_labels = 0
    valid_labels = 0
    blockers: list[str] = []
    for raw in coverage:
        if not isinstance(raw, dict):
            raise TrainingReadinessError(
                TrainingReadinessCode.CORRUPT_INPUT,
                "coverage entry is malformed",
            )
        symbol = raw.get("symbol")
        horizon = raw.get("horizon")
        valid = raw.get("valid")
        missing = raw.get("missing")
        invalid = raw.get("invalid")
        resolution = raw.get("resolution_code")
        samples = raw.get("samples")
        if (
            not isinstance(symbol, str)
            or not isinstance(horizon, str)
            or not all(
                isinstance(value, int) for value in (valid, missing, invalid, samples)
            )
        ):
            raise TrainingReadinessError(
                TrainingReadinessCode.CORRUPT_INPUT,
                "coverage values are malformed",
            )
        pair = (symbol, horizon)
        if pair in actual:
            raise TrainingReadinessError(
                TrainingReadinessCode.CORRUPT_INPUT,
                "coverage entry is duplicated",
            )
        actual.add(pair)
        typed_valid = cast("int", valid)
        valid_labels += typed_valid
        missing_labels += cast("int", missing)
        invalid_labels += cast("int", invalid)
        if resolution == "RESOLVED":
            eligible_symbols.add(symbol)
            horizon_valid[horizon] += typed_valid
            horizon_minimum[horizon] = min(
                horizon_minimum.get(horizon, typed_valid), typed_valid
            )
            if typed_valid < minimum_valid:
                blockers.append(f"INSUFFICIENT_LABELS:{symbol}:{horizon}")
        else:
            abstaining_symbols.add(symbol)
            if typed_valid != 0 or cast("int", samples) != 0:
                blockers.append(f"UNRESOLVED_HAS_SAMPLES:{symbol}:{horizon}")
    if actual != expected:
        blockers.append("INCOMPLETE_TICKER_HORIZON_MATRIX")
    return (
        {
            "abstaining_symbols": sorted(abstaining_symbols),
            "coverage_rows": len(coverage),
            "eligible_symbols": sorted(eligible_symbols),
            "invalid_labels": invalid_labels,
            "minimum_valid_labels_per_resolved_symbol_horizon": minimum_valid,
            "missing_labels": missing_labels,
            "valid_labels": valid_labels,
            "per_horizon": [
                {
                    "horizon": horizon,
                    "minimum_valid_per_resolved_symbol": horizon_minimum.get(
                        horizon, 0
                    ),
                    "valid": horizon_valid[horizon],
                }
                for horizon in REQUIRED_HORIZON_LABELS
            ],
        },
        blockers,
    )


def _true_count(mask: object) -> int:
    value = pc.sum(pc.cast(mask, pa.int64())).as_py()
    return cast("int", value or 0)


def _model_ready_profile(  # noqa: C901, PLR0912, PLR0915
    data_root: Path,
    objects: Sequence[Mapping[str, object]],
    selected_features: Sequence[str],
    eligible_symbols: Sequence[str],
) -> tuple[dict[str, object], list[str]]:
    """Stream feature objects and count complete label-feature intersections."""
    feature_indices = tuple(FEATURE_NAMES.index(name) for name in selected_features)
    if not feature_indices:
        raise TrainingReadinessError(
            TrainingReadinessCode.MISSING_CORE_FEATURE,
            "model-ready scan requires at least one selected feature",
        )
    counts: Counter[tuple[str, str, str]] = Counter()
    feature_complete: Counter[str] = Counter()
    observed_reasons: set[str] = set()
    scanned_rows = 0
    horizon_count = len(REQUIRED_HORIZON_LABELS)
    for item in objects:
        if item.get("kind") != "FEATURE_LABEL":
            continue
        relative_text = item.get("storage_path")
        expected_records = item.get("record_count")
        expected_split = item.get("split")
        if (
            not isinstance(relative_text, str)
            or not isinstance(expected_records, int)
            or expected_split not in {"TRAIN", "VALIDATION", "TEST"}
        ):
            raise TrainingReadinessError(
                TrainingReadinessCode.CORRUPT_INPUT,
                "feature object scan metadata is malformed",
            )
        path = data_root / relative_text
        object_rows = 0
        try:
            parquet = pq.ParquetFile(path)
            for batch in parquet.iter_batches(
                batch_size=TRAINING_SCAN_BATCH_ROWS,
                columns=(
                    "source_ticker",
                    "split",
                    "normalized_features_ppm",
                    "feature_reason_codes",
                    "labels",
                ),
            ):
                if batch.num_rows == 0:
                    continue
                object_rows += batch.num_rows
                tickers = set(cast("list[str]", pc.unique(batch.column(0)).to_pylist()))
                splits = set(cast("list[str]", pc.unique(batch.column(1)).to_pylist()))
                if len(tickers) != 1 or splits != {expected_split}:
                    raise TrainingReadinessError(
                        TrainingReadinessCode.CORRUPT_INPUT,
                        "feature object mixes ticker or split identity",
                    )
                ticker = next(iter(tickers))
                normalized = batch.column(2)
                complete_mask = pc.is_valid(
                    pc.list_element(normalized, feature_indices[0])
                )
                for feature_index in feature_indices[1:]:
                    complete_mask = pc.and_(
                        complete_mask,
                        pc.is_valid(pc.list_element(normalized, feature_index)),
                    )
                complete_count = _true_count(complete_mask)
                feature_complete[expected_split] += complete_count
                complete_reasons = pc.filter(batch.column(3), complete_mask)
                observed_reasons.update(
                    reason
                    for reason in cast(
                        "list[str | None]",
                        pc.unique(pc.list_flatten(complete_reasons)).to_pylist(),
                    )
                    if reason is not None
                )
                flattened_labels = pc.list_flatten(batch.column(4))
                if len(flattened_labels) != batch.num_rows * horizon_count:
                    raise TrainingReadinessError(
                        TrainingReadinessCode.CORRUPT_INPUT,
                        "feature row label cardinality is incompatible",
                    )
                label_horizons = flattened_labels.field("horizon")
                label_validity = flattened_labels.field("validity")
                for horizon_index, horizon in enumerate(REQUIRED_HORIZON_LABELS):
                    positions = pa.array(
                        range(
                            horizon_index,
                            len(flattened_labels),
                            horizon_count,
                        ),
                        type=pa.int64(),
                    )
                    observed_horizons = set(
                        cast(
                            "list[str]",
                            pc.unique(pc.take(label_horizons, positions)).to_pylist(),
                        )
                    )
                    if observed_horizons != {horizon}:
                        raise TrainingReadinessError(
                            TrainingReadinessCode.CORRUPT_INPUT,
                            "feature row label order is incompatible",
                        )
                    valid_mask = pc.equal(
                        pc.take(label_validity, positions),
                        "VALID",
                    )
                    counts[(ticker, horizon, expected_split)] += _true_count(
                        pc.and_(complete_mask, valid_mask)
                    )
        except TrainingReadinessError:
            raise
        except (OSError, pa.ArrowException) as error:
            raise TrainingReadinessError(
                TrainingReadinessCode.CORRUPT_INPUT,
                "feature object cannot be scanned safely",
            ) from error
        if object_rows != expected_records:
            raise TrainingReadinessError(
                TrainingReadinessCode.CORRUPT_INPUT,
                "feature object scan record count mismatch",
            )
        scanned_rows += object_rows

    blockers: list[str] = []
    unknown_reasons = sorted(observed_reasons - set(ALLOWED_DEGRADED_REASONS))
    if unknown_reasons:
        blockers.append("UNAPPROVED_DATA_QUALITY_REASON")
    per_horizon: list[dict[str, object]] = []
    for horizon in REQUIRED_HORIZON_LABELS:
        training_by_symbol = {
            symbol: counts[(symbol, horizon, "TRAIN")] for symbol in eligible_symbols
        }
        validation_rows = sum(
            counts[(symbol, horizon, "VALIDATION")] for symbol in eligible_symbols
        )
        test_rows = sum(
            counts[(symbol, horizon, "TEST")] for symbol in eligible_symbols
        )
        training_rows = sum(training_by_symbol.values())
        contributors = sorted(
            symbol for symbol, count in training_by_symbol.items() if count > 0
        )
        noncontributors = sorted(set(eligible_symbols) - set(contributors))
        if training_rows < MIN_POOLED_TRAINING_ROWS_PER_HORIZON:
            blockers.append(f"INSUFFICIENT_POOLED_TRAINING_ROWS:{horizon}")
        if len(contributors) < MIN_POOLED_TRAINING_SYMBOLS_PER_HORIZON:
            blockers.append(f"INSUFFICIENT_POOLED_TRAINING_SYMBOLS:{horizon}")
        if validation_rows == 0:
            blockers.append(f"MISSING_MODEL_READY_VALIDATION:{horizon}")
        if test_rows == 0:
            blockers.append(f"MISSING_MODEL_READY_TEST:{horizon}")
        per_horizon.append(
            {
                "contributing_training_symbols": len(contributors),
                "horizon": horizon,
                "noncontributing_training_symbols": noncontributors,
                "test_rows": test_rows,
                "training_rows": training_rows,
                "validation_rows": validation_rows,
            }
        )
    return (
        {
            "feature_complete_rows_by_split": {
                split: feature_complete[split]
                for split in ("TRAIN", "VALIDATION", "TEST")
            },
            "minimum_pooled_training_rows_per_horizon": (
                MIN_POOLED_TRAINING_ROWS_PER_HORIZON
            ),
            "minimum_pooled_training_symbols_per_horizon": (
                MIN_POOLED_TRAINING_SYMBOLS_PER_HORIZON
            ),
            "observed_quality_reason_codes": sorted(observed_reasons),
            "per_horizon": per_horizon,
            "scanned_feature_rows": scanned_rows,
        },
        blockers,
    )


def assess_training_readiness(  # noqa: C901, PLR0912, PLR0913, PLR0915
    *,
    data_root: Path,
    dataset_manifest_path: Path,
    backfill_report_path: Path,
    promotion_report_path: Path,
    source_approval_path: Path,
    ticker_path: Path,
    provenance: GitProvenance,
    assessment_time_utc: datetime | None = None,
    verify_objects: bool = True,
    minimum_valid: int = MIN_VALID_LABELS_PER_SYMBOL_HORIZON,
) -> dict[str, object]:
    """Authenticate inputs and create a deterministic training admission report."""
    if minimum_valid <= 0:
        raise TrainingReadinessError(
            TrainingReadinessCode.INVALID_CONFIGURATION,
            "minimum valid label threshold must be positive",
        )
    manifest = _read_document(dataset_manifest_path, "feature dataset manifest")
    backfill = _read_document(backfill_report_path, "backfill report")
    promotion = _read_document(promotion_report_path, "canonical promotion report")
    source_approval = _read_document(source_approval_path, "source approval")
    manifest_sha = _verify_self_hash(manifest, "manifest_sha256", "dataset manifest")
    backfill_sha = _verify_self_hash(backfill, "document_sha256", "backfill report")
    promotion_sha = _verify_self_hash(
        promotion, "document_sha256", "canonical promotion report"
    )
    source_approval_sha = _verify_self_hash(
        source_approval, "document_sha256", "source approval"
    )
    try:
        ticker_source = ticker_path.read_bytes()
    except OSError as error:
        raise TrainingReadinessError(
            TrainingReadinessCode.CORRUPT_INPUT,
            "ticker universe cannot be read",
        ) from error
    universe = parse_ticker_universe(ticker_source, UnresolvedInstrumentResolver())
    symbols = tuple(entry.symbol for entry in universe.ordered_entries)
    try:
        resolved_data_root = data_root.resolve(strict=True)
    except OSError as error:
        raise TrainingReadinessError(
            TrainingReadinessCode.CORRUPT_INPUT,
            "data root cannot be authenticated",
        ) from error
    checked_at = (
        datetime.now(UTC) if assessment_time_utc is None else assessment_time_utc
    )
    if checked_at.tzinfo is None or checked_at.utcoffset() != UTC.utcoffset(checked_at):
        raise TrainingReadinessError(
            TrainingReadinessCode.INVALID_CONFIGURATION,
            "assessment time must be timezone-aware UTC",
        )
    checked_at = checked_at.astimezone(UTC)
    blockers: list[str] = []
    warnings: list[str] = []
    if provenance.worktree_dirty:
        blockers.append(TrainingReadinessCode.DIRTY_WORKTREE.value)
    leakage = manifest.get("leakage")
    if not isinstance(leakage, dict) or leakage.get("status") != "PASS":
        blockers.append(TrainingReadinessCode.LEAKAGE_REJECTED.value)
    identity = manifest.get("dataset_identity")
    if not isinstance(identity, dict) or manifest.get("dataset_id") != _digest(
        identity
    ):
        raise TrainingReadinessError(
            TrainingReadinessCode.CORRUPT_INPUT,
            "dataset identity is malformed or does not match dataset_id",
        )
    if (
        tuple(identity.get("feature_names", ())) != FEATURE_NAMES
        or tuple(identity.get("horizons", ())) != REQUIRED_HORIZON_LABELS
    ):
        raise TrainingReadinessError(
            TrainingReadinessCode.CORRUPT_INPUT,
            "feature or horizon contract is incompatible",
        )
    if (
        backfill.get("ticker_source_sha256") != universe.source_file_sha256.hex()
        or promotion.get("backfill_report_sha256") != backfill_sha
    ):
        blockers.append(TrainingReadinessCode.PROVENANCE_MISMATCH.value)
    approval_sha = backfill.get("approval_record_sha256")
    source_policy_sha = backfill.get("source_policy_sha256")
    if (
        not _is_sha256(approval_sha)
        or not _is_sha256(source_policy_sha)
        or backfill.get("feed") != "iex"
        or backfill.get("timeframe") != "1Min"
        or backfill.get("live_trading_capable") is not False
    ):
        blockers.append(TrainingReadinessCode.SOURCE_AUTHORIZATION_MISMATCH.value)
    approval_expiry_text = source_approval.get("expires_at_utc")
    try:
        approval_expiry = datetime.fromisoformat(cast("str", approval_expiry_text))
    except (AttributeError, TypeError, ValueError):
        approval_expiry = datetime.min.replace(tzinfo=UTC)
    approval_expiry_is_utc = (
        approval_expiry.tzinfo is not None
        and approval_expiry.utcoffset() == UTC.utcoffset(approval_expiry)
    )
    normalized_approval_expiry = (
        approval_expiry.astimezone(UTC).isoformat().replace("+00:00", "Z")
        if approval_expiry_is_utc
        else (
            approval_expiry.isoformat() if approval_expiry.tzinfo is not None else None
        )
    )
    approval_id = source_approval.get("approval_id")
    approval_id_valid = (
        isinstance(approval_id, str)
        and 0 < len(approval_id.encode("utf-8")) <= MAX_APPROVAL_ID_BYTES
    )
    approval_root_text = source_approval.get("data_root")
    approval_root_matches = False
    if isinstance(approval_root_text, str) and Path(approval_root_text).is_absolute():
        try:
            approval_root_matches = (
                Path(approval_root_text).resolve(strict=True) == resolved_data_root
            )
        except OSError:
            approval_root_matches = False
    authorization_valid = (
        approval_id_valid
        and source_approval_sha == approval_sha
        and source_approval.get("decision") == "APPROVED"
        and source_approval.get("model_training_authorized") is True
        and source_approval.get("live_trading_capable") is False
        and source_approval.get("source_id") == "alpaca_iex_historical_bars"
        and source_approval.get("feed") == "iex"
        and source_approval.get("timeframe") == "1Min"
        and source_approval.get("purpose") == "INTERNAL_ACADEMIC_NONCOMMERCIAL_RESEARCH"
        and source_approval.get("distribution") == "INTERNAL_SINGLE_USER_ONLY"
        and approval_root_matches
        and source_approval.get("ticker_source_sha256")
        == universe.source_file_sha256.hex()
        and approval_expiry_is_utc
        and approval_expiry > checked_at
    )
    if not authorization_valid:
        blockers.append(TrainingReadinessCode.SOURCE_AUTHORIZATION_MISMATCH.value)
    source_partitions = identity.get("source_partition_ids")
    promoted_partitions = promotion.get("partition_manifest_ids")
    lineage_valid = (
        isinstance(source_partitions, list)
        and isinstance(promoted_partitions, list)
        and all(isinstance(value, str) for value in source_partitions)
        and all(isinstance(value, str) for value in promoted_partitions)
        and len(source_partitions) == len(set(source_partitions))
        and len(promoted_partitions) == len(set(promoted_partitions))
        and set(source_partitions) == set(promoted_partitions)
    )
    if not lineage_valid:
        blockers.append("CANONICAL_PARTITION_LINEAGE_MISMATCH")
    objects = manifest.get("objects")
    if not isinstance(objects, list):
        raise TrainingReadinessError(
            TrainingReadinessCode.CORRUPT_INPUT, "dataset objects are malformed"
        )
    if verify_objects:
        checked_bytes, feature_records, summary_records = _verify_dataset_objects(
            data_root, cast("Sequence[Mapping[str, object]]", objects)
        )
    else:
        blockers.append("DATASET_OBJECT_VERIFICATION_SKIPPED")
        checked_bytes = 0
        feature_records = cast("int", manifest.get("sample_count", -1))
        summary_records = cast("int", manifest.get("session_summary_count", -1))
    if feature_records != manifest.get(
        "sample_count"
    ) or summary_records != manifest.get("session_summary_count"):
        blockers.append("DATASET_OBJECT_RECORD_COUNT_MISMATCH")
    selected, excluded = _feature_profile(identity)
    absent_core = sorted(set(REQUIRED_TRAINING_FEATURES) - set(selected))
    if absent_core:
        blockers.append(TrainingReadinessCode.MISSING_CORE_FEATURE.value)
    coverage, coverage_blockers = _coverage_profile(
        manifest.get("coverage"), symbols, minimum_valid
    )
    blockers.extend(coverage_blockers)
    if verify_objects:
        model_ready, model_ready_blockers = _model_ready_profile(
            data_root,
            cast("Sequence[Mapping[str, object]]", objects),
            selected,
            cast("Sequence[str]", coverage["eligible_symbols"]),
        )
        blockers.extend(model_ready_blockers)
    else:
        blockers.append("TRAINING_ROW_VERIFICATION_SKIPPED")
        model_ready = None
    missing_pairs = backfill.get("missing_symbol_session_pairs")
    if not isinstance(missing_pairs, list) or not all(
        isinstance(value, str) and ":" in value for value in missing_pairs
    ):
        raise TrainingReadinessError(
            TrainingReadinessCode.CORRUPT_INPUT,
            "backfill missing-session evidence is malformed",
        )
    missing_dates = Counter(value.split(":", 1)[1] for value in missing_pairs)
    whole_universe_gaps = sorted(
        date for date, count in missing_dates.items() if count == len(symbols)
    )
    if whole_universe_gaps:
        warnings.append("WHOLE_UNIVERSE_SESSION_GAP_EXPLICITLY_EXCLUDED")
    data_quality = promotion.get("data_quality")
    if not isinstance(data_quality, dict):
        raise TrainingReadinessError(
            TrainingReadinessCode.CORRUPT_INPUT,
            "canonical data-quality evidence is malformed",
        )
    if data_quality.get("mapping_semantics") != "NOW_KNOWN_CURRENT_UNIVERSE":
        blockers.append("UNAPPROVED_REFERENCE_SEMANTICS")
    warnings.extend(
        (
            "FIXED_CURRENT_COHORT_SURVIVORSHIP_BIAS",
            "HISTORICAL_HALT_COVERAGE_INCOMPLETE",
            "HISTORICAL_PUBLICATION_TIMES_UNOBSERVED",
            "IEX_SINGLE_EXCHANGE_COVERAGE",
        )
    )
    assessment_identity = {
        "backfill_report_sha256": backfill_sha,
        "assessment_time_utc": checked_at.isoformat().replace("+00:00", "Z"),
        "builder_source_bundle_sha256": provenance.source_bundle_sha256,
        "dataset_id": manifest["dataset_id"],
        "dataset_manifest_sha256": manifest_sha,
        "dependency_lock_sha256": provenance.dependency_lock_sha256,
        "minimum_valid_labels": minimum_valid,
        "minimum_pooled_training_rows_per_horizon": (
            MIN_POOLED_TRAINING_ROWS_PER_HORIZON
        ),
        "minimum_pooled_training_symbols_per_horizon": (
            MIN_POOLED_TRAINING_SYMBOLS_PER_HORIZON
        ),
        "model_ready_coverage_sha256": (
            _digest(model_ready) if model_ready is not None else None
        ),
        "promotion_report_sha256": promotion_sha,
        "selected_features": selected,
        "source_approval_sha256": approval_sha,
        "source_approval_document_sha256": source_approval_sha,
        "source_policy_sha256": source_policy_sha,
        "ticker_source_sha256": universe.source_file_sha256.hex(),
    }
    status = "READY_FOR_INFRASTRUCTURE_VALIDATION" if not blockers else "BLOCKED"
    body: dict[str, object] = {
        "assessment_id": _digest(assessment_identity),
        "blocking_reasons": sorted(set(blockers)),
        "authorization": {
            "approval_id": approval_id if approval_id_valid else None,
            "authorization_valid": authorization_valid,
            "checked_at_utc": checked_at.isoformat().replace("+00:00", "Z"),
            "expires_at_utc": normalized_approval_expiry,
            "model_training_authorized": (
                source_approval.get("model_training_authorized") is True
            ),
            "source_approval_sha256": source_approval_sha,
        },
        "code_provenance": provenance.document(),
        "coverage": coverage,
        "dataset": {
            "checked_bytes_decimal": checked_bytes,
            "dataset_id": manifest["dataset_id"],
            "leakage_status": cast("Mapping[str, object]", leakage).get("status"),
            "manifest_sha256": manifest_sha,
            "object_count": len(objects),
            "sample_count": manifest.get("sample_count"),
            "session_summary_count": manifest.get("session_summary_count"),
        },
        "economic_value_claimed": False,
        "external_dependencies": {
            "alpaca_api_or_network_required_for_training": False,
            "alpaca_data_authorization_bound": True,
            "alpaca_source_approval_sha256": source_approval_sha,
            "alpaca_source_policy_sha256": (
                source_policy_sha if _is_sha256(source_policy_sha) else None
            ),
            "alfred_required_for_selected_features": False,
            "gdelt_required_for_selected_features": False,
            "sector_source_required_for_selected_features": False,
        },
        "live_trading_capable": False,
        "model_ready_coverage": model_ready,
        "schema_version": TRAINING_READINESS_SCHEMA_VERSION,
        "status": status,
        "training_input_policy": {
            "allowed_feature_validity": ["DEGRADED"],
            "allowed_quality_reason_codes": list(ALLOWED_DEGRADED_REASONS),
            "excluded_features": excluded,
            "feature_vector_indices": [FEATURE_NAMES.index(name) for name in selected],
            "missing_value_policy": (
                "DROP_SAMPLE_WHEN_SELECTED_FEATURE_OR_LABEL_IS_NULL"
            ),
            "purpose": "INFRASTRUCTURE_VALIDATION_ONLY",
            "selected_features": selected,
            "unsupported_symbol_policy": "ABSTAIN",
        },
        "warnings": sorted(set(warnings)),
        "whole_universe_missing_sessions": whole_universe_gaps,
    }
    report_sha = _digest(body)
    return {**body, "report_sha256": report_sha}


def publish_training_readiness(
    repository: DataRepository,
    quota: QuotaEvidence,
    report: Mapping[str, object],
) -> Path:
    """Publish one immutable, admitted readiness report."""
    report_sha = report.get("report_sha256")
    assessment_id = report.get("assessment_id")
    if (
        not isinstance(report_sha, str)
        or not isinstance(assessment_id, str)
        or report_sha
        != _digest({k: v for k, v in report.items() if k != "report_sha256"})
    ):
        raise TrainingReadinessError(
            TrainingReadinessCode.CORRUPT_INPUT,
            "training readiness report is not self-authenticating",
        )
    if (
        report.get("status") != "READY_FOR_INFRASTRUCTURE_VALIDATION"
        or report.get("blocking_reasons") != []
        or not isinstance(report.get("model_ready_coverage"), dict)
    ):
        raise TrainingReadinessError(
            TrainingReadinessCode.INVALID_CONFIGURATION,
            "blocked or incomplete training readiness report cannot be published",
        )
    encoded = _canonical_bytes(report) + b"\n"
    staged_relative = f"tmp/training-readiness/{assessment_id}.json.part"
    final_relative = f"reports/training-readiness/{assessment_id}.json"
    staged = repository.root / staged_relative
    staged.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    descriptor = os.open(
        staged,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
        0o600,
    )
    try:
        with os.fdopen(descriptor, "wb", closefd=True) as output:
            output.write(encoded)
            output.flush()
            os.fsync(output.fileno())
    except Exception:
        staged.unlink(missing_ok=True)
        raise
    request = StorageRequest(
        f"training-readiness:{assessment_id[:24]}",
        output_bytes=len(encoded),
        temporary_bytes=len(encoded),
    )
    try:
        with repository.acquire_admission(request, quota) as lease:
            return repository.publish_staged_object(
                staged_relative,
                final_relative,
                expected_sha256=hashlib.sha256(encoded).hexdigest(),
                expected_size_bytes=len(encoded),
                lease=lease,
            )
    finally:
        staged.unlink(missing_ok=True)
