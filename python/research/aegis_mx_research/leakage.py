"""Fail-closed leakage validation for point-in-time datasets."""

from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
from typing import TYPE_CHECKING

from aegis_mx_research.types import (
    DatasetManifest,
    DatasetSample,
    DatasetSplit,
    RecordKind,
    SplitMethod,
)

if TYPE_CHECKING:
    from aegis_mx_research.store import PointInTimeStore


class LeakageCode(IntEnum):
    """Stable machine-readable leakage rejection reasons."""

    FUTURE_MACRO_REVISION = 1
    FUTURE_CONSTITUENT = 2
    POST_EVENT_ESTIMATE = 3
    FUTURE_CORPORATE_ACTION = 4
    SURVIVORSHIP_BIAS = 5
    LABEL_OVERLAP = 6
    RANDOMIZED_TIME_SPLIT = 7
    NON_CHRONOLOGICAL_SPLIT = 8
    FUTURE_RECORD = 9
    MISSING_PROVENANCE = 10
    FUTURE_FEATURE = 11


@dataclass(frozen=True, slots=True)
class LeakageViolation:
    """One deterministic leakage finding bound to sample and source record."""

    code: LeakageCode
    sample_id: str
    record_id: str | None
    detail: str


@dataclass(frozen=True, slots=True)
class LeakageReport:
    """Complete validation result; any violation makes the dataset unsafe."""

    dataset_id: str
    violations: tuple[LeakageViolation, ...]

    @property
    def accepted(self) -> bool:
        """Return true only when no leakage was detected."""
        return not self.violations

    def raise_if_invalid(self) -> None:
        """Fail closed with the complete deterministic report."""
        if self.violations:
            raise LeakageDetectedError(self)


class LeakageDetectedError(ValueError):
    """Dataset rejection retaining all machine-readable violations."""

    def __init__(self, report: LeakageReport) -> None:
        """Retain the complete report while presenting a bounded summary."""
        self.report = report
        super().__init__(
            f"dataset {report.dataset_id} rejected with "
            f"{len(report.violations)} leakage violation(s)"
        )


