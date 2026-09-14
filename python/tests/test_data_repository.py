"""Adversarial tests for the bounded forecasting POC data repository."""

from __future__ import annotations

import errno
import hashlib
import json
import os
import runpy
import sys
import threading
from dataclasses import replace
from pathlib import Path
from typing import TYPE_CHECKING, Any, cast

if TYPE_CHECKING:
    from collections.abc import Callable, Iterator

import pytest
from aegis_mx_research import (
    DATA_REPOSITORY_SCHEMA_VERSION,
    DECIMAL_GB,
    DEFAULT_DATA_ROOT,
    STORAGE_AREAS,
    AdmissionReason,
    DataRepository,
    FileSystemState,
    LineageRelation,
    ManifestLineage,
    ManifestTimeRange,
    PartitionManifest,
    PublicationStage,
    QuotaEvidence,
    SourceManifest,
    StorageAuditContext,
    StorageError,
    StorageErrorCode,
    StoragePolicy,
    StorageRequest,
    StorageUsage,
    VerificationIssue,
    data_cli,
    decode_manifest,
    deterministic_dataset_id,
    encode_manifest,
    make_audit_record,
)
from aegis_mx_research import data_repository as repository_module
from jsonschema import Draft202012Validator  # type: ignore[import-untyped]

NONZERO_SHA256 = "ab" * 32
UNIVERSE_SHA256 = "cd" * 32
SOURCE_A_ID = "source-" + "11" * 32
SOURCE_B_ID = "source-" + "22" * 32


def _filesystem(
    *,
    free_bytes: int = 150 * DECIMAL_GB,
    total_bytes: int = 500 * DECIMAL_GB,
) -> FileSystemState:
    return FileSystemState(total_bytes=total_bytes, free_bytes=free_bytes)


def _repository(
    tmp_path: Path,
    *,
    policy: StoragePolicy | None = None,
    filesystem: FileSystemState | None = None,
) -> DataRepository:
    state = filesystem or _filesystem()
    repository = DataRepository(
        tmp_path / "poc-data",
        policy=policy or StoragePolicy(),
        filesystem_probe=lambda _path: state,
        git_worktree=Path.cwd(),
    )
    repository.initialize()
    return repository


def _quota(*, used_bytes: int = 10 * DECIMAL_GB) -> QuotaEvidence:
    return QuotaEvidence(
        limit_bytes=250 * DECIMAL_GB,
        used_bytes=used_bytes,
        source="synthetic-authoritative-fixture",
        observed_at_utc="2026-09-09T16:00:00Z",
        authoritative=True,
    )


def _times() -> ManifestTimeRange:
    return ManifestTimeRange(
        event_time_min_ns=10,
        event_time_max_ns=20,
        publication_time_min_ns=11,
        publication_time_max_ns=21,
        receive_time_min_ns=12,
        receive_time_max_ns=22,
        processing_time_min_ns=13,
        processing_time_max_ns=23,
        revision_time_min_ns=11,
        revision_time_max_ns=21,
    )


def _source_manifest(
    *,
    storage_path: str = "raw/provider/object.bin",
    object_sha256: str = NONZERO_SHA256,
    size_bytes: int = 7,
    lineage: ManifestLineage | None = None,
) -> SourceManifest:
    return SourceManifest(
        source_name="synthetic-provider",
        source_version="fixture-v1",
        source_object_id="object-2026-09-09",
        storage_path=storage_path,
        object_sha256=object_sha256,
        size_bytes=size_bytes,
        record_count=2,
        schema_name="synthetic-minute",
        schema_version="1.0.0",
        times=_times(),
        universe_snapshot_sha256=UNIVERSE_SHA256,
        lineage=lineage or ManifestLineage(),
    )


def _write_object(repository: DataRepository, relative_path: str, data: bytes) -> Path:
    path = repository.root.joinpath(*relative_path.split("/"))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


