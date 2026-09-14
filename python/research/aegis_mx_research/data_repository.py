"""Fail-closed storage controls for the bounded forecasting proof of concept."""

from __future__ import annotations

import errno
import fcntl
import hashlib
import json
import os
import stat
from dataclasses import dataclass
from datetime import UTC, datetime
from enum import StrEnum
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, Final, Self, cast

if TYPE_CHECKING:
    from collections.abc import Callable, Iterator, Mapping, Sequence

DATA_REPOSITORY_SCHEMA_VERSION: Final = "3.0.0"
POLICY_MIGRATION_SCHEMA_VERSION: Final = "1.0.0"
MANIFEST_SCHEMA_VERSION: Final = "1.0.0"
AUDIT_SCHEMA_VERSION: Final = "1.0.0"
DECIMAL_GB: Final = 1_000_000_000
BINARY_GIB: Final = 1 << 30
BINARY_TIB: Final = 1 << 40
MAX_BYTES: Final = (1 << 63) - 1
MAX_MANIFEST_BYTES: Final = 1_048_576
MAX_PATH_BYTES: Final = 512
SHA256_HEX_BYTES: Final = 64
DEFAULT_DATA_ROOT: Final = Path("/scratch/djy8hg/aegis_mx_poc_data")
POC_STORAGE_LIMIT_BYTES: Final = 800 * DECIMAL_GB
STORAGE_AREAS: Final = (
    "quarantine",
    "raw",
    "canonical",
    "derived",
    "datasets",
    "manifests",
    "models",
    "reports",
    "tmp",
)
OBJECT_AREAS: Final = frozenset(
    {"quarantine", "raw", "canonical", "derived", "datasets", "models", "reports"}
)
PARTIAL_SUFFIXES: Final = (".part", ".partial", ".download")
_PROJECT_ROOT: Final = Path(__file__).resolve().parents[3]


class StorageErrorCode(StrEnum):
    """Stable reason codes for storage and integrity failures."""

    INVALID_POLICY = "INVALID_POLICY"
    INVALID_REQUEST = "INVALID_REQUEST"
    INVALID_PATH = "INVALID_PATH"
    SYMLINK_DETECTED = "SYMLINK_DETECTED"
    HARDLINK_DETECTED = "HARDLINK_DETECTED"
    UNSUPPORTED_FILE = "UNSUPPORTED_FILE"
    FILESYSTEM_UNKNOWN = "FILESYSTEM_UNKNOWN"
    CONCURRENT_ADMISSION = "CONCURRENT_ADMISSION"
    ADMISSION_DENIED = "ADMISSION_DENIED"
    MANIFEST_CORRUPT = "MANIFEST_CORRUPT"
    HASH_MISMATCH = "HASH_MISMATCH"
    SIZE_MISMATCH = "SIZE_MISMATCH"
    IMMUTABLE_CONFLICT = "IMMUTABLE_CONFLICT"
    INTERRUPTED_PUBLICATION = "INTERRUPTED_PUBLICATION"
    MISSING_LINEAGE = "MISSING_LINEAGE"
    IO_FAILURE = "IO_FAILURE"


class AdmissionReason(StrEnum):
    """Deterministically ordered admission dispositions."""

    INTEGER_OVERFLOW = "INTEGER_OVERFLOW"
    PROJECTION_UNKNOWN = "PROJECTION_UNKNOWN"
    QUOTA_UNKNOWN = "QUOTA_UNKNOWN"
    FILESYSTEM_CAPACITY_UNKNOWN = "FILESYSTEM_CAPACITY_UNKNOWN"
    TEMPORARY_LIMIT = "TEMPORARY_LIMIT"
    HARD_ROOT_LIMIT = "HARD_ROOT_LIMIT"
    TARGET_REVIEW_REQUIRED = "TARGET_REVIEW_REQUIRED"
    FILESYSTEM_RESERVE = "FILESYSTEM_RESERVE"
    QUOTA_EXHAUSTED = "QUOTA_EXHAUSTED"


class LineageRelation(StrEnum):
    """How one immutable manifest relates to a prior manifest."""

    NONE = "NONE"
    CORRECTS = "CORRECTS"
    REPLACES = "REPLACES"


class PublicationStage(StrEnum):
    """Injectable atomic-publication fault boundaries."""

    TEMP_FILE_SYNCED = "TEMP_FILE_SYNCED"
    MANIFEST_LINKED = "MANIFEST_LINKED"


class StorageError(RuntimeError):
    """Typed fail-closed storage exception."""

    def __init__(self, code: StorageErrorCode, message: str) -> None:
        """Retain a stable machine-readable code beside the bounded message."""
        super().__init__(message)
        self.code = code


def _canonical_bytes(value: object) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("ascii")