class LeakageValidator:
    """Deterministic point-in-time and chronological split validator."""

    def validate(
        self, manifest: DatasetManifest, store: PointInTimeStore
    ) -> LeakageReport:
        """Validate every sample, provenance record, universe, and split boundary."""
        violations: list[LeakageViolation] = []
        if manifest.split_method is SplitMethod.RANDOMIZED:
            violations.append(
                LeakageViolation(
                    LeakageCode.RANDOMIZED_TIME_SPLIT,
                    "",
                    None,
                    "randomized train/test splitting is forbidden for time series",
                )
            )
        for sample in manifest.samples:
            self._validate_sample(sample, manifest, store, violations)
        self._validate_split_boundaries(manifest, violations)
        ordered = tuple(
            sorted(
                violations,
                key=lambda item: (
                    item.code.value,
                    item.sample_id,
                    "" if item.record_id is None else item.record_id,
                    item.detail,
                ),
            )
        )
        return LeakageReport(manifest.dataset_id, ordered)

    def validate_or_raise(
        self, manifest: DatasetManifest, store: PointInTimeStore
    ) -> LeakageReport:
        """Validate and raise when any unsafe condition is present."""
        report = self.validate(manifest, store)
        report.raise_if_invalid()
        return report

    @staticmethod
    def _validate_sample(
        sample: DatasetSample,
        manifest: DatasetManifest,
        store: PointInTimeStore,
        violations: list[LeakageViolation],
    ) -> None:
        if sample.feature_end_ns > sample.event_time_ns:
            violations.append(
                LeakageViolation(
                    LeakageCode.FUTURE_FEATURE,
                    sample.sample_id,
                    None,
                    "feature window extends beyond the sample event time",
                )
            )
        if sample.label_start_ns <= sample.feature_end_ns:
            violations.append(
                LeakageViolation(
                    LeakageCode.LABEL_OVERLAP,
                    sample.sample_id,
                    None,
                    "label interval overlaps the feature interval",
                )
            )
        for record_id in sample.record_ids:
            record = store.record_by_id(record_id)
            if record is None:
                violations.append(
                    LeakageViolation(
                        LeakageCode.MISSING_PROVENANCE,
                        sample.sample_id,
                        record_id,
                        "sample references an unknown point-in-time record",
                    )
                )
                continue
            cutoff = min(sample.feature_end_ns, sample.event_time_ns)
            if record.available_at_ns > cutoff:
                violations.append(
                    LeakageViolation(
                        _future_code(record.kind),
                        sample.sample_id,
                        record_id,
                        "record was unavailable at the feature cutoff",
                    )
                )
            if (
                record.kind is RecordKind.INDEX_MEMBERSHIP
                and not record.validity.contains(sample.event_time_ns)
            ):
                violations.append(
                    LeakageViolation(
                        LeakageCode.FUTURE_CONSTITUENT,
                        sample.sample_id,
                        record_id,
                        "constituent state is not effective at the sample time",
                    )
                )
            if (
                record.kind is RecordKind.CORPORATE_ACTION
                and not record.validity.contains(sample.event_time_ns)
            ):
                violations.append(
                    LeakageViolation(
                        LeakageCode.FUTURE_CORPORATE_ACTION,
                        sample.sample_id,
                        record_id,
                        "corporate action was applied outside its effective interval",
                    )
                )
        expected_universe = store.membership_at(
            sample.event_time_ns,
            index_id=manifest.universe_index_id,
            known_at_ns=min(sample.feature_end_ns, sample.event_time_ns),
        )
        if set(expected_universe) != set(sample.universe_instrument_ids):
            violations.append(
                LeakageViolation(
                    LeakageCode.SURVIVORSHIP_BIAS,
                    sample.sample_id,
                    None,
                    "sample universe differs from point-in-time index membership",
                )
            )

    @staticmethod
    def _validate_split_boundaries(
        manifest: DatasetManifest, violations: list[LeakageViolation]
    ) -> None:
        grouped = {
            split: tuple(sample for sample in manifest.samples if sample.split is split)
            for split in DatasetSplit
        }
        prior: tuple[DatasetSample, ...] = ()
        for split in DatasetSplit:
            current = grouped[split]
            if current and prior:
                latest_prior_event = max(sample.event_time_ns for sample in prior)
                earliest_current_event = min(sample.event_time_ns for sample in current)
                if latest_prior_event >= earliest_current_event:
                    violations.append(
                        LeakageViolation(
                            LeakageCode.NON_CHRONOLOGICAL_SPLIT,
                            min(
                                current, key=lambda sample: sample.event_time_ns
                            ).sample_id,
                            None,
                            "later dataset partition is not strictly later in time",
                        )
                    )
                latest_prior_label = max(sample.label_end_ns for sample in prior)
                earliest_current_feature = min(
                    sample.feature_start_ns for sample in current
                )
                if latest_prior_label + manifest.embargo_ns > earliest_current_feature:
                    violations.append(
                        LeakageViolation(
                            LeakageCode.LABEL_OVERLAP,
                            min(
                                current, key=lambda sample: sample.feature_start_ns
                            ).sample_id,
                            None,
                            "prior labels overlap the next split or its embargo",
                        )
                    )
            prior += current


def _future_code(kind: RecordKind) -> LeakageCode:
    if kind is RecordKind.MACRO_VINTAGE:
        return LeakageCode.FUTURE_MACRO_REVISION
    if kind is RecordKind.INDEX_MEMBERSHIP:
        return LeakageCode.FUTURE_CONSTITUENT
    if kind is RecordKind.ANALYST_ESTIMATE:
        return LeakageCode.POST_EVENT_ESTIMATE
    if kind is RecordKind.CORPORATE_ACTION:
        return LeakageCode.FUTURE_CORPORATE_ACTION
    return LeakageCode.FUTURE_RECORD