def _resign(envelope: dict[str, Any]) -> bytes:
    body = {
        "manifest_schema_version": envelope["manifest_schema_version"],
        "manifest_type": envelope["manifest_type"],
        "payload": envelope["payload"],
    }
    encoded = json.dumps(
        body, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode("ascii")
    digest = hashlib.sha256(encoded).hexdigest()
    envelope["manifest_id"] = f"{str(envelope['manifest_type']).lower()}-{digest}"
    envelope["manifest_sha256"] = digest
    return json.dumps(envelope, separators=(",", ":"), sort_keys=True).encode()


def test_policy_defaults_are_decimal_and_deterministic() -> None:
    policy = StoragePolicy()
    assert Path("/scratch/djy8hg/aegis_mx_poc_data") == DEFAULT_DATA_ROOT
    assert policy.administrative_allocation_bytes == 10_995_116_277_760
    assert policy.target_root_bytes == 800_000_000_000
    assert policy.hard_root_bytes == 800_000_000_000
    assert policy.minimum_reserve_bytes == 50_000_000_000
    assert policy.temporary_limit_bytes == 20_000_000_000
    assert policy.sha256 == StoragePolicy().sha256
    assert len(policy.sha256) == 64


@pytest.mark.parametrize(
    "changes",
    [
        {"administrative_allocation_bytes": 0},
        {"administrative_allocation_bytes": 250 * DECIMAL_GB},
        {"target_root_bytes": -1},
        {"target_root_bytes": 801 * DECIMAL_GB},
        {"hard_root_bytes": 801 * DECIMAL_GB},
        {"minimum_reserve_bytes": 0},
        {"minimum_reserve_bytes": 49 * DECIMAL_GB},
        {"temporary_limit_bytes": 21 * DECIMAL_GB},
        {"temporary_limit_bytes": 101 * DECIMAL_GB},
    ],
)
def test_policy_rejects_invalid_limits(changes: dict[str, int]) -> None:
    with pytest.raises(StorageError, match="storage policy"):
        replace(StoragePolicy(), **changes)  # type: ignore[arg-type]


def test_policy_allows_only_fail_safer_operational_limits() -> None:
    policy = StoragePolicy(
        target_root_bytes=700 * DECIMAL_GB,
        hard_root_bytes=750 * DECIMAL_GB,
        minimum_reserve_bytes=60 * DECIMAL_GB,
        temporary_limit_bytes=10 * DECIMAL_GB,
    )
    assert policy.target_root_bytes == 700 * DECIMAL_GB


def test_initialize_creates_only_the_bounded_layout(tmp_path: Path) -> None:
    repository = _repository(tmp_path)
    for area in STORAGE_AREAS:
        assert (repository.root / area).is_dir()
    assert (
        json.loads((repository.root / ".aegis-data-root.json").read_text())[
            "schema_version"
        ]
        == DATA_REPOSITORY_SCHEMA_VERSION
    )
    repository.initialize()

    unbound = DataRepository(
        tmp_path / "explicitly-unbound-test-root",
        filesystem_probe=lambda _path: _filesystem(),
        git_worktree=None,
    )
    unbound.initialize()
    assert (unbound.root / "manifests").is_dir()


def test_initialize_rejects_relative_git_nested_and_symlink_roots(
    tmp_path: Path,
) -> None:
    with pytest.raises(StorageError, match="absolute"):
        DataRepository(Path("relative"), git_worktree=Path.cwd())
    with pytest.raises(StorageError, match="Git worktree"):
        DataRepository(Path.cwd() / "data", git_worktree=Path.cwd()).initialize()

    target = tmp_path / "target"
    target.mkdir()
    linked = tmp_path / "linked"
    linked.symlink_to(target, target_is_directory=True)
    with pytest.raises(StorageError, match="symlink"):
        DataRepository(linked, git_worktree=Path.cwd()).initialize()

    worktree_alias = tmp_path / "worktree-alias"
    worktree_alias.symlink_to(Path.cwd(), target_is_directory=True)
    with pytest.raises(StorageError, match="Git worktree"):
        DataRepository(
            worktree_alias / "hidden-data", git_worktree=Path.cwd()
        ).initialize()

    loop = tmp_path / "loop"
    loop.symlink_to(loop, target_is_directory=True)
    with pytest.raises(StorageError, match="ancestry"):
        DataRepository(loop / "data", git_worktree=Path.cwd()).initialize()


def test_initialize_rejects_invalid_existing_area_and_policy_marker(
    tmp_path: Path,
) -> None:
    root = tmp_path / "poc-data"
    root.mkdir()
    (root / "raw").write_text("not a directory")
    with pytest.raises(StorageError, match="storage area"):
        DataRepository(root, git_worktree=Path.cwd()).initialize()

    (root / "raw").unlink()
    repository = DataRepository(root, git_worktree=Path.cwd())
    repository.initialize()
    marker = root / ".aegis-data-root.json"
    marker.chmod(0o600)
    marker.write_text("{}")
    with pytest.raises(StorageError, match="marker"):
        repository.initialize()


def _legacy_v2_root(tmp_path: Path) -> tuple[Path, bytes]:
    root = tmp_path / "legacy-data"
    root.mkdir(parents=True)
    for area in STORAGE_AREAS:
        (root / area).mkdir()
    (root / ".admission.lock").touch(mode=0o600)
    marker_body = {
        "data_root_kind": "AEGIS_MX_BOUNDED_FORECASTING_POC",
        "policy_sha256": repository_module.LEGACY_STORAGE_POLICY_V2_SHA256,
        "schema_version": "2.0.0",
    }
    marker = (
        json.dumps(
            marker_body, ensure_ascii=True, separators=(",", ":"), sort_keys=True
        ).encode("ascii")
        + b"\n"
    )
    marker_path = root / ".aegis-data-root.json"
    marker_path.write_bytes(marker)
    marker_path.chmod(0o400)
    return root, marker


def test_policy_v2_to_v3_migration_is_explicit_audited_and_idempotent(
    tmp_path: Path,
) -> None:
    root, legacy_marker = _legacy_v2_root(tmp_path)
    repository = DataRepository(root, git_worktree=Path.cwd())
    with pytest.raises(StorageError, match="marker"):
        repository.initialize()

    planned = repository.migrate_policy_v2_to_v3(
        expected_current_policy_sha256=(
            repository_module.LEGACY_STORAGE_POLICY_V2_SHA256
        )
    )
    assert planned["migrated"] is False
    assert planned["already_current"] is False
    assert (root / ".aegis-data-root.json").read_bytes() == legacy_marker
    assert not (root / "manifests/policy-history").exists()

    migrated = repository.migrate_policy_v2_to_v3(
        expected_current_policy_sha256=(
            repository_module.LEGACY_STORAGE_POLICY_V2_SHA256
        ),
        execute=True,
    )
    assert migrated["migrated"] is True
    history = root / cast("str", migrated["legacy_marker_archive"])
    record = root / cast("str", migrated["migration_record"])
    assert history.read_bytes() == legacy_marker
    assert history.stat().st_mode & 0o777 == 0o400
    record_body = json.loads(record.read_text())
    migration_schema = json.loads(
        Path("schemas/data-storage-policy-migration-v1.schema.json").read_text()
    )
    Draft202012Validator(migration_schema).validate(record_body)
    assert record_body["from_policy_sha256"] == (
        repository_module.LEGACY_STORAGE_POLICY_V2_SHA256
    )
    assert record_body["to_policy_sha256"] == StoragePolicy().sha256
    assert record_body["network_access_performed"] is False
    repository.initialize()

    repeated = repository.migrate_policy_v2_to_v3(
        expected_current_policy_sha256=(
            repository_module.LEGACY_STORAGE_POLICY_V2_SHA256
        ),
        execute=True,
    )
    assert repeated["already_current"] is True
    assert repeated["migrated"] is False


def test_policy_migration_rejects_unreviewed_or_unsafe_state(tmp_path: Path) -> None:
    root, _marker = _legacy_v2_root(tmp_path / "base")
    repository = DataRepository(root, git_worktree=Path.cwd())
    with pytest.raises(StorageError, match="nonzero lowercase"):
        repository.migrate_policy_v2_to_v3(expected_current_policy_sha256="bad")
    with pytest.raises(StorageError, match="only the reviewed"):
        repository.migrate_policy_v2_to_v3(expected_current_policy_sha256="1" * 64)

    missing = DataRepository(tmp_path / "missing", git_worktree=Path.cwd())
    with pytest.raises(StorageError, match="existing non-symlink"):
        missing.migrate_policy_v2_to_v3(
            expected_current_policy_sha256=(
                repository_module.LEGACY_STORAGE_POLICY_V2_SHA256
            )
        )

    target, _target_marker = _legacy_v2_root(tmp_path / "symlink-target")
    linked = tmp_path / "linked"
    linked.symlink_to(target, target_is_directory=True)
    with pytest.raises(StorageError, match="existing non-symlink"):
        DataRepository(linked, git_worktree=Path.cwd()).migrate_policy_v2_to_v3(
            expected_current_policy_sha256=(
                repository_module.LEGACY_STORAGE_POLICY_V2_SHA256
            )
        )

    absent_area_root, _absent_marker = _legacy_v2_root(tmp_path / "absent-area")
    (absent_area_root / "tmp").rmdir()
    with pytest.raises(StorageError, match="existing storage area tmp"):
        DataRepository(
            absent_area_root, git_worktree=Path.cwd()
        ).migrate_policy_v2_to_v3(
            expected_current_policy_sha256=(
                repository_module.LEGACY_STORAGE_POLICY_V2_SHA256
            )
        )

    linked_area_root, _linked_area_marker = _legacy_v2_root(tmp_path / "linked-area")
    (linked_area_root / "tmp").rmdir()
    (linked_area_root / "tmp").symlink_to(
        linked_area_root / "raw", target_is_directory=True
    )
    with pytest.raises(StorageError, match="existing storage area tmp"):
        DataRepository(
            linked_area_root, git_worktree=Path.cwd()
        ).migrate_policy_v2_to_v3(
            expected_current_policy_sha256=(
                repository_module.LEGACY_STORAGE_POLICY_V2_SHA256
            )
        )

    corrupt_root, _corrupt_marker = _legacy_v2_root(tmp_path / "corrupt")
    corrupt_marker_path = corrupt_root / ".aegis-data-root.json"
    corrupt_marker_path.chmod(0o600)
    corrupt_marker_path.write_text("{}")
    with pytest.raises(StorageError, match="does not match"):
        DataRepository(corrupt_root, git_worktree=Path.cwd()).migrate_policy_v2_to_v3(
            expected_current_policy_sha256=(
                repository_module.LEGACY_STORAGE_POLICY_V2_SHA256
            )
        )


def test_policy_migration_rejects_conflicts_and_supports_interrupted_retry(  # noqa: PLR0915
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    expected = repository_module.LEGACY_STORAGE_POLICY_V2_SHA256

    invalid_directory_root, _marker = _legacy_v2_root(tmp_path / "directory")
    (invalid_directory_root / "manifests/policy-history").write_text("not-dir")
    with pytest.raises(StorageError, match="history path is not a directory"):
        DataRepository(
            invalid_directory_root, git_worktree=Path.cwd()
        ).migrate_policy_v2_to_v3(expected_current_policy_sha256=expected, execute=True)

    history_conflict_root, _marker = _legacy_v2_root(tmp_path / "history-conflict")
    history_conflict_repository = DataRepository(
        history_conflict_root, git_worktree=Path.cwd()
    )
    history_plan = history_conflict_repository.migrate_policy_v2_to_v3(
        expected_current_policy_sha256=expected
    )
    history_conflict = history_conflict_root / cast(
        "str", history_plan["legacy_marker_archive"]
    )
    history_conflict.parent.mkdir()
    history_conflict.write_text("conflict")
    with pytest.raises(StorageError, match="archive conflicts"):
        history_conflict_repository.migrate_policy_v2_to_v3(
            expected_current_policy_sha256=expected, execute=True
        )

    linked_history_root, _marker = _legacy_v2_root(tmp_path / "linked-history")
    linked_history_repository = DataRepository(
        linked_history_root, git_worktree=Path.cwd()
    )
    linked_history_plan = linked_history_repository.migrate_policy_v2_to_v3(
        expected_current_policy_sha256=expected
    )
    linked_history = linked_history_root / cast(
        "str", linked_history_plan["legacy_marker_archive"]
    )
    linked_history.parent.mkdir()
    linked_history.symlink_to(linked_history_root / "missing")
    with pytest.raises(StorageError, match="cannot be a symlink"):
        linked_history_repository.migrate_policy_v2_to_v3(
            expected_current_policy_sha256=expected, execute=True
        )

    record_conflict_root, record_marker = _legacy_v2_root(tmp_path / "record-conflict")
    record_conflict_repository = DataRepository(
        record_conflict_root, git_worktree=Path.cwd()
    )
    record_plan = record_conflict_repository.migrate_policy_v2_to_v3(
        expected_current_policy_sha256=expected
    )
    history_path = record_conflict_root / cast(
        "str", record_plan["legacy_marker_archive"]
    )
    history_path.parent.mkdir()
    history_path.write_bytes(record_marker)
    record_path = record_conflict_root / cast("str", record_plan["migration_record"])
    record_path.write_text("conflict")
    with pytest.raises(StorageError, match="migration record conflicts"):
        record_conflict_repository.migrate_policy_v2_to_v3(
            expected_current_policy_sha256=expected, execute=True
        )

    staged_root, _marker = _legacy_v2_root(tmp_path / "staged")
    (staged_root / ".aegis-data-root.json.v3.part").write_text("staged")
    with pytest.raises(StorageError, match="staged policy marker"):
        DataRepository(staged_root, git_worktree=Path.cwd()).migrate_policy_v2_to_v3(
            expected_current_policy_sha256=expected, execute=True
        )

    linked_stage_root, _marker = _legacy_v2_root(tmp_path / "linked-stage")
    (linked_stage_root / ".aegis-data-root.json.v3.part").symlink_to(
        linked_stage_root / "missing"
    )
    with pytest.raises(StorageError, match="staged policy marker"):
        DataRepository(
            linked_stage_root, git_worktree=Path.cwd()
        ).migrate_policy_v2_to_v3(expected_current_policy_sha256=expected, execute=True)

    retry_root, _marker = _legacy_v2_root(tmp_path / "retry")
    retry_repository = DataRepository(retry_root, git_worktree=Path.cwd())
    original_replace = Path.replace
    replacement_error = "injected replacement failure"

    def fail_replace(_path: Path, _target: Path) -> Path:
        raise OSError(replacement_error)

    monkeypatch.setattr(Path, "replace", fail_replace)
    with pytest.raises(StorageError, match="replacement failed"):
        retry_repository.migrate_policy_v2_to_v3(
            expected_current_policy_sha256=expected, execute=True
        )
    monkeypatch.setattr(Path, "replace", original_replace)
    (retry_root / ".aegis-data-root.json.v3.part").unlink()
    retried = retry_repository.migrate_policy_v2_to_v3(
        expected_current_policy_sha256=expected, execute=True
    )
    assert retried["migrated"] is True


def test_policy_migration_detects_marker_change_during_execution(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    root, _marker = _legacy_v2_root(tmp_path)
    repository = DataRepository(root, git_worktree=Path.cwd())
    marker_path = root / ".aegis-data-root.json"
    original_read = repository._read_secure  # noqa: SLF001
    marker_reads = 0

    def changing_read(path: Path, maximum: int) -> bytes:
        nonlocal marker_reads
        if path == marker_path:
            marker_reads += 1
            if marker_reads == 2:
                marker_path.chmod(0o600)
                marker_path.write_text("{}")
        return original_read(path, maximum)

    monkeypatch.setattr(repository, "_read_secure", changing_read)
    with pytest.raises(StorageError, match="changed during migration"):
        repository.migrate_policy_v2_to_v3(
            expected_current_policy_sha256=(
                repository_module.LEGACY_STORAGE_POLICY_V2_SHA256
            ),
            execute=True,
        )


def test_usage_accounts_for_committed_partial_temporary_and_sparse_files(
    tmp_path: Path,
) -> None:
    repository = _repository(tmp_path)
    _write_object(repository, "raw/complete.bin", b"1234")
    _write_object(repository, "raw/interrupted.part", b"12345")
    _write_object(repository, "tmp/work.bin", b"123456")
    sparse = repository.root / "derived" / "sparse.bin"
    sparse.parent.mkdir(parents=True, exist_ok=True)
    with sparse.open("wb") as output:
        output.truncate(2 * DECIMAL_GB)

    usage = repository.usage()
    assert usage.partial_bytes == 5
    assert usage.temporary_bytes == 6
    assert usage.total_bytes >= 2 * DECIMAL_GB + 15
    assert usage.logical_bytes >= usage.allocated_bytes
    assert usage.file_count >= 5
    total = usage.to_dict()["total"]
    assert isinstance(total, dict)
    assert total["bytes_decimal"] == usage.total_bytes
    assert "gib_binary" in total


def test_usage_rejects_symlink_hardlink_and_special_files(tmp_path: Path) -> None:
    repository = _repository(tmp_path)
    outside = tmp_path / "outside"
    outside.write_bytes(b"outside")
    symlink = repository.root / "raw" / "escape"
    symlink.symlink_to(outside)
    with pytest.raises(StorageError, match="symlink"):
        repository.usage()
    symlink.unlink()

    first = _write_object(repository, "raw/first", b"content")
    hardlink = repository.root / "raw" / "second"
    os.link(first, hardlink)
    with pytest.raises(StorageError, match="hard link"):
        repository.usage()
    hardlink.unlink()
    first.unlink()

    fifo = repository.root / "raw" / "fifo"
    os.mkfifo(fifo)
    with pytest.raises(StorageError, match="unsupported"):
        repository.usage()


def test_quota_evidence_requires_complete_authoritative_metadata() -> None:
    assert not QuotaEvidence().known
    assert not replace(_quota(), authoritative=False).known
    assert _quota().known
    with pytest.raises(StorageError, match="quota"):
        QuotaEvidence(limit_bytes=1, used_bytes=2, source="fixture", authoritative=True)
    with pytest.raises(StorageError, match="integer"):
        QuotaEvidence(limit_bytes=-1, used_bytes=0)


@pytest.mark.parametrize(
    ("storage_request", "quota", "filesystem", "reason"),
    [
        (
            StorageRequest("unknown-quota", 1),
            QuotaEvidence(),
            _filesystem(),
            AdmissionReason.QUOTA_UNKNOWN,
        ),
        (
            StorageRequest("unknown-output", None),
            _quota(),
            _filesystem(),
            AdmissionReason.PROJECTION_UNKNOWN,
        ),
        (
            StorageRequest("reserve", 2 * DECIMAL_GB),
            _quota(),
            _filesystem(free_bytes=51 * DECIMAL_GB),
            AdmissionReason.FILESYSTEM_RESERVE,
        ),
        (
            StorageRequest("quota", 2 * DECIMAL_GB),
            _quota(used_bytes=249 * DECIMAL_GB),
            _filesystem(),
            AdmissionReason.QUOTA_EXHAUSTED,
        ),
        (
            StorageRequest("temporary", 1, temporary_bytes=21 * DECIMAL_GB),
            _quota(),
            _filesystem(),
            AdmissionReason.TEMPORARY_LIMIT,
        ),
        (
            StorageRequest("hard", 801 * DECIMAL_GB),
            _quota(),
            _filesystem(),
            AdmissionReason.HARD_ROOT_LIMIT,
        ),
        (
            StorageRequest("target", 801 * DECIMAL_GB),
            _quota(),
            _filesystem(),
            AdmissionReason.TARGET_REVIEW_REQUIRED,
        ),
    ],
)
def test_admission_fails_closed(
    tmp_path: Path,
    storage_request: StorageRequest,
    quota: QuotaEvidence,
    filesystem: FileSystemState,
    reason: AdmissionReason,
) -> None:
    decision = _repository(tmp_path, filesystem=filesystem).estimate(
        storage_request, quota
    )
    assert not decision.admitted
    assert reason in decision.reasons
    assert decision.to_dict()["admitted"] is False


def test_admission_uses_stricter_quota_and_accounts_for_overhead(
    tmp_path: Path,
) -> None:
    repository = _repository(tmp_path)
    quota = replace(_quota(), limit_bytes=300 * DECIMAL_GB)
    request = StorageRequest(
        "accepted",
        output_bytes=1_000,
        temporary_bytes=2_000,
        retry_overhead_bytes=3_000,
    )
    decision = repository.estimate(request, quota)
    assert decision.admitted
    assert decision.effective_quota_limit_bytes == 300 * DECIMAL_GB
    assert decision.projected_root_peak_bytes == decision.usage.total_bytes + 6_000
    assert decision.projected_temporary_peak_bytes == 5_000
    assert decision.projected_quota_used_bytes == quota.used_bytes + 6_000  # type: ignore[operator]


def test_verified_scratch_quota_does_not_inherit_obsolete_home_ceiling(
    tmp_path: Path,
) -> None:
    repository = _repository(tmp_path)
    quota = replace(
        _quota(),
        limit_bytes=10_995_116_277_760,
        used_bytes=608_990_093_312,
        source="/opt/rci/bin/hdquota",
        observed_at_utc="2026-09-11T00:12:13.122793Z",
    )
    decision = repository.estimate(
        StorageRequest("verified-scratch-pilot", 23_277_475, 1_868_276, 0),
        quota,
    )
    assert decision.admitted
    assert decision.effective_quota_limit_bytes == 10_995_116_277_760
    assert decision.projected_quota_used_bytes == 609_015_239_063
    assert decision.projected_root_peak_bytes == decision.usage.total_bytes + 25_145_751
    assert decision.projected_temporary_peak_bytes == 1_868_276


def test_integer_overflow_and_invalid_request_are_rejected(tmp_path: Path) -> None:
    with pytest.raises(StorageError, match="output_bytes"):
        StorageRequest("negative", -1)
    with pytest.raises(StorageError, match="operation_id"):
        StorageRequest("", 1)
    request = StorageRequest("overflow", (1 << 63) - 1, retry_overhead_bytes=1)
    decision = _repository(tmp_path).estimate(request, _quota())
    assert not decision.admitted
    assert decision.reasons == (AdmissionReason.INTEGER_OVERFLOW,)


def test_admission_lease_serializes_writers_and_releases(tmp_path: Path) -> None:
    repository = _repository(tmp_path)
    request = StorageRequest("writer-one", 1_000)
    first = repository.acquire_admission(request, _quota())
    assert first.active
    with pytest.raises(StorageError, match="concurrent"):
        repository.acquire_admission(StorageRequest("writer-two", 1_000), _quota())
    first.close()
    assert not first.active
    with repository.acquire_admission(request, _quota()) as second:
        assert second.active
        assert second.decision.admitted


def test_concurrent_threads_never_receive_two_active_leases(tmp_path: Path) -> None:
    repository = _repository(tmp_path)
    barrier = threading.Barrier(2)
    release = threading.Event()
    results: list[str] = []

    def contender(identifier: str) -> None:
        barrier.wait()
        try:
            with repository.acquire_admission(StorageRequest(identifier, 1), _quota()):
                results.append("admitted")
                release.wait(timeout=2)
        except StorageError as error:
            results.append(error.code.value)
            release.set()

    threads = [
        threading.Thread(target=contender, args=("one",)),
        threading.Thread(target=contender, args=("two",)),
    ]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    assert results.count("admitted") == 1
    assert results.count(StorageErrorCode.CONCURRENT_ADMISSION.value) == 1


def test_manifest_encoding_is_deterministic_and_strict() -> None:
    manifest = _source_manifest()
    encoded = encode_manifest(manifest)
    assert encoded == encode_manifest(manifest)
    assert decode_manifest(encoded) == manifest
    assert manifest.manifest_id.startswith("source-")

    payload = json.loads(encoded)
    payload["unknown"] = True
    with pytest.raises(StorageError, match="manifest"):
        decode_manifest(json.dumps(payload).encode())
    with pytest.raises(StorageError, match="manifest"):
        decode_manifest(b"not-json")
    with pytest.raises(StorageError, match="manifest"):
        decode_manifest(b"[]")


def test_manifest_validation_rejects_bad_metadata_and_paths() -> None:
    manifest = _source_manifest()
    for changes in (
        {"source_name": ""},
        {"object_sha256": "0" * 64},
        {"size_bytes": -1},
        {"record_count": -1},
        {"storage_path": "../escape"},
        {"storage_path": "/absolute"},
        {"storage_path": "tmp/not-accepted"},
    ):
        with pytest.raises(StorageError):
            replace(manifest, **changes)

    with pytest.raises(StorageError, match="time"):
        replace(_times(), event_time_min_ns=21)
    with pytest.raises(StorageError, match="lineage"):
        ManifestLineage(LineageRelation.CORRECTS, None, "reason")
    with pytest.raises(StorageError, match="lineage"):
        ManifestLineage(LineageRelation.NONE, "source-parent", "reason")
    with pytest.raises(StorageError, match="manifest type"):
        ManifestLineage(LineageRelation.CORRECTS, "unknown-" + "11" * 32, "reason")
    with pytest.raises(StorageError, match="SHA-256"):
        ManifestLineage(LineageRelation.CORRECTS, "source-invalid", "reason")


def test_partition_and_dataset_identity_are_content_deterministic() -> None:
    partition = PartitionManifest(
        dataset_name="minute-bars",
        partition_key="session=2026-09-09/bucket=01",
        storage_path="canonical/session=2026-09-09/bucket=01.parquet",
        object_sha256=NONZERO_SHA256,
        size_bytes=123,
        record_count=79,
        schema_name="canonical-minute",
        schema_version="1.0.0",
        times=_times(),
        universe_snapshot_sha256=UNIVERSE_SHA256,
        source_manifest_ids=(SOURCE_B_ID, SOURCE_A_ID),
        lineage=ManifestLineage(),
    )
    decoded = decode_manifest(encode_manifest(partition))
    assert decoded == partition
    assert partition.source_manifest_ids == (SOURCE_A_ID, SOURCE_B_ID)

    first = deterministic_dataset_id(
        ("partition-b", "partition-a"),
        universe_snapshot_sha256=UNIVERSE_SHA256,
        calendar_version="calendar-v1",
        schema_version="dataset-v1",
    )
    second = deterministic_dataset_id(
        ("partition-a", "partition-b"),
        universe_snapshot_sha256=UNIVERSE_SHA256,
        calendar_version="calendar-v1",
        schema_version="dataset-v1",
    )
    assert first == second
    assert first.startswith("dataset-")
    with pytest.raises(StorageError, match="partition"):
        deterministic_dataset_id(
            (),
            universe_snapshot_sha256=UNIVERSE_SHA256,
            calendar_version="calendar-v1",
            schema_version="dataset-v1",
        )


def test_atomic_publication_and_verification(tmp_path: Path) -> None:
    repository = _repository(tmp_path)
    data = b"content"
    digest = hashlib.sha256(data).hexdigest()
    _write_object(repository, "raw/provider/object.bin", data)
    manifest = _source_manifest(object_sha256=digest)

    with repository.acquire_admission(
        StorageRequest("publish", 10_000), _quota()
    ) as lease:
        published = repository.publish_manifest(manifest, lease)
    assert published.is_file()
    assert published.stat().st_mode & 0o222 == 0
    assert repository.load_manifest(manifest.manifest_id) == manifest
    assert repository.verify().passed

    with repository.acquire_admission(
        StorageRequest("idempotent", 10_000), _quota()
    ) as lease:
        assert repository.publish_manifest(manifest, lease) == published


def test_publication_requires_a_live_sufficient_lease(tmp_path: Path) -> None:
    repository = _repository(tmp_path)
    manifest = _source_manifest()
    lease = repository.acquire_admission(StorageRequest("too-small", 1), _quota())
    with pytest.raises(StorageError, match="budget"):
        repository.publish_manifest(manifest, lease)
    lease.close()
    with pytest.raises(StorageError, match="lease"):
        repository.publish_manifest(manifest, lease)


def test_staged_object_publication_validates_lease_size_and_hash(
    tmp_path: Path,
) -> None:
    repository = _repository(tmp_path)
    request = StorageRequest("staged-object-validation", 10_000, 10_000, 0)
    payload = b"verified-object"
    digest = hashlib.sha256(payload).hexdigest()
    staged_relative = "tmp/downloads/object.partial"
    final_relative = f"raw/provider/{digest}.bin"

    with repository.acquire_admission(request, _quota()) as lease:
        pass
    _write_object(repository, staged_relative, payload)
    with pytest.raises(StorageError, match="active admission lease"):
        repository.publish_staged_object(
            staged_relative,
            final_relative,
            expected_sha256=digest,
            expected_size_bytes=len(payload),
            lease=lease,
        )

    other_repository = _repository(tmp_path / "other")
    with (
        other_repository.acquire_admission(request, _quota()) as other_lease,
        pytest.raises(StorageError, match="active admission lease"),
    ):
        repository.publish_staged_object(
            staged_relative,
            final_relative,
            expected_sha256=digest,
            expected_size_bytes=len(payload),
            lease=other_lease,
        )

    with repository.acquire_admission(request, _quota()) as active_lease:
        with pytest.raises(StorageError, match="size does not match"):
            repository.publish_staged_object(
                staged_relative,
                final_relative,
                expected_sha256=digest,
                expected_size_bytes=len(payload) + 1,
                lease=active_lease,
            )
        with pytest.raises(StorageError, match="SHA-256 does not match"):
            repository.publish_staged_object(
                staged_relative,
                final_relative,
                expected_sha256="12" * 32,
                expected_size_bytes=len(payload),
                lease=active_lease,
            )


def test_staged_object_publication_is_idempotent_and_conflict_safe(
    tmp_path: Path,
) -> None:
    repository = _repository(tmp_path)
    request = StorageRequest("staged-object-idempotence", 10_000, 10_000, 0)
    payload = b"verified-object"
    digest = hashlib.sha256(payload).hexdigest()
    staged_relative = "tmp/downloads/object.partial"
    final_relative = f"raw/provider/{digest}.bin"
    final_path = _write_object(repository, final_relative, payload)

    with repository.acquire_admission(request, _quota()) as lease:
        _write_object(repository, staged_relative, payload)
        assert (
            repository.publish_staged_object(
                staged_relative,
                final_relative,
                expected_sha256=digest,
                expected_size_bytes=len(payload),
                lease=lease,
            )
            == final_path
        )
        assert not (repository.root / staged_relative).exists()

        _write_object(repository, staged_relative, payload)
        final_path.write_bytes(b"short")
        with pytest.raises(StorageError, match="different bytes"):
            repository.publish_staged_object(
                staged_relative,
                final_relative,
                expected_sha256=digest,
                expected_size_bytes=len(payload),
                lease=lease,
            )

        final_path.write_bytes(b"tampered-object")
        assert final_path.stat().st_size == len(payload)
        with pytest.raises(StorageError, match="different bytes"):
            repository.publish_staged_object(
                staged_relative,
                final_relative,
                expected_sha256=digest,
                expected_size_bytes=len(payload),
                lease=lease,
            )


@pytest.mark.parametrize("failure", [FileExistsError(), OSError()])
def test_staged_object_publication_wraps_link_failures(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, failure: OSError
) -> None:
    repository = _repository(tmp_path)
    request = StorageRequest("staged-object-link-failure", 10_000, 10_000, 0)
    payload = b"verified-object"
    digest = hashlib.sha256(payload).hexdigest()
    staged_relative = "tmp/downloads/object.partial"
    _write_object(repository, staged_relative, payload)

    def fail_link(*_args: object, **_kwargs: object) -> None:
        raise failure

    monkeypatch.setattr(os, "link", fail_link)
    with repository.acquire_admission(request, _quota()) as lease:
        expected = (
            "appeared concurrently"
            if isinstance(failure, FileExistsError)
            else "publication was interrupted"
        )
        with pytest.raises(StorageError, match=expected):
            repository.publish_staged_object(
                staged_relative,
                f"raw/provider/{digest}.bin",
                expected_sha256=digest,
                expected_size_bytes=len(payload),
                lease=lease,
            )


def test_interrupted_publication_is_visible_and_never_replaces(
    tmp_path: Path,
) -> None:
    repository = _repository(tmp_path)
    data = b"content"
    _write_object(repository, "raw/provider/object.bin", data)
    manifest = _source_manifest(object_sha256=hashlib.sha256(data).hexdigest())

    def interrupt(stage: PublicationStage) -> None:
        if stage is PublicationStage.TEMP_FILE_SYNCED:
            message = "synthetic interruption"
            raise OSError(message)

    with (
        repository.acquire_admission(
            StorageRequest("interrupt", 10_000), _quota()
        ) as lease,
        pytest.raises(StorageError, match="interrupted"),
    ):
        repository.publish_manifest(manifest, lease, fault_injector=interrupt)
    assert not repository.manifest_path(manifest).exists()
    assert any(repository.root.joinpath("tmp").iterdir())
    assert repository.usage().temporary_bytes > 0


def test_verify_reports_corrupt_manifest_hash_mismatch_and_missing_lineage(
    tmp_path: Path,
) -> None:
    repository = _repository(tmp_path)
    data = b"content"
    path = _write_object(repository, "raw/provider/object.bin", data)
    manifest = _source_manifest(object_sha256=hashlib.sha256(data).hexdigest())
    with repository.acquire_admission(
        StorageRequest("publish", 10_000), _quota()
    ) as lease:
        manifest_path = repository.publish_manifest(manifest, lease)

    path.write_bytes(b"CONTENT")
    report = repository.verify()
    assert not report.passed
    assert StorageErrorCode.HASH_MISMATCH.value in {item.code for item in report.errors}

    path.write_bytes(data)
    manifest_path.chmod(0o600)
    manifest_path.write_text("{")
    report = repository.verify()
    assert StorageErrorCode.MANIFEST_CORRUPT.value in {
        item.code for item in report.errors
    }

    manifest_path.unlink()
    corrected = _source_manifest(
        object_sha256=hashlib.sha256(data).hexdigest(),
        lineage=ManifestLineage(
            LineageRelation.CORRECTS,
            "source-" + "33" * 32,
            "provider correction",
        ),
    )
    with repository.acquire_admission(
        StorageRequest("corrected", 10_000), _quota()
    ) as lease:
        repository.publish_manifest(corrected, lease)
    report = repository.verify()
    assert StorageErrorCode.MISSING_LINEAGE.value in {
        item.code for item in report.errors
    }


def test_verify_reports_manifest_filename_and_object_size_mismatch(
    tmp_path: Path,
) -> None:
    repository = _repository(tmp_path)
    data = b"content"
    _write_object(repository, "raw/provider/object.bin", data)
    manifest = _source_manifest(
        object_sha256=hashlib.sha256(data).hexdigest(), size_bytes=8
    )
    manifest_dir = repository.root / "manifests" / "source"
    manifest_dir.mkdir(parents=True, exist_ok=True)
    (manifest_dir / "wrong-name.json").write_bytes(encode_manifest(manifest))
    report = repository.verify()
    codes = {item.code for item in report.errors}
    assert StorageErrorCode.MANIFEST_CORRUPT.value in codes

    (manifest_dir / "wrong-name.json").rename(
        manifest_dir / f"{manifest.manifest_id}.json"
    )
    codes = {item.code for item in repository.verify().errors}
    assert StorageErrorCode.SIZE_MISMATCH.value in codes


def test_cleanup_plan_is_deterministic_and_never_deletes(tmp_path: Path) -> None:
    repository = _repository(tmp_path)
    temporary = _write_object(repository, "tmp/retry.part", b"12345")
    partial = _write_object(repository, "raw/pending.partial", b"1234")
    quarantine = _write_object(repository, "quarantine/rejected.bin", b"123")
    committed = _write_object(repository, "canonical/accepted.bin", b"12")
    before = {
        path: path.read_bytes() for path in (temporary, partial, quarantine, committed)
    }
    target = repository.usage().total_bytes - 8

    first = repository.cleanup_plan(target_bytes=target)
    second = repository.cleanup_plan(target_bytes=target)
    assert first == second
    assert first.required_reduction_bytes == 8
    assert first.planned_reduction_bytes >= 8
    assert all(not item.automatic for item in first.candidates)
    assert all(item.operator_approval_required for item in first.candidates)
    assert first.candidates[0].relative_path == "tmp/retry.part"
    assert {path: path.read_bytes() for path in before} == before


def test_path_validation_blocks_manifest_object_escape_and_symlink(
    tmp_path: Path,
) -> None:
    repository = _repository(tmp_path)
    outside = tmp_path / "outside"
    outside.mkdir()
    link = repository.root / "raw" / "provider"
    link.symlink_to(outside, target_is_directory=True)
    with pytest.raises(StorageError, match="symlink"):
        repository.acquire_admission(StorageRequest("publish", 10_000), _quota())


@pytest.mark.parametrize(
    "timestamp",
    ["not-a-time", "2026-09-09T16:00:00", "2026-09-09T12:00:00-04:00"],
)
def test_quota_timestamp_must_be_explicit_utc(timestamp: str) -> None:
    with pytest.raises(StorageError, match=r"UTC|ISO-8601"):
        replace(_quota(), observed_at_utc=timestamp)


def test_filesystem_state_rejects_invalid_or_inconsistent_values() -> None:
    assert not FileSystemState(None, None).known
    with pytest.raises(StorageError, match="free bytes"):
        FileSystemState(total_bytes=1, free_bytes=2)
    with pytest.raises(StorageError, match="integer"):
        FileSystemState(total_bytes=-1, free_bytes=0)


def test_unknown_filesystem_capacity_blocks_admission(tmp_path: Path) -> None:
    decision = _repository(tmp_path, filesystem=FileSystemState(None, None)).estimate(
        StorageRequest("unknown-filesystem", 1), _quota()
    )
    assert not decision.admitted
    assert AdmissionReason.FILESYSTEM_CAPACITY_UNKNOWN in decision.reasons


@pytest.mark.parametrize(
    "storage_path",
    ["raw\\ambiguous", "raw/" + "x" * 513, "", 7],
)
def test_manifest_rejects_malformed_path_forms(storage_path: object) -> None:
    with pytest.raises(StorageError, match="path"):
        replace(_source_manifest(), storage_path=storage_path)  # type: ignore[arg-type]


def test_manifest_time_ranges_reject_availability_disorder() -> None:
    with pytest.raises(StorageError, match="availability"):
        replace(_times(), processing_time_min_ns=1)
    with pytest.raises(StorageError, match="time range"):
        replace(_times(), event_time_min_ns=-1)


def test_partition_requires_unique_nonempty_source_manifests() -> None:
    for identifiers in ((), (SOURCE_A_ID, SOURCE_A_ID)):
        with pytest.raises(StorageError, match="nonempty and unique"):
            PartitionManifest(
                dataset_name="minute-bars",
                partition_key="session=2026-09-09",
                storage_path="canonical/partition.bin",
                object_sha256=NONZERO_SHA256,
                size_bytes=1,
                record_count=1,
                schema_name="canonical-minute",
                schema_version="1.0.0",
                times=_times(),
                universe_snapshot_sha256=UNIVERSE_SHA256,
                source_manifest_ids=identifiers,
                lineage=ManifestLineage(),
            )


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("object_sha256", "AB" * 32),
        ("object_sha256", "gg" * 32),
        ("universe_snapshot_sha256", "short"),
        ("source_name", 1),
        ("schema_version", "x" * 65),
    ],
)
def test_source_manifest_rejects_additional_malformed_fields(
    field: str, value: object
) -> None:
    with pytest.raises(StorageError):
        replace(_source_manifest(), **{field: value})  # type: ignore[arg-type]


@pytest.mark.parametrize(
    ("mutation", "message"),
    [
        (lambda item: item.update({"manifest_schema_version": "9.9.9"}), "schema"),
        (lambda item: item.update({"manifest_type": "UNKNOWN"}), "type"),
        (lambda item: item.update({"manifest_id": "source-tampered"}), "hash"),
    ],
)
def test_decode_manifest_rejects_envelope_tampering(
    mutation: Callable[[dict[str, Any]], None], message: str
) -> None:
    envelope = json.loads(encode_manifest(_source_manifest()))
    mutation(envelope)
    with pytest.raises(StorageError, match=message):
        decode_manifest(json.dumps(envelope).encode())


@pytest.mark.parametrize(
    ("path", "value"),
    [
        (("payload", "record_count"), "two"),
        (("payload", "source_name"), 2),
        (("payload", "lineage", "parent_manifest_id"), 3),
        (("payload", "lineage", "reason"), 4),
        (("payload", "lineage", "relation"), "INVALID"),
    ],
)
def test_decode_manifest_rejects_signed_malformed_payloads(
    path: tuple[str, ...], value: object
) -> None:
    envelope: dict[str, Any] = json.loads(encode_manifest(_source_manifest()))
    target: dict[str, Any] = envelope
    for component in path[:-1]:
        target = target[component]
    target[path[-1]] = value
    with pytest.raises(StorageError, match=r"manifest|lineage|string|integer"):
        decode_manifest(_resign(envelope))


def test_decode_partition_rejects_invalid_and_noncanonical_sources() -> None:
    partition = PartitionManifest(
        dataset_name="minute-bars",
        partition_key="session=2026-09-09",
        storage_path="canonical/partition.bin",
        object_sha256=NONZERO_SHA256,
        size_bytes=1,
        record_count=1,
        schema_name="canonical-minute",
        schema_version="1.0.0",
        times=_times(),
        universe_snapshot_sha256=UNIVERSE_SHA256,
        source_manifest_ids=(SOURCE_A_ID, SOURCE_B_ID),
        lineage=ManifestLineage(),
    )
    envelope: dict[str, Any] = json.loads(encode_manifest(partition))
    envelope["payload"]["source_manifest_ids"] = [1]
    with pytest.raises(StorageError, match="string array"):
        decode_manifest(_resign(envelope))

    envelope = json.loads(encode_manifest(partition))
    envelope["payload"]["source_manifest_ids"] = [SOURCE_B_ID, SOURCE_A_ID]
    with pytest.raises(StorageError, match="decoded manifest identity"):
        decode_manifest(_resign(envelope))


def test_decode_manifest_rejects_oversize_and_nonbyte_input() -> None:
    with pytest.raises(StorageError, match="oversized"):
        decode_manifest(b"x" * 1_048_577)
    with pytest.raises(StorageError, match="encoding"):
        decode_manifest("not-bytes")  # type: ignore[arg-type]


def test_dataset_identity_rejects_duplicates_and_invalid_metadata() -> None:
    with pytest.raises(StorageError, match="unique"):
        deterministic_dataset_id(
            ("partition-a", "partition-a"),
            universe_snapshot_sha256=UNIVERSE_SHA256,
            calendar_version="calendar-v1",
            schema_version="dataset-v1",
        )
    with pytest.raises(StorageError, match="SHA-256"):
        deterministic_dataset_id(
            ("partition-a",),
            universe_snapshot_sha256="bad",
            calendar_version="calendar-v1",
            schema_version="dataset-v1",
        )


def test_verification_issue_and_audit_records_are_structured(tmp_path: Path) -> None:
    issue = VerificationIssue("CODE", "raw/item", "detail")
    assert issue.to_dict()["code"] == "CODE"
    context = StorageAuditContext(
        operation="test",
        outcome="SUCCEEDED",
        data_root=tmp_path,
        policy_sha256=StoragePolicy().sha256,
    )
    record = make_audit_record(
        context,
        {"evidence": True},
        observed_at_wall_utc="2026-09-09T16:00:00Z",
    )
    assert record.audit_id.startswith("storage-audit-")
    assert record.to_dict()["audit_id"] == record.audit_id
    assert record.to_dict()["network_access_performed"] is False
    with pytest.raises(StorageError, match="outcome"):
        replace(record, outcome="UNKNOWN")
    with pytest.raises(StorageError, match="reason"):
        replace(record, reason_codes=("",))


def test_lease_close_and_inactive_entry_are_fail_closed(tmp_path: Path) -> None:
    repository = _repository(tmp_path)
    lease = repository.acquire_admission(StorageRequest("lease", 1), _quota())
    lease.close()
    lease.close()
    with pytest.raises(StorageError, match="inactive"):
        lease.__enter__()


def test_initialize_rejects_regular_file_root(tmp_path: Path) -> None:
    root = tmp_path / "root-file"
    root.write_text("not a directory")
    with pytest.raises(StorageError, match="directory"):
        DataRepository(root, git_worktree=Path.cwd()).initialize()


def test_probe_failures_and_denied_acquire_release_the_lock(tmp_path: Path) -> None:
    for failure in (
        OSError("synthetic stat failure"),
        StorageError(StorageErrorCode.IO_FAILURE, "wrong storage error"),
        StorageError(StorageErrorCode.FILESYSTEM_UNKNOWN, "known capacity failure"),
    ):

        def fail(_path: Path, current: BaseException = failure) -> FileSystemState:
            raise current

        repository = DataRepository(
            tmp_path / failure.__class__.__name__ / str(id(failure)),
            filesystem_probe=fail,
            git_worktree=Path.cwd(),
        )
        repository.initialize()
        with pytest.raises(StorageError, match=r"filesystem|capacity"):
            repository.usage()

    repository = _repository(tmp_path / "denied")
    with pytest.raises(StorageError, match="admission denied"):
        repository.acquire_admission(StorageRequest("denied", 1), QuotaEvidence())
    with repository.acquire_admission(StorageRequest("accepted", 1), _quota()):
        pass


def test_quota_projection_overflow_is_explicit() -> None:
    quota = QuotaEvidence(
        limit_bytes=(1 << 63) - 1,
        used_bytes=(1 << 63) - 1,
        source="fixture",
        observed_at_utc="2026-09-09T16:00:00Z",
        authoritative=True,
    )
    projection = vars(repository_module)["_quota_projection"]
    assert projection(quota, StoragePolicy(), 1)[2] is AdmissionReason.INTEGER_OVERFLOW


def test_cleanup_plan_covers_all_candidate_classes(tmp_path: Path) -> None:
    repository = _repository(tmp_path)
    for relative, content in (
        ("tmp/temp.bin", b"1"),
        ("raw/object.part", b"2"),
        ("quarantine/bad.bin", b"3"),
        ("reports/report.json", b"4"),
        ("models/model.bin", b"5"),
        ("derived/features.bin", b"6"),
        ("canonical/minute.bin", b"7"),
    ):
        _write_object(repository, relative, content)
    plan = repository.cleanup_plan(target_bytes=0)
    reasons = {candidate.reason for candidate in plan.candidates}
    assert reasons == {
        "INTERRUPTED_OR_TEMPORARY_REVIEW",
        "PARTIAL_OBJECT_REVIEW",
        "QUARANTINE_RETENTION_REVIEW",
        "IMMUTABLE_RETENTION_REVIEW",
    }
    empty_plan = repository.cleanup_plan(target_bytes=repository.usage().total_bytes)
    assert not empty_plan.candidates


def test_source_and_partition_publication_preserve_reference_lineage(
    tmp_path: Path,
) -> None:
    repository = _repository(tmp_path)
    source_data = b"source!"
    _write_object(repository, "raw/provider/object.bin", source_data)
    source = _source_manifest(object_sha256=hashlib.sha256(source_data).hexdigest())
    with repository.acquire_admission(
        StorageRequest("source", 10_000), _quota()
    ) as lease:
        repository.publish_manifest(source, lease)

    partition_data = b"partition"
    _write_object(repository, "canonical/partition.bin", partition_data)
    partition = PartitionManifest(
        dataset_name="minute-bars",
        partition_key="session=2026-09-09",
        storage_path="canonical/partition.bin",
        object_sha256=hashlib.sha256(partition_data).hexdigest(),
        size_bytes=len(partition_data),
        record_count=1,
        schema_name="canonical-minute",
        schema_version="1.0.0",
        times=_times(),
        universe_snapshot_sha256=UNIVERSE_SHA256,
        source_manifest_ids=(source.manifest_id,),
        lineage=ManifestLineage(),
    )
    with repository.acquire_admission(
        StorageRequest("partition", 10_000), _quota()
    ) as lease:
        repository.publish_manifest(partition, lease)
    assert repository.load_manifest(partition.manifest_id) == partition
    assert repository.verify().passed
    with pytest.raises(StorageError, match="manifest type"):
        repository.load_manifest("invalid-id")
    with pytest.raises(StorageError, match=r"manifest type|SHA-256"):
        repository.load_manifest("source-../../raw/object")


def test_existing_manifest_conflict_and_interrupted_retry_are_not_overwritten(
    tmp_path: Path,
) -> None:
    repository = _repository(tmp_path)
    data = b"content"
    _write_object(repository, "raw/provider/object.bin", data)
    manifest = _source_manifest(object_sha256=hashlib.sha256(data).hexdigest())
    with repository.acquire_admission(
        StorageRequest("publish", 10_000), _quota()
    ) as lease:
        path = repository.publish_manifest(manifest, lease)
    path.chmod(0o600)
    path.write_bytes(b"different")
    with (
        repository.acquire_admission(
            StorageRequest("conflict", 10_000), _quota()
        ) as lease,
        pytest.raises(StorageError, match="different bytes"),
    ):
        repository.publish_manifest(manifest, lease)

    path.unlink()
    temporary = repository.root / "tmp" / f"{manifest.manifest_id}.manifest.partial"
    temporary.write_bytes(b"prior")
    with (
        repository.acquire_admission(
            StorageRequest("retry", 10_000), _quota()
        ) as lease,
        pytest.raises(StorageError, match="prior interrupted"),
    ):
        repository.publish_manifest(manifest, lease)


def test_verify_turns_root_integrity_failure_into_report(tmp_path: Path) -> None:
    repository = _repository(tmp_path)
    (repository.root / "raw" / "escape").symlink_to(tmp_path)
    report = repository.verify()
    assert not report.passed
    assert report.checked_manifests == 0
    assert report.errors[0].code == StorageErrorCode.SYMLINK_DETECTED.value


def test_verify_reports_aggregate_byte_overflow(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    repository = _repository(tmp_path)
    data = b"content"
    _write_object(repository, "raw/provider/object.bin", data)
    manifest = _source_manifest(object_sha256=hashlib.sha256(data).hexdigest())
    with repository.acquire_admission(
        StorageRequest("publish-overflow-fixture", 10_000), _quota()
    ) as lease:
        repository.publish_manifest(manifest, lease)
    usage = repository.usage()
    original = cast("Callable[..., int]", vars(repository_module)["_checked_add"])

    def fixed_usage(_repository: DataRepository) -> StorageUsage:
        return usage

    def overflow_verified_total(*values: int) -> int:
        if values == (0, len(data)):
            message = "synthetic verified-byte overflow"
            raise OverflowError(message)
        return original(*values)

    monkeypatch.setattr(DataRepository, "usage", fixed_usage)
    monkeypatch.setattr(repository_module, "_checked_add", overflow_verified_total)
    report = repository.verify()
    assert report.errors[0].code == StorageErrorCode.IO_FAILURE.value
    assert report.verified_bytes == 0


def test_cli_entry_points_execute_without_network(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    root = tmp_path / "cli"
    monkeypatch.setattr(sys, "argv", ["aegis-data", "--data-root", str(root), "usage"])
    assert data_cli.cli_main() == 0
    with (
        pytest.warns(RuntimeWarning, match="found in sys.modules"),
        pytest.raises(SystemExit) as exit_info,
    ):
        runpy.run_module("aegis_mx_research.data_cli", run_name="__main__")
    assert exit_info.value.code == 0


def test_usage_overflow_is_reported_as_io_failure(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    repository = _repository(tmp_path)
    original = cast("Callable[..., int]", vars(repository_module)["_checked_add"])

    def overflow(*values: int) -> int:
        if values and values[0] == 0:
            message = "synthetic overflow"
            raise OverflowError(message)
        return original(*values)

    monkeypatch.setattr(repository_module, "_checked_add", overflow)
    with pytest.raises(StorageError, match="signed 64-bit range") as error:
        repository.usage()
    assert error.value.code is StorageErrorCode.IO_FAILURE


def test_publication_covers_concurrent_and_post_link_faults(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    repository = _repository(tmp_path)
    data = b"content"
    _write_object(repository, "raw/provider/object.bin", data)
    manifest = _source_manifest(object_sha256=hashlib.sha256(data).hexdigest())
    original_link = os.link

    def concurrent_link(*_args: object, **_kwargs: object) -> None:
        raise FileExistsError

    monkeypatch.setattr(os, "link", concurrent_link)
    with (
        repository.acquire_admission(
            StorageRequest("concurrent-link", 10_000), _quota()
        ) as lease,
        pytest.raises(StorageError, match="appeared concurrently"),
    ):
        repository.publish_manifest(manifest, lease)

    partial = repository.root / "tmp" / f"{manifest.manifest_id}.manifest.partial"
    partial.unlink()
    monkeypatch.setattr(os, "link", original_link)

    def post_link_fault(stage: PublicationStage) -> None:
        if stage is PublicationStage.MANIFEST_LINKED:
            message = "post-link interruption"
            raise OSError(message)

    with (
        repository.acquire_admission(
            StorageRequest("post-link", 10_000), _quota()
        ) as lease,
        pytest.raises(StorageError, match="interrupted"),
    ):
        repository.publish_manifest(manifest, lease, fault_injector=post_link_fault)
    assert repository.manifest_path(manifest).exists()
    assert partial.exists()


def test_publication_preserves_typed_fault(
    tmp_path: Path,
) -> None:
    repository = _repository(tmp_path)
    data = b"content"
    _write_object(repository, "raw/provider/object.bin", data)
    manifest = _source_manifest(object_sha256=hashlib.sha256(data).hexdigest())

    def typed_fault(_stage: PublicationStage) -> None:
        raise StorageError(StorageErrorCode.IO_FAILURE, "typed publication fault")

    with (
        repository.acquire_admission(
            StorageRequest("typed", 10_000), _quota()
        ) as lease,
        pytest.raises(StorageError, match="typed publication fault"),
    ):
        repository.publish_manifest(manifest, lease, fault_injector=typed_fault)


@pytest.mark.parametrize("failure_errno", [errno.ELOOP, errno.EIO])
def test_admission_lock_open_failure_has_stable_code(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    failure_errno: int,
) -> None:
    repository = _repository(tmp_path)
    original_open = os.open
    lock_path = repository.root / ".admission.lock"

    def fail_lock(
        path: str | os.PathLike[str],
        flags: int,
        mode: int = 0o777,
        *,
        dir_fd: int | None = None,
    ) -> int:
        if Path(path) == lock_path:
            raise OSError(failure_errno, "synthetic open fault")
        return original_open(path, flags, mode, dir_fd=dir_fd)

    monkeypatch.setattr(os, "open", fail_lock)
    expected = (
        StorageErrorCode.SYMLINK_DETECTED
        if failure_errno == errno.ELOOP
        else StorageErrorCode.IO_FAILURE
    )
    with pytest.raises(StorageError, match="lock is unavailable") as error:
        repository.estimate(StorageRequest("lock-open", 1), _quota())
    assert error.value.code is expected


def test_admission_lock_rejects_hard_link_after_initialization(tmp_path: Path) -> None:
    repository = _repository(tmp_path)
    lock_path = repository.root / ".admission.lock"
    other = repository.root / "lock-evidence"
    lock_path.unlink()
    other.touch()
    os.link(other, lock_path)
    with pytest.raises(StorageError, match="unique regular file"):
        repository.estimate(StorageRequest("hard-link-lock", 1), _quota())


def test_inventory_reports_scan_cross_device_and_race_faults(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    repository = _repository(tmp_path)
    raw = repository.root / "raw"
    original_scandir = os.scandir

    def fail_raw(path: str | os.PathLike[str]) -> Iterator[os.DirEntry[str]]:
        if Path(path) == raw:
            message = "synthetic scan fault"
            raise OSError(message)
        return original_scandir(path)

    monkeypatch.setattr(os, "scandir", fail_raw)
    with pytest.raises(StorageError, match="cannot scan"):
        repository.usage()

    monkeypatch.setattr(os, "scandir", original_scandir)
    item = _write_object(repository, "raw/item.bin", b"value")
    original_path_stat = Path.stat

    def different_root_device(
        path: Path, *, follow_symlinks: bool = True
    ) -> os.stat_result:
        status = original_path_stat(path, follow_symlinks=follow_symlinks)
        if path == repository.root:
            values = list(status)
            values[2] = status.st_dev + 1
            return os.stat_result(values)
        return status

    monkeypatch.setattr(Path, "stat", different_root_device)
    with pytest.raises(StorageError, match="cross-device"):
        repository.usage()

    monkeypatch.setattr(Path, "stat", original_path_stat)
    original_lstat = os.lstat

    def changed_lstat(
        path: str | os.PathLike[str], *, dir_fd: int | None = None
    ) -> os.stat_result:
        status = original_lstat(path, dir_fd=dir_fd)
        if Path(path) == item:
            values = list(status)
            values[6] = status.st_size + 1
            return os.stat_result(values)
        return status

    monkeypatch.setattr(os, "lstat", changed_lstat)
    with pytest.raises(StorageError, match="changed during accounting"):
        repository.usage()


def test_internal_path_and_filesystem_probes_fail_closed(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    repository = _repository(tmp_path)
    with pytest.raises(StorageError, match="escapes"):
        repository._assert_safe_path(tmp_path / "outside")  # noqa: SLF001
    repository._assert_safe_path(repository.root / "raw" / "not-created" / "object")  # noqa: SLF001

    target = repository.root / "raw" / "link"
    target.symlink_to(tmp_path)
    with pytest.raises(StorageError, match="symlink"):
        repository._assert_safe_path(target / "object")  # noqa: SLF001

    def fail_statvfs(_path: str | os.PathLike[str]) -> os.statvfs_result:
        message = "synthetic statvfs fault"
        raise OSError(message)

    monkeypatch.setattr(os, "statvfs", fail_statvfs)
    with pytest.raises(StorageError, match="statvfs"):
        repository._probe_filesystem(repository.root)  # noqa: SLF001


def test_secure_file_helpers_cover_bounded_os_faults(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    repository = _repository(tmp_path)
    helper = repository.root / "raw" / "helper.bin"

    original_write = os.write

    def short_write(_descriptor: int, _payload: object) -> int:
        return 0

    monkeypatch.setattr(os, "write", short_write)
    with pytest.raises(OSError, match="short manifest write"):
        repository._create_exclusive_file(helper, b"value", 0o400)  # noqa: SLF001
    monkeypatch.setattr(os, "write", original_write)
    helper.unlink()

    missing = repository.root / "raw" / "missing.bin"
    with pytest.raises(StorageError, match="unavailable"):
        repository._validate_regular_file(missing)  # noqa: SLF001
    directory = repository.root / "raw"
    with pytest.raises(StorageError, match="regular file"):
        repository._validate_regular_file(directory)  # noqa: SLF001
    helper.touch()
    with pytest.raises(StorageError, match="empty"):
        repository._validate_regular_file(helper)  # noqa: SLF001

    sibling = repository.root / "raw" / "sibling.bin"
    sibling.write_bytes(b"value")
    helper.unlink()
    os.link(sibling, helper)
    with pytest.raises(StorageError, match="hard linked"):
        repository._validate_regular_file(helper)  # noqa: SLF001
    helper.unlink()
    sibling.unlink()
    helper.symlink_to(tmp_path / "outside")
    with pytest.raises(StorageError, match="symlink"):
        repository._validate_regular_file(helper)  # noqa: SLF001


def test_secure_read_and_hash_detect_open_size_short_read_and_races(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    repository = _repository(tmp_path)
    target = _write_object(repository, "raw/read.bin", b"value")
    original_open = os.open

    def fail_target_open(
        path: str | os.PathLike[str],
        flags: int,
        mode: int = 0o777,
        *,
        dir_fd: int | None = None,
    ) -> int:
        if Path(path) == target:
            message = "synthetic no-follow fault"
            raise OSError(message)
        return original_open(path, flags, mode, dir_fd=dir_fd)

    monkeypatch.setattr(os, "open", fail_target_open)
    with pytest.raises(StorageError, match="securely open"):
        repository._read_secure(target, 100)  # noqa: SLF001

    monkeypatch.setattr(os, "open", original_open)
    with pytest.raises(StorageError, match="size bound"):
        repository._read_secure(target, 1)  # noqa: SLF001

    original_read = os.read

    def short_read(_descriptor: int, _size: int) -> bytes:
        return b""

    monkeypatch.setattr(os, "read", short_read)
    with pytest.raises(StorageError, match="short read"):
        repository._read_secure(target, 100)  # noqa: SLF001
    monkeypatch.setattr(os, "read", original_read)

    original_fstat = os.fstat
    calls = 0

    def changed_fstat(descriptor: int) -> os.stat_result:
        nonlocal calls
        status = original_fstat(descriptor)
        calls += 1
        if calls == 2:
            values = list(status)
            values[6] = status.st_size + 1
            return os.stat_result(values)
        return status

    monkeypatch.setattr(os, "fstat", changed_fstat)
    with pytest.raises(StorageError, match="changed during read"):
        repository._read_secure(target, 100)  # noqa: SLF001

    calls = 0
    with pytest.raises(StorageError, match="SHA-256 verification"):
        repository._hash_secure_file(target)  # noqa: SLF001


def test_storage_json_schemas_match_runtime_contracts() -> None:
    schema_root = Path("schemas")
    legacy_policy_schema = json.loads(
        (schema_root / "data-storage-policy-v1.schema.json").read_text()
    )
    policy_v2_schema = json.loads(
        (schema_root / "data-storage-policy-v2.schema.json").read_text()
    )
    policy_schema = json.loads(
        (schema_root / "data-storage-policy-v3.schema.json").read_text()
    )
    manifest_schema = json.loads(
        (schema_root / "data-manifest-v1.schema.json").read_text()
    )
    audit_schema = json.loads(
        (schema_root / "data-storage-audit-v1.schema.json").read_text()
    )
    assert set(policy_schema["required"]) == set(StoragePolicy().to_dict())
    assert (
        legacy_policy_schema["properties"]["administrative_allocation_bytes_decimal"][
            "const"
        ]
        == 250_000_000_000
    )
    assert policy_v2_schema["properties"]["hard_root_bytes_decimal"]["maximum"] == (
        100_000_000_000
    )
    assert policy_schema["properties"]["hard_root_bytes_decimal"]["maximum"] == (
        800_000_000_000
    )
    assert (
        manifest_schema["$defs"]["sourceEnvelope"]["properties"][
            "manifest_schema_version"
        ]["const"]
        == repository_module.MANIFEST_SCHEMA_VERSION
    )
    assert audit_schema["properties"]["network_access_performed"]["const"] is False