def _sha256_hex(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def _is_exact_int(value: object) -> bool:
    return type(value) is int


def _validate_byte_count(value: int, name: str) -> None:
    if not _is_exact_int(value) or value < 0 or value > MAX_BYTES:
        message = f"{name} must be an integer from 0 through {MAX_BYTES}"
        raise StorageError(StorageErrorCode.INVALID_REQUEST, message)


LEGACY_STORAGE_POLICY_V2 = {
    "administrative_allocation_bytes_decimal": 10 * BINARY_TIB,
    "hard_root_bytes_decimal": 100 * DECIMAL_GB,
    "minimum_reserve_bytes_decimal": 50 * DECIMAL_GB,
    "schema_version": "2.0.0",
    "target_root_bytes_decimal": 80 * DECIMAL_GB,
    "temporary_limit_bytes_decimal": 20 * DECIMAL_GB,
}
LEGACY_STORAGE_POLICY_V2_SHA256: Final = _sha256_hex(
    _canonical_bytes(LEGACY_STORAGE_POLICY_V2)
)


def _checked_add(*values: int) -> int:
    total = 0
    for value in values:
        _validate_byte_count(value, "byte count")
        if value > MAX_BYTES - total:
            message = "signed 64-bit byte calculation overflow"
            raise OverflowError(message)
        total += value
    return total


def _validate_text(value: str, name: str, maximum: int = 160) -> None:
    if (
        not isinstance(value, str)
        or not value
        or "\x00" in value
        or len(value.encode("utf-8")) > maximum
    ):
        message = f"{name} must be nonempty, NUL-free, and at most {maximum} bytes"
        raise StorageError(StorageErrorCode.MANIFEST_CORRUPT, message)


def _validate_sha256(value: str, name: str) -> None:
    if (
        not isinstance(value, str)
        or len(value) != SHA256_HEX_BYTES
        or value.lower() != value
        or any(character not in "0123456789abcdef" for character in value)
        or not any(character != "0" for character in value)
    ):
        message = f"{name} must be a nonzero lowercase SHA-256 digest"
        raise StorageError(StorageErrorCode.MANIFEST_CORRUPT, message)


def _validate_manifest_id(
    value: str, name: str, *, allowed_prefixes: frozenset[str]
) -> None:
    _validate_text(value, name)
    prefix, separator, digest = value.partition("-")
    if not separator or prefix not in allowed_prefixes:
        message = f"{name} has an invalid manifest type"
        raise StorageError(StorageErrorCode.MANIFEST_CORRUPT, message)
    _validate_sha256(digest, name)


def _validate_utc(value: str, name: str) -> None:
    _validate_text(value, name, 40)
    try:
        parsed = datetime.fromisoformat(value)
    except ValueError as error:
        message = f"{name} must be an ISO-8601 UTC timestamp"
        raise StorageError(StorageErrorCode.INVALID_REQUEST, message) from error
    if parsed.tzinfo is None or parsed.utcoffset() != UTC.utcoffset(parsed):
        message = f"{name} must have an explicit UTC offset"
        raise StorageError(StorageErrorCode.INVALID_REQUEST, message)


def _quantity(value: int | None) -> dict[str, int | str | None]:
    if value is None:
        return {"bytes_decimal": None, "gib_binary": None}
    return {
        "bytes_decimal": value,
        "gib_binary": f"{value / BINARY_GIB:.6f}",
    }


def _validate_relative_path(
    value: str,
    *,
    allowed_areas: frozenset[str] | None = None,
) -> PurePosixPath:
    if (
        not isinstance(value, str)
        or not value
        or "\x00" in value
        or "\\" in value
        or len(value.encode("utf-8")) > MAX_PATH_BYTES
    ):
        raise StorageError(StorageErrorCode.INVALID_PATH, "storage path is malformed")
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
        raise StorageError(
            StorageErrorCode.INVALID_PATH,
            "storage path must be a normalized relative POSIX path",
        )
    if allowed_areas is not None and path.parts[0] not in allowed_areas:
        raise StorageError(
            StorageErrorCode.INVALID_PATH,
            "storage path is outside an approved object area",
        )
    return path


@dataclass(frozen=True, slots=True)
class StoragePolicy:
    """Immutable decimal-byte limits for the bounded POC."""

    administrative_allocation_bytes: int = 10 * BINARY_TIB
    target_root_bytes: int = POC_STORAGE_LIMIT_BYTES
    hard_root_bytes: int = POC_STORAGE_LIMIT_BYTES
    minimum_reserve_bytes: int = 50 * DECIMAL_GB
    temporary_limit_bytes: int = 20 * DECIMAL_GB
    schema_version: str = DATA_REPOSITORY_SCHEMA_VERSION

    def __post_init__(self) -> None:
        """Require the target, hard, temporary, and allocation limits to nest."""
        for name, value in (
            (
                "administrative_allocation_bytes",
                self.administrative_allocation_bytes,
            ),
            ("target_root_bytes", self.target_root_bytes),
            ("hard_root_bytes", self.hard_root_bytes),
            ("minimum_reserve_bytes", self.minimum_reserve_bytes),
            ("temporary_limit_bytes", self.temporary_limit_bytes),
        ):
            try:
                _validate_byte_count(value, name)
            except StorageError as error:
                message = "storage policy limits are inconsistent or unsupported"
                raise StorageError(StorageErrorCode.INVALID_POLICY, message) from error
        if (
            self.administrative_allocation_bytes != 10 * BINARY_TIB
            or self.target_root_bytes > POC_STORAGE_LIMIT_BYTES
            or self.hard_root_bytes > POC_STORAGE_LIMIT_BYTES
            or self.minimum_reserve_bytes < 50 * DECIMAL_GB
            or self.temporary_limit_bytes > 20 * DECIMAL_GB
            or self.target_root_bytes > self.hard_root_bytes
            or self.hard_root_bytes > self.administrative_allocation_bytes
            or self.temporary_limit_bytes > self.hard_root_bytes
            or self.schema_version != DATA_REPOSITORY_SCHEMA_VERSION
        ):
            message = "storage policy limits are inconsistent or unsupported"
            raise StorageError(StorageErrorCode.INVALID_POLICY, message)

    @property
    def sha256(self) -> str:
        """Return the deterministic policy identity."""
        return _sha256_hex(_canonical_bytes(self.to_dict()))

    def to_dict(self) -> dict[str, int | str]:
        """Return the canonical decimal-byte policy representation."""
        return {
            "administrative_allocation_bytes_decimal": (
                self.administrative_allocation_bytes
            ),
            "hard_root_bytes_decimal": self.hard_root_bytes,
            "minimum_reserve_bytes_decimal": self.minimum_reserve_bytes,
            "schema_version": self.schema_version,
            "target_root_bytes_decimal": self.target_root_bytes,
            "temporary_limit_bytes_decimal": self.temporary_limit_bytes,
        }


@dataclass(frozen=True, slots=True)
class FileSystemState:
    """Filesystem capacity used only for the physical reserve control."""

    total_bytes: int | None
    free_bytes: int | None

    def __post_init__(self) -> None:
        """Validate known values without treating them as quota evidence."""
        for name, value in (
            ("total_bytes", self.total_bytes),
            ("free_bytes", self.free_bytes),
        ):
            if value is not None:
                _validate_byte_count(value, name)
        if (
            self.total_bytes is not None
            and self.free_bytes is not None
            and self.free_bytes > self.total_bytes
        ):
            message = "filesystem free bytes cannot exceed total bytes"
            raise StorageError(StorageErrorCode.FILESYSTEM_UNKNOWN, message)

    @property
    def known(self) -> bool:
        """Return whether both capacity values are authoritative observations."""
        return self.total_bytes is not None and self.free_bytes is not None


@dataclass(frozen=True, slots=True)
class QuotaEvidence:
    """Explicit user-entitlement evidence; filesystem free space is insufficient."""

    limit_bytes: int | None = None
    used_bytes: int | None = None
    source: str | None = None
    observed_at_utc: str | None = None
    authoritative: bool = False

    def __post_init__(self) -> None:
        """Validate supplied fields while allowing an explicit unknown state."""
        for name, value in (
            ("limit_bytes", self.limit_bytes),
            ("used_bytes", self.used_bytes),
        ):
            if value is not None:
                _validate_byte_count(value, name)
        if (
            self.limit_bytes is not None
            and self.used_bytes is not None
            and self.used_bytes > self.limit_bytes
        ):
            message = "quota used bytes cannot exceed quota limit"
            raise StorageError(StorageErrorCode.INVALID_REQUEST, message)
        if self.source is not None:
            _validate_text(self.source, "quota source")
        if self.observed_at_utc is not None:
            _validate_utc(self.observed_at_utc, "quota observed_at_utc")

    @property
    def known(self) -> bool:
        """Require complete, explicitly authoritative quota evidence."""
        return (
            self.authoritative
            and self.limit_bytes is not None
            and self.used_bytes is not None
            and self.source is not None
            and self.observed_at_utc is not None
        )

    def to_dict(self) -> dict[str, object]:
        """Return labeled quota evidence without inventing missing values."""
        return {
            "authoritative": self.authoritative,
            "known": self.known,
            "limit": _quantity(self.limit_bytes),
            "observed_at_utc": self.observed_at_utc,
            "source": self.source,
            "used": _quantity(self.used_bytes),
        }


@dataclass(frozen=True, slots=True)
class StorageRequest:
    """Bounded projected writes for one admission epoch."""

    operation_id: str
    output_bytes: int | None
    temporary_bytes: int = 0
    retry_overhead_bytes: int = 0

    def __post_init__(self) -> None:
        """Validate bounded byte projections and operation identity."""
        try:
            _validate_text(self.operation_id, "operation_id")
        except StorageError as error:
            raise StorageError(StorageErrorCode.INVALID_REQUEST, str(error)) from error
        for name, value in (
            ("output_bytes", self.output_bytes),
            ("temporary_bytes", self.temporary_bytes),
            ("retry_overhead_bytes", self.retry_overhead_bytes),
        ):
            if value is not None:
                try:
                    _validate_byte_count(value, name)
                except StorageError as error:
                    raise StorageError(
                        StorageErrorCode.INVALID_REQUEST, str(error)
                    ) from error

    def to_dict(self) -> dict[str, object]:
        """Return the labeled request representation."""
        return {
            "operation_id": self.operation_id,
            "output": _quantity(self.output_bytes),
            "retry_overhead": _quantity(self.retry_overhead_bytes),
            "temporary": _quantity(self.temporary_bytes),
        }


@dataclass(frozen=True, slots=True)
class FileInventory:
    """One safely inspected regular file."""

    relative_path: str
    logical_bytes: int
    allocated_bytes: int
    area: str
    partial: bool
    temporary: bool


@dataclass(frozen=True, slots=True)
class StorageUsage:
    """Logical-size accounting with explicit partial and temporary categories."""

    committed_bytes: int
    partial_bytes: int
    temporary_bytes: int
    logical_bytes: int
    allocated_bytes: int
    file_count: int
    partial_file_count: int
    temporary_file_count: int
    filesystem: FileSystemState
    inventory: tuple[FileInventory, ...]

    @property
    def total_bytes(self) -> int:
        """Return total logical data-root bytes without category double counting."""
        return self.logical_bytes

    def to_dict(self) -> dict[str, object]:
        """Return decimal bytes plus separately labeled binary GiB."""
        return {
            "allocated": _quantity(self.allocated_bytes),
            "committed": _quantity(self.committed_bytes),
            "file_count": self.file_count,
            "filesystem": {
                "free": _quantity(self.filesystem.free_bytes),
                "known": self.filesystem.known,
                "total": _quantity(self.filesystem.total_bytes),
                "used_as_quota_entitlement": False,
            },
            "partial": _quantity(self.partial_bytes),
            "partial_file_count": self.partial_file_count,
            "temporary": _quantity(self.temporary_bytes),
            "temporary_file_count": self.temporary_file_count,
            "total": _quantity(self.total_bytes),
        }


@dataclass(frozen=True, slots=True)
class AdmissionDecision:
    """Auditable admission result calculated while holding the writer lock."""

    admitted: bool
    reasons: tuple[AdmissionReason, ...]
    request: StorageRequest
    usage: StorageUsage
    quota: QuotaEvidence
    effective_quota_limit_bytes: int | None
    projected_root_peak_bytes: int | None
    projected_temporary_peak_bytes: int | None
    projected_quota_used_bytes: int | None
    projected_filesystem_free_bytes: int | None
    policy_sha256: str

    def to_dict(self) -> dict[str, object]:
        """Return a stable, unit-labeled decision record."""
        return {
            "admitted": self.admitted,
            "effective_quota_limit": _quantity(self.effective_quota_limit_bytes),
            "policy_sha256": self.policy_sha256,
            "projected_filesystem_free": _quantity(
                self.projected_filesystem_free_bytes
            ),
            "projected_quota_used": _quantity(self.projected_quota_used_bytes),
            "projected_root_peak": _quantity(self.projected_root_peak_bytes),
            "projected_temporary_peak": _quantity(self.projected_temporary_peak_bytes),
            "quota": self.quota.to_dict(),
            "reasons": [reason.value for reason in self.reasons],
            "request": self.request.to_dict(),
            "usage": self.usage.to_dict(),
        }


@dataclass(frozen=True, slots=True)
class _RootProjection:
    increment_bytes: int | None
    root_peak_bytes: int | None
    temporary_peak_bytes: int | None
    reason: AdmissionReason | None


def _root_projection(request: StorageRequest, usage: StorageUsage) -> _RootProjection:
    if request.output_bytes is None:
        return _RootProjection(None, None, None, AdmissionReason.PROJECTION_UNKNOWN)
    try:
        increment = _checked_add(
            request.output_bytes,
            request.temporary_bytes,
            request.retry_overhead_bytes,
        )
        root_peak = _checked_add(usage.total_bytes, increment)
        temporary_peak = _checked_add(
            usage.temporary_bytes,
            request.temporary_bytes,
            request.retry_overhead_bytes,
        )
    except OverflowError:
        return _RootProjection(None, None, None, AdmissionReason.INTEGER_OVERFLOW)
    return _RootProjection(increment, root_peak, temporary_peak, None)


def _quota_projection(
    quota: QuotaEvidence,
    policy: StoragePolicy,
    increment_bytes: int | None,
) -> tuple[int | None, int | None, AdmissionReason | None]:
    if not quota.known:
        return None, None, AdmissionReason.QUOTA_UNKNOWN
    quota_limit = cast("int", quota.limit_bytes)
    quota_used = cast("int", quota.used_bytes)
    effective_limit = min(quota_limit, policy.administrative_allocation_bytes)
    if increment_bytes is None:
        return effective_limit, None, None
    try:
        projected_used = _checked_add(quota_used, increment_bytes)
    except OverflowError:
        return effective_limit, None, AdmissionReason.INTEGER_OVERFLOW
    return effective_limit, projected_used, None


def _filesystem_projection(
    filesystem: FileSystemState, increment_bytes: int | None
) -> tuple[int | None, AdmissionReason | None]:
    if not filesystem.known:
        return None, AdmissionReason.FILESYSTEM_CAPACITY_UNKNOWN
    if increment_bytes is None:
        return None, None
    filesystem_free = cast("int", filesystem.free_bytes)
    return max(filesystem_free - increment_bytes, 0), None


def _limit_reasons(
    policy: StoragePolicy,
    root: _RootProjection,
    filesystem_free: int | None,
    quota_projection: tuple[int | None, int | None],
) -> tuple[AdmissionReason, ...]:
    reasons: list[AdmissionReason] = []
    quota_limit, quota_used = quota_projection
    if (
        root.temporary_peak_bytes is not None
        and root.temporary_peak_bytes > policy.temporary_limit_bytes
    ):
        reasons.append(AdmissionReason.TEMPORARY_LIMIT)
    if (
        root.root_peak_bytes is not None
        and root.root_peak_bytes > policy.hard_root_bytes
    ):
        reasons.append(AdmissionReason.HARD_ROOT_LIMIT)
    if (
        root.root_peak_bytes is not None
        and root.root_peak_bytes > policy.target_root_bytes
    ):
        reasons.append(AdmissionReason.TARGET_REVIEW_REQUIRED)
    if filesystem_free is not None and filesystem_free < policy.minimum_reserve_bytes:
        reasons.append(AdmissionReason.FILESYSTEM_RESERVE)
    if quota_used is not None and quota_limit is not None and quota_used > quota_limit:
        reasons.append(AdmissionReason.QUOTA_EXHAUSTED)
    return tuple(reasons)


@dataclass(frozen=True, slots=True)
class ManifestTimeRange:
    """Aggregate point-in-time domains retained by every data manifest."""

    event_time_min_ns: int
    event_time_max_ns: int
    publication_time_min_ns: int
    publication_time_max_ns: int
    receive_time_min_ns: int
    receive_time_max_ns: int
    processing_time_min_ns: int
    processing_time_max_ns: int
    revision_time_min_ns: int
    revision_time_max_ns: int

    def __post_init__(self) -> None:
        """Validate each range and the aggregate availability ordering."""
        pairs = (
            ("event", self.event_time_min_ns, self.event_time_max_ns),
            ("publication", self.publication_time_min_ns, self.publication_time_max_ns),
            ("receive", self.receive_time_min_ns, self.receive_time_max_ns),
            ("processing", self.processing_time_min_ns, self.processing_time_max_ns),
            ("revision", self.revision_time_min_ns, self.revision_time_max_ns),
        )
        for name, minimum, maximum in pairs:
            if (
                not _is_exact_int(minimum)
                or not _is_exact_int(maximum)
                or minimum < 0
                or maximum < minimum
                or maximum > MAX_BYTES
            ):
                message = f"{name} time range is invalid"
                raise StorageError(StorageErrorCode.MANIFEST_CORRUPT, message)
        if (
            self.receive_time_min_ns < self.publication_time_min_ns
            or self.receive_time_max_ns < self.publication_time_max_ns
            or self.processing_time_min_ns < self.receive_time_min_ns
            or self.processing_time_max_ns < self.receive_time_max_ns
            or self.revision_time_min_ns < self.publication_time_min_ns
            or self.revision_time_max_ns < self.publication_time_max_ns
        ):
            message = "manifest availability time ranges are inconsistent"
            raise StorageError(StorageErrorCode.MANIFEST_CORRUPT, message)

    def to_dict(self) -> dict[str, int]:
        """Return all explicit timestamp domains."""
        return {
            "event_time_max_ns": self.event_time_max_ns,
            "event_time_min_ns": self.event_time_min_ns,
            "processing_time_max_ns": self.processing_time_max_ns,
            "processing_time_min_ns": self.processing_time_min_ns,
            "publication_time_max_ns": self.publication_time_max_ns,
            "publication_time_min_ns": self.publication_time_min_ns,
            "receive_time_max_ns": self.receive_time_max_ns,
            "receive_time_min_ns": self.receive_time_min_ns,
            "revision_time_max_ns": self.revision_time_max_ns,
            "revision_time_min_ns": self.revision_time_min_ns,
        }


@dataclass(frozen=True, slots=True)
class ManifestLineage:
    """Explicit correction or replacement relationship."""

    relation: LineageRelation = LineageRelation.NONE
    parent_manifest_id: str | None = None
    reason: str | None = None

    def __post_init__(self) -> None:
        """Require complete lineage for corrections and replacements."""
        if self.relation is LineageRelation.NONE:
            if self.parent_manifest_id is not None or self.reason is not None:
                message = "NONE lineage cannot name a parent or reason"
                raise StorageError(StorageErrorCode.MANIFEST_CORRUPT, message)
            return
        if self.parent_manifest_id is None or self.reason is None:
            message = "correction or replacement lineage requires parent and reason"
            raise StorageError(StorageErrorCode.MANIFEST_CORRUPT, message)
        _validate_manifest_id(
            self.parent_manifest_id,
            "lineage parent_manifest_id",
            allowed_prefixes=frozenset({"source", "partition"}),
        )
        _validate_text(self.reason, "lineage reason")

    def to_dict(self) -> dict[str, str | None]:
        """Return canonical lineage metadata."""
        return {
            "parent_manifest_id": self.parent_manifest_id,
            "reason": self.reason,
            "relation": self.relation.value,
        }


def _validate_manifest_common(manifest: SourceManifest | PartitionManifest) -> None:
    _validate_relative_path(manifest.storage_path, allowed_areas=OBJECT_AREAS)
    _validate_sha256(manifest.object_sha256, "object_sha256")
    _validate_sha256(manifest.universe_snapshot_sha256, "universe_snapshot_sha256")
    _validate_text(manifest.schema_name, "schema_name")
    _validate_text(manifest.schema_version, "schema_version", SHA256_HEX_BYTES)
    try:
        _validate_byte_count(manifest.size_bytes, "size_bytes")
        _validate_byte_count(manifest.record_count, "record_count")
    except StorageError as error:
        raise StorageError(StorageErrorCode.MANIFEST_CORRUPT, str(error)) from error


@dataclass(frozen=True, slots=True)
class SourceManifest:
    """Immutable content-addressed source-object manifest."""

    source_name: str
    source_version: str
    source_object_id: str
    storage_path: str
    object_sha256: str
    size_bytes: int
    record_count: int
    schema_name: str
    schema_version: str
    times: ManifestTimeRange
    universe_snapshot_sha256: str
    lineage: ManifestLineage

    def __post_init__(self) -> None:
        """Validate bounded source identity and common object metadata."""
        _validate_text(self.source_name, "source_name")
        _validate_text(self.source_version, "source_version")
        _validate_text(self.source_object_id, "source_object_id")
        _validate_manifest_common(self)

    @property
    def manifest_id(self) -> str:
        """Return a deterministic identity over the complete immutable payload."""
        return _manifest_identity("source", self.to_payload())

    def to_payload(self) -> dict[str, object]:
        """Return the canonical source payload."""
        return {
            "lineage": self.lineage.to_dict(),
            "object_sha256": self.object_sha256,
            "record_count": self.record_count,
            "schema_name": self.schema_name,
            "schema_version": self.schema_version,
            "size_bytes_decimal": self.size_bytes,
            "source_name": self.source_name,
            "source_object_id": self.source_object_id,
            "source_version": self.source_version,
            "storage_path": self.storage_path,
            "times": self.times.to_dict(),
            "universe_snapshot_sha256": self.universe_snapshot_sha256,
        }


@dataclass(frozen=True, slots=True)
class PartitionManifest:
    """Immutable canonical or derived data-partition manifest."""

    dataset_name: str
    partition_key: str
    storage_path: str
    object_sha256: str
    size_bytes: int
    record_count: int
    schema_name: str
    schema_version: str
    times: ManifestTimeRange
    universe_snapshot_sha256: str
    source_manifest_ids: tuple[str, ...]
    lineage: ManifestLineage

    def __post_init__(self) -> None:
        """Validate partition identity and canonicalize source references."""
        _validate_text(self.dataset_name, "dataset_name")
        _validate_text(self.partition_key, "partition_key")
        _validate_manifest_common(self)
        if not self.source_manifest_ids or len(set(self.source_manifest_ids)) != len(
            self.source_manifest_ids
        ):
            message = "partition source_manifest_ids must be nonempty and unique"
            raise StorageError(StorageErrorCode.MANIFEST_CORRUPT, message)
        for identifier in self.source_manifest_ids:
            _validate_manifest_id(
                identifier,
                "source_manifest_id",
                allowed_prefixes=frozenset({"source"}),
            )
        object.__setattr__(
            self, "source_manifest_ids", tuple(sorted(self.source_manifest_ids))
        )

    @property
    def manifest_id(self) -> str:
        """Return a deterministic identity over the complete immutable payload."""
        return _manifest_identity("partition", self.to_payload())

    def to_payload(self) -> dict[str, object]:
        """Return the canonical partition payload."""
        return {
            "dataset_name": self.dataset_name,
            "lineage": self.lineage.to_dict(),
            "object_sha256": self.object_sha256,
            "partition_key": self.partition_key,
            "record_count": self.record_count,
            "schema_name": self.schema_name,
            "schema_version": self.schema_version,
            "size_bytes_decimal": self.size_bytes,
            "source_manifest_ids": list(self.source_manifest_ids),
            "storage_path": self.storage_path,
            "times": self.times.to_dict(),
            "universe_snapshot_sha256": self.universe_snapshot_sha256,
        }


type DataManifest = SourceManifest | PartitionManifest


def _manifest_identity(kind: str, payload: Mapping[str, object]) -> str:
    body = {
        "manifest_schema_version": MANIFEST_SCHEMA_VERSION,
        "manifest_type": kind.upper(),
        "payload": payload,
    }
    return f"{kind}-{_sha256_hex(_canonical_bytes(body))}"


def encode_manifest(manifest: DataManifest) -> bytes:
    """Serialize a manifest canonically with redundant integrity metadata."""
    kind = "source" if isinstance(manifest, SourceManifest) else "partition"
    body = {
        "manifest_schema_version": MANIFEST_SCHEMA_VERSION,
        "manifest_type": kind.upper(),
        "payload": manifest.to_payload(),
    }
    digest = _sha256_hex(_canonical_bytes(body))
    envelope = {
        **body,
        "manifest_id": f"{kind}-{digest}",
        "manifest_sha256": digest,
    }
    return _canonical_bytes(envelope) + b"\n"


def _require_mapping(value: object, name: str) -> Mapping[str, object]:
    if not isinstance(value, dict) or not all(isinstance(key, str) for key in value):
        message = f"{name} must be a JSON object with string keys"
        raise StorageError(StorageErrorCode.MANIFEST_CORRUPT, message)
    return value


def _require_exact_keys(
    value: Mapping[str, object], expected: frozenset[str], name: str
) -> None:
    if value.keys() != expected:
        message = f"{name} fields do not match schema"
        raise StorageError(StorageErrorCode.MANIFEST_CORRUPT, message)


def _require_str(value: object, name: str) -> str:
    if not isinstance(value, str):
        message = f"{name} must be a string"
        raise StorageError(StorageErrorCode.MANIFEST_CORRUPT, message)
    return value


def _require_int(value: object, name: str) -> int:
    if not _is_exact_int(value):
        message = f"{name} must be an integer"
        raise StorageError(StorageErrorCode.MANIFEST_CORRUPT, message)
    return cast("int", value)


def _decode_times(value: object) -> ManifestTimeRange:
    mapping = _require_mapping(value, "times")
    fields = frozenset(
        {
            "event_time_max_ns",
            "event_time_min_ns",
            "processing_time_max_ns",
            "processing_time_min_ns",
            "publication_time_max_ns",
            "publication_time_min_ns",
            "receive_time_max_ns",
            "receive_time_min_ns",
            "revision_time_max_ns",
            "revision_time_min_ns",
        }
    )
    _require_exact_keys(mapping, fields, "times")
    return ManifestTimeRange(
        **{name: _require_int(mapping[name], name) for name in fields}
    )


def _decode_lineage(value: object) -> ManifestLineage:
    mapping = _require_mapping(value, "lineage")
    _require_exact_keys(
        mapping,
        frozenset({"parent_manifest_id", "reason", "relation"}),
        "lineage",
    )
    parent = mapping["parent_manifest_id"]
    reason = mapping["reason"]
    if parent is not None and not isinstance(parent, str):
        raise StorageError(
            StorageErrorCode.MANIFEST_CORRUPT,
            "lineage parent_manifest_id must be a string or null",
        )
    if reason is not None and not isinstance(reason, str):
        raise StorageError(
            StorageErrorCode.MANIFEST_CORRUPT,
            "lineage reason must be a string or null",
        )
    try:
        relation = LineageRelation(_require_str(mapping["relation"], "relation"))
    except ValueError as error:
        raise StorageError(
            StorageErrorCode.MANIFEST_CORRUPT, "lineage relation is invalid"
        ) from error
    return ManifestLineage(relation, parent, reason)


_COMMON_PAYLOAD_FIELDS: Final = frozenset(
    {
        "lineage",
        "object_sha256",
        "record_count",
        "schema_name",
        "schema_version",
        "size_bytes_decimal",
        "storage_path",
        "times",
        "universe_snapshot_sha256",
    }
)


@dataclass(frozen=True, slots=True)
class _DecodedCommon:
    lineage: ManifestLineage
    object_sha256: str
    record_count: int
    schema_name: str
    schema_version: str
    size_bytes: int
    storage_path: str
    times: ManifestTimeRange
    universe_snapshot_sha256: str


def _decode_common_payload(payload: Mapping[str, object]) -> _DecodedCommon:
    return _DecodedCommon(
        lineage=_decode_lineage(payload["lineage"]),
        object_sha256=_require_str(payload["object_sha256"], "object_sha256"),
        record_count=_require_int(payload["record_count"], "record_count"),
        schema_name=_require_str(payload["schema_name"], "schema_name"),
        schema_version=_require_str(payload["schema_version"], "schema_version"),
        size_bytes=_require_int(payload["size_bytes_decimal"], "size_bytes"),
        storage_path=_require_str(payload["storage_path"], "storage_path"),
        times=_decode_times(payload["times"]),
        universe_snapshot_sha256=_require_str(
            payload["universe_snapshot_sha256"], "universe_snapshot_sha256"
        ),
    )


def _decode_source_payload(payload: Mapping[str, object]) -> SourceManifest:
    _require_exact_keys(
        payload,
        _COMMON_PAYLOAD_FIELDS | {"source_name", "source_object_id", "source_version"},
        "source payload",
    )
    common = _decode_common_payload(payload)
    return SourceManifest(
        source_name=_require_str(payload["source_name"], "source_name"),
        source_version=_require_str(payload["source_version"], "source_version"),
        source_object_id=_require_str(payload["source_object_id"], "source_object_id"),
        storage_path=common.storage_path,
        object_sha256=common.object_sha256,
        size_bytes=common.size_bytes,
        record_count=common.record_count,
        schema_name=common.schema_name,
        schema_version=common.schema_version,
        times=common.times,
        universe_snapshot_sha256=common.universe_snapshot_sha256,
        lineage=common.lineage,
    )


def _decode_partition_payload(payload: Mapping[str, object]) -> PartitionManifest:
    _require_exact_keys(
        payload,
        _COMMON_PAYLOAD_FIELDS
        | {"dataset_name", "partition_key", "source_manifest_ids"},
        "partition payload",
    )
    identifiers = payload["source_manifest_ids"]
    if not isinstance(identifiers, list) or not all(
        isinstance(identifier, str) for identifier in identifiers
    ):
        message = "source_manifest_ids must be a string array"
        raise StorageError(StorageErrorCode.MANIFEST_CORRUPT, message)
    common = _decode_common_payload(payload)
    return PartitionManifest(
        dataset_name=_require_str(payload["dataset_name"], "dataset_name"),
        partition_key=_require_str(payload["partition_key"], "partition_key"),
        source_manifest_ids=tuple(identifiers),
        storage_path=common.storage_path,
        object_sha256=common.object_sha256,
        size_bytes=common.size_bytes,
        record_count=common.record_count,
        schema_name=common.schema_name,
        schema_version=common.schema_version,
        times=common.times,
        universe_snapshot_sha256=common.universe_snapshot_sha256,
        lineage=common.lineage,
    )


def decode_manifest(encoded: bytes) -> DataManifest:
    """Strictly deserialize and authenticate one source or partition manifest."""
    if not isinstance(encoded, bytes) or len(encoded) > MAX_MANIFEST_BYTES:
        raise StorageError(
            StorageErrorCode.MANIFEST_CORRUPT,
            "manifest encoding is invalid or oversized",
        )
    try:
        decoded: object = json.loads(encoded)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        message = "manifest is malformed or incomplete"
        raise StorageError(StorageErrorCode.MANIFEST_CORRUPT, message) from error
    envelope = _require_mapping(decoded, "manifest")
    _require_exact_keys(
        envelope,
        frozenset(
            {
                "manifest_id",
                "manifest_schema_version",
                "manifest_sha256",
                "manifest_type",
                "payload",
            }
        ),
        "manifest",
    )
    if envelope["manifest_schema_version"] != MANIFEST_SCHEMA_VERSION:
        message = "manifest schema version is unsupported"
        raise StorageError(StorageErrorCode.MANIFEST_CORRUPT, message)
    kind = _require_str(envelope["manifest_type"], "manifest_type")
    if kind not in {"SOURCE", "PARTITION"}:
        message = "manifest type is unsupported"
        raise StorageError(StorageErrorCode.MANIFEST_CORRUPT, message)
    payload = _require_mapping(envelope["payload"], "payload")
    body = {
        "manifest_schema_version": MANIFEST_SCHEMA_VERSION,
        "manifest_type": kind,
        "payload": payload,
    }
    digest = _sha256_hex(_canonical_bytes(body))
    manifest_id = _require_str(envelope["manifest_id"], "manifest_id")
    manifest_sha256 = _require_str(envelope["manifest_sha256"], "manifest_sha256")
    expected_id = f"{kind.lower()}-{digest}"
    if manifest_id != expected_id or manifest_sha256 != digest:
        message = "manifest identity or hash mismatch"
        raise StorageError(StorageErrorCode.HASH_MISMATCH, message)
    manifest: DataManifest
    if kind == "SOURCE":
        manifest = _decode_source_payload(payload)
    else:
        manifest = _decode_partition_payload(payload)
    if manifest.manifest_id != manifest_id:
        message = "decoded manifest identity mismatch"
        raise StorageError(StorageErrorCode.HASH_MISMATCH, message)
    return manifest


def deterministic_dataset_id(
    partition_manifest_ids: Sequence[str],
    *,
    universe_snapshot_sha256: str,
    calendar_version: str,
    schema_version: str,
) -> str:
    """Derive a stable dataset ID from sorted immutable partition identities."""
    if not partition_manifest_ids or len(set(partition_manifest_ids)) != len(
        partition_manifest_ids
    ):
        raise StorageError(
            StorageErrorCode.MANIFEST_CORRUPT,
            "partition manifest identities must be nonempty and unique",
        )
    for identifier in partition_manifest_ids:
        _validate_text(identifier, "partition manifest identity")
    _validate_sha256(universe_snapshot_sha256, "universe_snapshot_sha256")
    _validate_text(calendar_version, "calendar_version")
    _validate_text(schema_version, "schema_version")
    payload = {
        "calendar_version": calendar_version,
        "partition_manifest_ids": sorted(partition_manifest_ids),
        "schema_version": schema_version,
        "universe_snapshot_sha256": universe_snapshot_sha256,
    }
    return f"dataset-{_sha256_hex(_canonical_bytes(payload))}"


@dataclass(frozen=True, slots=True)
class VerificationIssue:
    """One bounded manifest or object verification failure."""

    code: str
    relative_path: str
    detail: str

    def to_dict(self) -> dict[str, str]:
        """Return a stable issue record."""
        return {
            "code": self.code,
            "detail": self.detail,
            "relative_path": self.relative_path,
        }


@dataclass(frozen=True, slots=True)
class VerificationReport:
    """Complete local manifest verification result."""

    checked_manifests: int
    checked_objects: int
    verified_bytes: int
    errors: tuple[VerificationIssue, ...]

    @property
    def passed(self) -> bool:
        """Return true only when every discovered manifest and object verifies."""
        return not self.errors

    def to_dict(self) -> dict[str, object]:
        """Return a machine-readable verification report."""
        return {
            "checked_manifests": self.checked_manifests,
            "checked_objects": self.checked_objects,
            "errors": [error.to_dict() for error in self.errors],
            "passed": self.passed,
            "verified": _quantity(self.verified_bytes),
        }


@dataclass(frozen=True, slots=True)
class CleanupCandidate:
    """Operator-reviewed candidate; it never represents an executed deletion."""

    relative_path: str
    size_bytes: int
    reason: str
    automatic: bool = False
    operator_approval_required: bool = True

    def to_dict(self) -> dict[str, object]:
        """Return a machine-readable cleanup candidate."""
        return {
            "automatic": self.automatic,
            "operator_approval_required": self.operator_approval_required,
            "reason": self.reason,
            "relative_path": self.relative_path,
            "size": _quantity(self.size_bytes),
        }


@dataclass(frozen=True, slots=True)
class CleanupPlan:
    """Non-mutating storage reduction proposal."""

    target_bytes: int
    current_bytes: int
    required_reduction_bytes: int
    planned_reduction_bytes: int
    candidates: tuple[CleanupCandidate, ...]

    def to_dict(self) -> dict[str, object]:
        """Return the explicit no-deletion plan."""
        return {
            "automatic_deletion_performed": False,
            "candidates": [candidate.to_dict() for candidate in self.candidates],
            "current": _quantity(self.current_bytes),
            "planned_reduction": _quantity(self.planned_reduction_bytes),
            "required_reduction": _quantity(self.required_reduction_bytes),
            "target": _quantity(self.target_bytes),
        }


def _cleanup_priority(item: FileInventory) -> tuple[int, str]:
    ranks = {
        "quarantine": 2,
        "reports": 3,
        "models": 4,
        "derived": 5,
        "datasets": 5,
    }
    if item.temporary:
        rank = 0
    elif item.partial:
        rank = 1
    else:
        rank = ranks.get(item.area, 6)
    return rank, item.relative_path


def _cleanup_reason(item: FileInventory) -> str:
    if item.temporary:
        return "INTERRUPTED_OR_TEMPORARY_REVIEW"
    if item.partial:
        return "PARTIAL_OBJECT_REVIEW"
    if item.area == "quarantine":
        return "QUARANTINE_RETENTION_REVIEW"
    return "IMMUTABLE_RETENTION_REVIEW"


@dataclass(frozen=True, slots=True)
class StorageAuditRecord:
    """Structured evidence envelope for data-root operations."""

    operation: str
    outcome: str
    observed_at_wall_utc: str
    data_root: str
    policy_sha256: str
    evidence_sha256: str
    reason_codes: tuple[str, ...]

    def __post_init__(self) -> None:
        """Validate bounded audit metadata and cryptographic identities."""
        _validate_text(self.operation, "audit operation")
        if self.outcome not in {"SUCCEEDED", "REJECTED"}:
            raise StorageError(
                StorageErrorCode.INVALID_REQUEST, "audit outcome is invalid"
            )
        _validate_utc(self.observed_at_wall_utc, "audit observed_at_wall_utc")
        _validate_text(self.data_root, "audit data_root", MAX_PATH_BYTES)
        _validate_sha256(self.policy_sha256, "policy_sha256")
        _validate_sha256(self.evidence_sha256, "evidence_sha256")
        for reason in self.reason_codes:
            _validate_text(reason, "audit reason code", 80)

    @property
    def audit_id(self) -> str:
        """Return the deterministic audit-record identity."""
        return f"storage-audit-{_sha256_hex(_canonical_bytes(self._payload_dict()))}"

    def _payload_dict(self) -> dict[str, object]:
        return {
            "automatic_deletion_performed": False,
            "data_root": self.data_root,
            "evidence_sha256": self.evidence_sha256,
            "live_trading_capable": False,
            "network_access_performed": False,
            "observed_at_wall_utc": self.observed_at_wall_utc,
            "operation": self.operation,
            "outcome": self.outcome,
            "policy_sha256": self.policy_sha256,
            "reason_codes": list(self.reason_codes),
            "schema_version": AUDIT_SCHEMA_VERSION,
        }

    def to_dict(self) -> dict[str, object]:
        """Return a content-identified, explicitly non-trading audit record."""
        return {"audit_id": self.audit_id, **self._payload_dict()}


@dataclass(frozen=True, slots=True)
class StorageAuditContext:
    """Stable context separated from a command's variable evidence payload."""

    operation: str
    outcome: str
    data_root: Path
    policy_sha256: str
    reason_codes: tuple[str, ...] = ()


def make_audit_record(
    context: StorageAuditContext,
    evidence: object,
    *,
    observed_at_wall_utc: str | None = None,
) -> StorageAuditRecord:
    """Create a content-bound structured audit record."""
    timestamp = observed_at_wall_utc or datetime.now(UTC).isoformat()
    return StorageAuditRecord(
        operation=context.operation,
        outcome=context.outcome,
        observed_at_wall_utc=timestamp,
        data_root=str(context.data_root),
        policy_sha256=context.policy_sha256,
        evidence_sha256=_sha256_hex(_canonical_bytes(evidence)),
        reason_codes=context.reason_codes,
    )


class AdmissionLease:
    """Process-local handle retaining the interprocess writer fence."""

    def __init__(
        self,
        *,
        root: Path,
        file_descriptor: int,
        decision: AdmissionDecision,
    ) -> None:
        """Take ownership of an already locked descriptor."""
        self._root = root
        self._file_descriptor = file_descriptor
        self.decision = decision

    @property
    def active(self) -> bool:
        """Return whether this lease still owns the writer fence."""
        return self._file_descriptor >= 0

    @property
    def root(self) -> Path:
        """Return the exact repository root fenced by this lease."""
        return self._root

    def close(self) -> None:
        """Release the writer fence idempotently."""
        if not self.active:
            return
        descriptor = self._file_descriptor
        self._file_descriptor = -1
        fcntl.flock(descriptor, fcntl.LOCK_UN)
        os.close(descriptor)

    def __enter__(self) -> Self:
        """Return an active lease for use as a context manager."""
        if not self.active:
            raise StorageError(StorageErrorCode.ADMISSION_DENIED, "lease is inactive")
        return self

    def __exit__(self, *_unused: object) -> None:
        """Release the writer fence when leaving a context."""
        self.close()


class DataRepository:
    """Bounded local repository with fail-closed accounting and publication."""

    def __init__(
        self,
        root: Path = DEFAULT_DATA_ROOT,
        *,
        policy: StoragePolicy | None = None,
        filesystem_probe: Callable[[Path], FileSystemState] | None = None,
        git_worktree: Path | None = _PROJECT_ROOT,
    ) -> None:
        """Bind a policy and capacity probe to one absolute external root."""
        if not root.is_absolute():
            raise StorageError(
                StorageErrorCode.INVALID_PATH, "data root must be an absolute path"
            )
        self.root = root.absolute()
        self.policy = policy or StoragePolicy()
        self._filesystem_probe = filesystem_probe or self._probe_filesystem
        self._git_worktree = (
            git_worktree.absolute() if git_worktree is not None else None
        )

    def initialize(self) -> None:
        """Create or validate the fixed storage layout without deleting anything."""
        self._initialize_root()
        for area in STORAGE_AREAS:
            self._initialize_area(area)
        self._initialize_marker()
        self._initialize_lock()

    def migrate_policy_v2_to_v3(  # noqa: C901, PLR0912, PLR0915
        self, *, expected_current_policy_sha256: str, execute: bool = False
    ) -> dict[str, object]:
        """Plan or perform the single supported authenticated policy migration."""
        _validate_sha256(
            expected_current_policy_sha256, "expected current policy SHA-256"
        )
        if expected_current_policy_sha256 != LEGACY_STORAGE_POLICY_V2_SHA256:
            raise StorageError(
                StorageErrorCode.INVALID_POLICY,
                "only the reviewed storage-policy v2 identity can be migrated",
            )
        if not self.root.exists() or self.root.is_symlink():
            raise StorageError(
                StorageErrorCode.INVALID_PATH,
                "policy migration requires an existing non-symlink data root",
            )
        self._initialize_root()
        for area in STORAGE_AREAS:
            path = self.root / area
            if not path.exists() or path.is_symlink():
                raise StorageError(
                    StorageErrorCode.INVALID_PATH,
                    f"policy migration requires existing storage area {area}",
                )
            self._initialize_area(area)

        marker = self.root / ".aegis-data-root.json"
        legacy_marker = (
            _canonical_bytes(
                {
                    "data_root_kind": "AEGIS_MX_BOUNDED_FORECASTING_POC",
                    "policy_sha256": LEGACY_STORAGE_POLICY_V2_SHA256,
                    "schema_version": "2.0.0",
                }
            )
            + b"\n"
        )
        current_marker = (
            _canonical_bytes(
                {
                    "data_root_kind": "AEGIS_MX_BOUNDED_FORECASTING_POC",
                    "policy_sha256": self.policy.sha256,
                    "schema_version": DATA_REPOSITORY_SCHEMA_VERSION,
                }
            )
            + b"\n"
        )
        migration_identity = {
            "from_marker_sha256": _sha256_hex(legacy_marker),
            "from_policy_sha256": LEGACY_STORAGE_POLICY_V2_SHA256,
            "schema_version": POLICY_MIGRATION_SCHEMA_VERSION,
            "to_marker_sha256": _sha256_hex(current_marker),
            "to_policy_sha256": self.policy.sha256,
        }
        migration_sha256 = _sha256_hex(_canonical_bytes(migration_identity))
        migration_id = f"policy-migration-{migration_sha256}"
        history_relative = (
            f"manifests/policy-history/marker-{LEGACY_STORAGE_POLICY_V2_SHA256}.json"
        )
        record_relative = f"manifests/policy-history/{migration_id}.json"

        def result(*, migrated: bool, already_current: bool) -> dict[str, object]:
            return {
                "already_current": already_current,
                "execute_requested": execute,
                "from_policy_sha256": LEGACY_STORAGE_POLICY_V2_SHA256,
                "legacy_marker_archive": history_relative,
                "migrated": migrated,
                "migration_id": migration_id,
                "migration_record": record_relative,
                "schema_version": POLICY_MIGRATION_SCHEMA_VERSION,
                "to_policy": self.policy.to_dict(),
                "to_policy_sha256": self.policy.sha256,
            }

        lock_descriptor = self._acquire_lock()
        try:
            observed = self._read_secure(marker, MAX_MANIFEST_BYTES)
            if observed == current_marker:
                return result(migrated=False, already_current=True)
            if observed != legacy_marker:
                raise StorageError(
                    StorageErrorCode.INVALID_POLICY,
                    "data-root marker does not match the reviewed v2 policy",
                )
            if not execute:
                return result(migrated=False, already_current=False)

            history_directory = self.root / "manifests" / "policy-history"
            self._assert_safe_path(history_directory)
            if history_directory.exists():
                if not history_directory.is_dir():
                    raise StorageError(
                        StorageErrorCode.INVALID_PATH,
                        "policy history path is not a directory",
                    )
            else:
                history_directory.mkdir(mode=0o700)
                self._sync_directory(history_directory.parent)

            history_path = self.root.joinpath(*PurePosixPath(history_relative).parts)
            if history_path.exists() or history_path.is_symlink():
                if self._read_secure(history_path, MAX_MANIFEST_BYTES) != legacy_marker:
                    raise StorageError(
                        StorageErrorCode.IMMUTABLE_CONFLICT,
                        "legacy policy-marker archive conflicts",
                    )
            else:
                self._create_exclusive_file(history_path, legacy_marker, 0o400)

            migration_record = {
                **migration_identity,
                "migration_id": migration_id,
                "migration_sha256": migration_sha256,
                "network_access_performed": False,
                "original_marker_archived": True,
            }
            record_bytes = _canonical_bytes(migration_record) + b"\n"
            record_path = self.root.joinpath(*PurePosixPath(record_relative).parts)
            if record_path.exists() or record_path.is_symlink():
                if self._read_secure(record_path, MAX_MANIFEST_BYTES) != record_bytes:
                    raise StorageError(
                        StorageErrorCode.IMMUTABLE_CONFLICT,
                        "policy migration record conflicts",
                    )
            else:
                self._create_exclusive_file(record_path, record_bytes, 0o400)
            self._sync_directory(history_directory)

            staged_marker = self.root / ".aegis-data-root.json.v3.part"
            if staged_marker.exists() or staged_marker.is_symlink():
                raise StorageError(
                    StorageErrorCode.INTERRUPTED_PUBLICATION,
                    "staged policy marker already exists",
                )
            self._create_exclusive_file(staged_marker, current_marker, 0o400)
            if self._read_secure(marker, MAX_MANIFEST_BYTES) != legacy_marker:
                raise StorageError(
                    StorageErrorCode.INVALID_POLICY,
                    "data-root marker changed during migration",
                )
            try:
                staged_marker.replace(marker)
                self._sync_directory(self.root)
            except OSError as error:
                raise StorageError(
                    StorageErrorCode.IO_FAILURE,
                    "policy marker replacement failed",
                ) from error
            return result(migrated=True, already_current=False)
        finally:
            fcntl.flock(lock_descriptor, fcntl.LOCK_UN)
            os.close(lock_descriptor)

    def _initialize_root(self) -> None:
        if self._git_worktree is not None:
            try:
                resolved_root = self.root.resolve(strict=False)
                resolved_worktree = self._git_worktree.resolve(strict=False)
            except (OSError, RuntimeError) as error:
                raise StorageError(
                    StorageErrorCode.INVALID_PATH,
                    "data-root ancestry cannot be resolved safely",
                ) from error
            if (
                self.root == self._git_worktree
                or self._git_worktree in self.root.parents
                or resolved_root == resolved_worktree
                or resolved_worktree in resolved_root.parents
            ):
                raise StorageError(
                    StorageErrorCode.INVALID_PATH,
                    "data root cannot be inside the Git worktree",
                )
        if self.root.exists() or self.root.is_symlink():
            root_status = os.lstat(self.root)
            if stat.S_ISLNK(root_status.st_mode):
                raise StorageError(
                    StorageErrorCode.SYMLINK_DETECTED, "data root cannot be a symlink"
                )
            if not stat.S_ISDIR(root_status.st_mode):
                raise StorageError(
                    StorageErrorCode.INVALID_PATH, "data root must be a directory"
                )
        else:
            self.root.mkdir(parents=True, mode=0o700)

    def _initialize_area(self, area: str) -> None:
        path = self.root / area
        if not path.exists() and not path.is_symlink():
            path.mkdir(mode=0o700)
            return
        status = os.lstat(path)
        if stat.S_ISLNK(status.st_mode):
            raise StorageError(
                StorageErrorCode.SYMLINK_DETECTED,
                f"storage area {area} cannot be a symlink",
            )
        if not stat.S_ISDIR(status.st_mode):
            raise StorageError(
                StorageErrorCode.INVALID_PATH,
                f"storage area {area} is not a directory",
            )

    def _initialize_marker(self) -> None:
        marker_payload = {
            "data_root_kind": "AEGIS_MX_BOUNDED_FORECASTING_POC",
            "policy_sha256": self.policy.sha256,
            "schema_version": DATA_REPOSITORY_SCHEMA_VERSION,
        }
        marker = self.root / ".aegis-data-root.json"
        encoded = _canonical_bytes(marker_payload) + b"\n"
        if marker.exists() or marker.is_symlink():
            if self._read_secure(marker, MAX_MANIFEST_BYTES) != encoded:
                raise StorageError(
                    StorageErrorCode.INVALID_POLICY,
                    "data-root policy marker is malformed or incompatible",
                )
        else:
            self._create_exclusive_file(marker, encoded, 0o400)
            self._sync_directory(self.root)

    def _initialize_lock(self) -> None:
        lock_path = self.root / ".admission.lock"
        if not lock_path.exists():
            descriptor = os.open(
                lock_path,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
                0o600,
            )
            os.close(descriptor)
        self._validate_regular_file(lock_path, allow_empty=True)

    def usage(self) -> StorageUsage:
        """Safely scan logical bytes, including sparse, partial, and temp files."""
        inventory = tuple(self._inventory())
        committed = 0
        partial = 0
        temporary = 0
        allocated = 0
        partial_count = 0
        temporary_count = 0
        try:
            for item in inventory:
                allocated = _checked_add(allocated, item.allocated_bytes)
                if item.temporary:
                    temporary = _checked_add(temporary, item.logical_bytes)
                    temporary_count += 1
                elif item.partial:
                    partial = _checked_add(partial, item.logical_bytes)
                    partial_count += 1
                else:
                    committed = _checked_add(committed, item.logical_bytes)
            logical = _checked_add(committed, partial, temporary)
        except OverflowError as error:
            raise StorageError(
                StorageErrorCode.IO_FAILURE,
                "data-root usage exceeds signed 64-bit range",
            ) from error
        try:
            filesystem = self._filesystem_probe(self.root)
        except (OSError, StorageError) as error:
            if (
                isinstance(error, StorageError)
                and error.code is StorageErrorCode.FILESYSTEM_UNKNOWN
            ):
                raise
            raise StorageError(
                StorageErrorCode.FILESYSTEM_UNKNOWN,
                "filesystem capacity could not be established",
            ) from error
        return StorageUsage(
            committed_bytes=committed,
            partial_bytes=partial,
            temporary_bytes=temporary,
            logical_bytes=logical,
            allocated_bytes=allocated,
            file_count=len(inventory),
            partial_file_count=partial_count,
            temporary_file_count=temporary_count,
            filesystem=filesystem,
            inventory=inventory,
        )

    def estimate(
        self, request: StorageRequest, quota: QuotaEvidence
    ) -> AdmissionDecision:
        """Calculate one serialized admission decision and release the fence."""
        descriptor = self._acquire_lock()
        try:
            return self._calculate_admission(request, quota)
        finally:
            fcntl.flock(descriptor, fcntl.LOCK_UN)
            os.close(descriptor)

    def acquire_admission(
        self, request: StorageRequest, quota: QuotaEvidence
    ) -> AdmissionLease:
        """Acquire the single-writer fence and retain it for an admitted write."""
        descriptor = self._acquire_lock()
        try:
            decision = self._calculate_admission(request, quota)
        except Exception:
            fcntl.flock(descriptor, fcntl.LOCK_UN)
            os.close(descriptor)
            raise
        if not decision.admitted:
            fcntl.flock(descriptor, fcntl.LOCK_UN)
            os.close(descriptor)
            reasons = ",".join(reason.value for reason in decision.reasons)
            message = f"storage admission denied: {reasons}"
            raise StorageError(StorageErrorCode.ADMISSION_DENIED, message)
        return AdmissionLease(
            root=self.root, file_descriptor=descriptor, decision=decision
        )

    def manifest_path(self, manifest: DataManifest) -> Path:
        """Return the canonical publication path for a manifest."""
        kind = "source" if isinstance(manifest, SourceManifest) else "partition"
        return self.root / "manifests" / kind / f"{manifest.manifest_id}.json"

    def publish_manifest(
        self,
        manifest: DataManifest,
        lease: AdmissionLease,
        *,
        fault_injector: Callable[[PublicationStage], None] | None = None,
    ) -> Path:
        """Verify and atomically publish an immutable manifest without replacement."""
        encoded = self._prepare_manifest_publication(manifest, lease)
        final_path = self.manifest_path(manifest)
        final_path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        self._assert_safe_path(final_path.parent)
        existing = self._check_existing_manifest(final_path, encoded)
        if existing is not None:
            return existing
        return self._publish_new_manifest(
            manifest,
            encoded,
            final_path,
            fault_injector,
        )

    def publish_staged_object(
        self,
        staged_relative_path: str,
        final_relative_path: str,
        *,
        expected_sha256: str,
        expected_size_bytes: int,
        lease: AdmissionLease,
    ) -> Path:
        """Atomically publish one verified staged object under an active lease.

        Staging is restricted to ``tmp/`` and final publication to a declared
        immutable object area (never ``tmp/`` or ``manifests/``). An identical
        existing object makes the operation idempotent; conflicting bytes are
        never replaced.
        """
        if not lease.active or lease.root != self.root:
            raise StorageError(
                StorageErrorCode.ADMISSION_DENIED,
                "object publication requires an active admission lease",
            )
        staged_relative = _validate_relative_path(
            staged_relative_path, allowed_areas=frozenset({"tmp"})
        )
        final_relative = _validate_relative_path(
            final_relative_path,
            allowed_areas=OBJECT_AREAS,
        )
        _validate_sha256(expected_sha256, "expected_sha256")
        _validate_byte_count(expected_size_bytes, "expected_size_bytes")
        staged_path = self.root.joinpath(*staged_relative.parts)
        final_path = self.root.joinpath(*final_relative.parts)
        self._assert_safe_path(staged_path)
        self._assert_safe_path(final_path)
        staged_status = self._validate_regular_file(staged_path)
        if staged_status.st_size != expected_size_bytes:
            raise StorageError(
                StorageErrorCode.SIZE_MISMATCH,
                "staged object size does not match the expected size",
            )
        if self._hash_secure_file(staged_path) != expected_sha256:
            raise StorageError(
                StorageErrorCode.HASH_MISMATCH,
                "staged object SHA-256 does not match the expected digest",
            )

        final_path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        self._assert_safe_path(final_path.parent)
        if final_path.exists() or final_path.is_symlink():
            final_status = self._validate_regular_file(final_path)
            if (
                final_status.st_size != expected_size_bytes
                or self._hash_secure_file(final_path) != expected_sha256
            ):
                raise StorageError(
                    StorageErrorCode.IMMUTABLE_CONFLICT,
                    "an immutable object path already contains different bytes",
                )
            staged_path.unlink()
            self._sync_directory(staged_path.parent)
            return final_path

        try:
            os.link(staged_path, final_path, follow_symlinks=False)
            self._sync_directory(final_path.parent)
            staged_path.unlink()
            self._sync_directory(staged_path.parent)
        except FileExistsError as error:
            raise StorageError(
                StorageErrorCode.IMMUTABLE_CONFLICT,
                "an immutable object appeared concurrently and was not replaced",
            ) from error
        except OSError as error:
            raise StorageError(
                StorageErrorCode.INTERRUPTED_PUBLICATION,
                "object publication was interrupted",
            ) from error
        return final_path

    def _prepare_manifest_publication(
        self, manifest: DataManifest, lease: AdmissionLease
    ) -> bytes:
        if not lease.active or lease.root != self.root:
            raise StorageError(
                StorageErrorCode.ADMISSION_DENIED,
                "manifest publication requires an active admission lease",
            )
        encoded = encode_manifest(manifest)
        output_budget = lease.decision.request.output_bytes
        if output_budget is None or len(encoded) > output_budget:
            raise StorageError(
                StorageErrorCode.ADMISSION_DENIED,
                "manifest encoding exceeds the admitted output budget",
            )
        self._verify_manifest_object(manifest)
        if isinstance(manifest, PartitionManifest):
            for source_id in manifest.source_manifest_ids:
                self.load_manifest(source_id)
        return encoded

    def _check_existing_manifest(self, final_path: Path, encoded: bytes) -> Path | None:
        if final_path.exists() or final_path.is_symlink():
            if self._read_secure(final_path, MAX_MANIFEST_BYTES) == encoded:
                return final_path
            raise StorageError(
                StorageErrorCode.IMMUTABLE_CONFLICT,
                "an immutable manifest path already contains different bytes",
            )
        return None

    def _publish_new_manifest(
        self,
        manifest: DataManifest,
        encoded: bytes,
        final_path: Path,
        fault_injector: Callable[[PublicationStage], None] | None,
    ) -> Path:
        temporary_path = self.root / "tmp" / f"{manifest.manifest_id}.manifest.partial"
        if temporary_path.exists() or temporary_path.is_symlink():
            raise StorageError(
                StorageErrorCode.INTERRUPTED_PUBLICATION,
                "a prior interrupted manifest publication requires operator recovery",
            )
        try:
            self._create_exclusive_file(temporary_path, encoded, 0o400)
            if fault_injector is not None:
                fault_injector(PublicationStage.TEMP_FILE_SYNCED)
            try:
                os.link(temporary_path, final_path, follow_symlinks=False)
            except FileExistsError as error:
                raise StorageError(
                    StorageErrorCode.IMMUTABLE_CONFLICT,
                    "manifest appeared concurrently and was not replaced",
                ) from error
            self._sync_directory(final_path.parent)
            if fault_injector is not None:
                fault_injector(PublicationStage.MANIFEST_LINKED)
            temporary_path.unlink()
            self._sync_directory(temporary_path.parent)
        except StorageError:
            raise
        except OSError as error:
            message = f"manifest publication was interrupted: {error}"
            raise StorageError(
                StorageErrorCode.INTERRUPTED_PUBLICATION, message
            ) from error
        return final_path

    def load_manifest(self, manifest_id: str) -> DataManifest:
        """Load and authenticate one manifest by exact content-derived identity."""
        _validate_manifest_id(
            manifest_id,
            "manifest_id",
            allowed_prefixes=frozenset({"source", "partition"}),
        )
        kind = manifest_id.partition("-")[0]
        path = self.root / "manifests" / kind / f"{manifest_id}.json"
        return decode_manifest(self._read_secure(path, MAX_MANIFEST_BYTES))

    def verify(self) -> VerificationReport:
        """Verify every manifest, object, source reference, and lineage edge."""
        try:
            self.usage()
        except StorageError as error:
            root_issue = VerificationIssue(error.code.value, ".", str(error))
            return VerificationReport(0, 0, 0, (root_issue,))

        paths = self._manifest_files()
        errors: list[VerificationIssue] = []
        manifests: dict[str, DataManifest] = {}
        verified_bytes = 0
        checked_objects = 0
        for path in paths:
            manifest, path_issue = self._verify_manifest_path(path)
            if path_issue is not None:
                errors.append(path_issue)
                continue
            manifest = cast("DataManifest", manifest)
            manifests[manifest.manifest_id] = manifest
            checked_objects += 1
            try:
                verified_bytes = _checked_add(verified_bytes, manifest.size_bytes)
            except OverflowError:
                relative = path.relative_to(self.root).as_posix()
                errors.append(
                    VerificationIssue(
                        StorageErrorCode.IO_FAILURE.value,
                        relative,
                        "verified object byte count exceeds signed 64-bit range",
                    )
                )
        errors.extend(self._reference_issues(manifests))
        return VerificationReport(
            len(paths),
            checked_objects,
            verified_bytes,
            tuple(sorted(errors, key=lambda item: (item.relative_path, item.code))),
        )

    def _manifest_files(self) -> tuple[Path, ...]:
        paths: list[Path] = []
        for kind in ("source", "partition"):
            directory = self.root / "manifests" / kind
            if directory.exists():
                paths.extend(directory.iterdir())
        return tuple(sorted(paths, key=lambda path: path.as_posix()))

    def _verify_manifest_path(
        self, path: Path
    ) -> tuple[DataManifest | None, VerificationIssue | None]:
        relative = path.relative_to(self.root).as_posix()
        try:
            manifest = decode_manifest(self._read_secure(path, MAX_MANIFEST_BYTES))
            self._validate_manifest_filename(path, manifest)
            self._verify_manifest_object(manifest)
        except (OSError, StorageError) as error:
            code = (
                error.code.value
                if isinstance(error, StorageError)
                else StorageErrorCode.IO_FAILURE.value
            )
            return None, VerificationIssue(code, relative, str(error))
        return manifest, None

    @staticmethod
    def _validate_manifest_filename(path: Path, manifest: DataManifest) -> None:
        if path.name != f"{manifest.manifest_id}.json":
            message = "manifest filename does not match its identity"
            raise StorageError(StorageErrorCode.MANIFEST_CORRUPT, message)

    def _reference_issues(
        self, manifests: Mapping[str, DataManifest]
    ) -> list[VerificationIssue]:
        errors: list[VerificationIssue] = []
        manifest_ids = frozenset(manifests)
        for manifest in manifests.values():
            references: list[str] = []
            if manifest.lineage.parent_manifest_id is not None:
                references.append(manifest.lineage.parent_manifest_id)
            if isinstance(manifest, PartitionManifest):
                references.extend(manifest.source_manifest_ids)
            for reference in references:
                if reference not in manifest_ids:
                    path = (
                        self.manifest_path(manifest).relative_to(self.root).as_posix()
                    )
                    errors.append(
                        VerificationIssue(
                            StorageErrorCode.MISSING_LINEAGE.value,
                            path,
                            f"referenced manifest is absent: {reference}",
                        )
                    )
        return errors

    def cleanup_plan(self, *, target_bytes: int | None = None) -> CleanupPlan:
        """Create a deterministic operator review list without deleting data."""
        target = self.policy.target_root_bytes if target_bytes is None else target_bytes
        _validate_byte_count(target, "cleanup target_bytes")
        usage = self.usage()
        required = max(usage.total_bytes - target, 0)
        if required == 0:
            return CleanupPlan(target, usage.total_bytes, 0, 0, ())

        candidates: list[CleanupCandidate] = []
        planned = 0
        for item in sorted(usage.inventory, key=_cleanup_priority):
            if item.area not in OBJECT_AREAS and not item.temporary:
                continue
            candidates.append(
                CleanupCandidate(
                    item.relative_path, item.logical_bytes, _cleanup_reason(item)
                )
            )
            planned = _checked_add(planned, item.logical_bytes)
            if planned >= required:
                break
        return CleanupPlan(
            target,
            usage.total_bytes,
            required,
            planned,
            tuple(candidates),
        )

    def _calculate_admission(
        self, request: StorageRequest, quota: QuotaEvidence
    ) -> AdmissionDecision:
        usage = self.usage()
        root = _root_projection(request, usage)
        effective_quota, projected_quota, quota_reason = _quota_projection(
            quota, self.policy, root.increment_bytes
        )
        projected_free, filesystem_reason = _filesystem_projection(
            usage.filesystem, root.increment_bytes
        )
        initial_reasons = tuple(
            reason
            for reason in (root.reason, quota_reason, filesystem_reason)
            if reason is not None
        )
        policy_reasons = _limit_reasons(
            self.policy,
            root,
            projected_free,
            (effective_quota, projected_quota),
        )
        reasons = tuple(dict.fromkeys((*initial_reasons, *policy_reasons)))
        return AdmissionDecision(
            admitted=not reasons,
            reasons=reasons,
            request=request,
            usage=usage,
            quota=quota,
            effective_quota_limit_bytes=effective_quota,
            projected_root_peak_bytes=root.root_peak_bytes,
            projected_temporary_peak_bytes=root.temporary_peak_bytes,
            projected_quota_used_bytes=projected_quota,
            projected_filesystem_free_bytes=projected_free,
            policy_sha256=self.policy.sha256,
        )

    def _acquire_lock(self) -> int:
        path = self.root / ".admission.lock"
        try:
            descriptor = os.open(
                path,
                os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW,
            )
        except OSError as error:
            code = (
                StorageErrorCode.SYMLINK_DETECTED
                if error.errno == errno.ELOOP
                else StorageErrorCode.IO_FAILURE
            )
            raise StorageError(code, "admission lock is unavailable") from error
        status = os.fstat(descriptor)
        if not stat.S_ISREG(status.st_mode) or status.st_nlink != 1:
            os.close(descriptor)
            raise StorageError(
                StorageErrorCode.HARDLINK_DETECTED,
                "admission lock is not a unique regular file",
            )
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            os.close(descriptor)
            raise StorageError(
                StorageErrorCode.CONCURRENT_ADMISSION,
                "concurrent storage admission is already active",
            ) from error
        return descriptor

    def _inventory(self) -> Iterator[FileInventory]:
        root_status = self.root.stat(follow_symlinks=False)
        root_device = root_status.st_dev

        def walk(directory: Path) -> Iterator[FileInventory]:
            try:
                entries = sorted(os.scandir(directory), key=lambda entry: entry.name)
            except OSError as error:
                raise StorageError(
                    StorageErrorCode.IO_FAILURE,
                    f"cannot scan data-root directory {directory}",
                ) from error
            for entry in entries:
                path = Path(entry.path)
                relative = path.relative_to(self.root).as_posix()
                status = entry.stat(follow_symlinks=False)
                if stat.S_ISLNK(status.st_mode):
                    raise StorageError(
                        StorageErrorCode.SYMLINK_DETECTED,
                        f"symlink is forbidden in data root: {relative}",
                    )
                if status.st_dev != root_device:
                    raise StorageError(
                        StorageErrorCode.INVALID_PATH,
                        f"cross-device entry is forbidden in data root: {relative}",
                    )
                if stat.S_ISDIR(status.st_mode):
                    yield from walk(path)
                    continue
                if not stat.S_ISREG(status.st_mode):
                    raise StorageError(
                        StorageErrorCode.UNSUPPORTED_FILE,
                        f"unsupported file type in data root: {relative}",
                    )
                if status.st_nlink != 1:
                    raise StorageError(
                        StorageErrorCode.HARDLINK_DETECTED,
                        f"hard link makes accounting ambiguous: {relative}",
                    )
                repeated = os.lstat(path)
                if (
                    repeated.st_dev,
                    repeated.st_ino,
                    repeated.st_size,
                    repeated.st_mtime_ns,
                ) != (
                    status.st_dev,
                    status.st_ino,
                    status.st_size,
                    status.st_mtime_ns,
                ):
                    raise StorageError(
                        StorageErrorCode.IO_FAILURE,
                        f"file changed during accounting: {relative}",
                    )
                parts = PurePosixPath(relative).parts
                area = parts[0] if parts[0] in STORAGE_AREAS else "internal"
                temporary = area == "tmp"
                partial = not temporary and path.name.endswith(PARTIAL_SUFFIXES)
                allocated_bytes = max(int(getattr(status, "st_blocks", 0)) * 512, 0)
                yield FileInventory(
                    relative,
                    status.st_size,
                    allocated_bytes,
                    area,
                    partial,
                    temporary,
                )

        yield from walk(self.root)

    def _verify_manifest_object(self, manifest: DataManifest) -> None:
        path = self._object_path(manifest.storage_path)
        status = self._validate_regular_file(path)
        if status.st_size != manifest.size_bytes:
            raise StorageError(
                StorageErrorCode.SIZE_MISMATCH,
                f"object size does not match manifest: {manifest.storage_path}",
            )
        digest = self._hash_secure_file(path)
        if digest != manifest.object_sha256:
            raise StorageError(
                StorageErrorCode.HASH_MISMATCH,
                f"object SHA-256 does not match manifest: {manifest.storage_path}",
            )

    def _object_path(self, relative_path: str) -> Path:
        path = _validate_relative_path(relative_path, allowed_areas=OBJECT_AREAS)
        candidate = self.root.joinpath(*path.parts)
        self._assert_safe_path(candidate)
        return candidate

    def _assert_safe_path(self, path: Path) -> None:
        try:
            relative = path.relative_to(self.root)
        except ValueError as error:
            raise StorageError(
                StorageErrorCode.INVALID_PATH, "path escapes the configured data root"
            ) from error
        current = self.root
        for part in relative.parts:
            current /= part
            if not current.exists() and not current.is_symlink():
                continue
            status = os.lstat(current)
            if stat.S_ISLNK(status.st_mode):
                raise StorageError(
                    StorageErrorCode.SYMLINK_DETECTED,
                    f"symlink is forbidden in data path: {relative.as_posix()}",
                )

    @staticmethod
    def _probe_filesystem(path: Path) -> FileSystemState:
        try:
            status = os.statvfs(path)
            total = _checked_add(status.f_frsize * status.f_blocks)
            free = _checked_add(status.f_frsize * status.f_bavail)
            return FileSystemState(total_bytes=total, free_bytes=free)
        except (OSError, OverflowError, StorageError) as error:
            raise StorageError(
                StorageErrorCode.FILESYSTEM_UNKNOWN,
                "statvfs capacity is unavailable or out of range",
            ) from error

    @staticmethod
    def _create_exclusive_file(path: Path, payload: bytes, mode: int) -> None:
        descriptor = os.open(
            path,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
            mode,
        )
        try:
            view = memoryview(payload)
            written = 0
            while written < len(view):
                count = os.write(descriptor, view[written:])
                if count <= 0:
                    message = "short manifest write"
                    raise OSError(message)
                written += count
            os.fsync(descriptor)
        finally:
            os.close(descriptor)

    @staticmethod
    def _sync_directory(path: Path) -> None:
        descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)

    @staticmethod
    def _validate_regular_file(
        path: Path, *, allow_empty: bool = False
    ) -> os.stat_result:
        try:
            status = os.lstat(path)
        except OSError as error:
            raise StorageError(
                StorageErrorCode.IO_FAILURE, f"required file is unavailable: {path}"
            ) from error
        if stat.S_ISLNK(status.st_mode):
            raise StorageError(
                StorageErrorCode.SYMLINK_DETECTED, f"file cannot be a symlink: {path}"
            )
        if not stat.S_ISREG(status.st_mode):
            raise StorageError(
                StorageErrorCode.UNSUPPORTED_FILE, f"path is not a regular file: {path}"
            )
        if status.st_nlink != 1:
            raise StorageError(
                StorageErrorCode.HARDLINK_DETECTED,
                f"file cannot be hard linked: {path}",
            )
        if not allow_empty and status.st_size == 0:
            raise StorageError(StorageErrorCode.SIZE_MISMATCH, f"file is empty: {path}")
        return status

    @classmethod
    def _read_secure(cls, path: Path, maximum_bytes: int) -> bytes:
        cls._validate_regular_file(path, allow_empty=True)
        try:
            descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
        except OSError as error:
            raise StorageError(
                StorageErrorCode.SYMLINK_DETECTED, f"cannot securely open file: {path}"
            ) from error
        try:
            before = os.fstat(descriptor)
            if before.st_size > maximum_bytes:
                raise StorageError(
                    StorageErrorCode.MANIFEST_CORRUPT,
                    f"file exceeds size bound: {path}",
                )
            chunks: list[bytes] = []
            remaining = before.st_size
            while remaining:
                chunk = os.read(descriptor, min(remaining, 65_536))
                if not chunk:
                    raise StorageError(
                        StorageErrorCode.IO_FAILURE, f"short read from file: {path}"
                    )
                chunks.append(chunk)
                remaining -= len(chunk)
            after = os.fstat(descriptor)
            if (
                before.st_dev,
                before.st_ino,
                before.st_size,
                before.st_mtime_ns,
            ) != (
                after.st_dev,
                after.st_ino,
                after.st_size,
                after.st_mtime_ns,
            ):
                raise StorageError(
                    StorageErrorCode.IO_FAILURE, f"file changed during read: {path}"
                )
            return b"".join(chunks)
        finally:
            os.close(descriptor)

    @classmethod
    def _hash_secure_file(cls, path: Path) -> str:
        cls._validate_regular_file(path)
        descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
        try:
            before = os.fstat(descriptor)
            digest = hashlib.sha256()
            while True:
                chunk = os.read(descriptor, 1_048_576)
                if not chunk:
                    break
                digest.update(chunk)
            after = os.fstat(descriptor)
            if (
                before.st_dev,
                before.st_ino,
                before.st_size,
                before.st_mtime_ns,
            ) != (
                after.st_dev,
                after.st_ino,
                after.st_size,
                after.st_mtime_ns,
            ):
                raise StorageError(
                    StorageErrorCode.IO_FAILURE,
                    f"file changed during SHA-256 verification: {path}",
                )
            return digest.hexdigest()
        finally:
            os.close(descriptor)
