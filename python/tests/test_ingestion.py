"""Adversarial tests for provider-neutral bounded ingestion."""

from __future__ import annotations

import json
import os
import socket
from dataclasses import replace
from datetime import UTC, date, datetime, timedelta, timezone
from pathlib import Path
from typing import TYPE_CHECKING, cast

import aegis_mx_research.ingestion as ingestion_module
import pytest
from aegis_mx_research import (
    DECIMAL_GB,
    DataEntitlement,
    DataEntitlementState,
    DataProvider,
    DataRepository,
    DeterministicMockHttpS3Provider,
    DownloadCheckpoint,
    EnvironmentCredentialProvider,
    FetchContractError,
    FetchErrorCode,
    FetchRequest,
    FetchResult,
    FetchStatus,
    FilesystemReplayProvider,
    FileSystemState,
    IngestionCoordinator,
    ManifestTimeRange,
    MockFailure,
    MockObject,
    ObjectFetchResult,
    ProviderFetchError,
    ProviderHealthState,
    QuotaEvidence,
    RateLimitPolicy,
    ResumeMode,
    RetryPolicy,
    SecretValue,
    ShutdownSignal,
    SourceObject,
    StorageError,
    StorageErrorCode,
    SyntheticMinuteBarProvider,
    redact_structure,
    redact_text,
)

if TYPE_CHECKING:
    from collections.abc import Callable

UNIVERSE_SHA256 = "ab" * 32
POLICY_SHA256 = "cd" * 32
APPROVAL_SHA256 = "ef" * 32


def _repository(root: Path) -> DataRepository:
    return DataRepository(
        root,
        filesystem_probe=lambda _path: FileSystemState(
            500 * DECIMAL_GB, 200 * DECIMAL_GB
        ),
        git_worktree=Path.cwd(),
    )


def _quota(*, used: int = 0, authoritative: bool = True) -> QuotaEvidence:
    return QuotaEvidence(
        limit_bytes=250 * DECIMAL_GB,
        used_bytes=used,
        source="deterministic-test-quota",
        observed_at_utc="2026-09-09T12:00:00Z",
        authoritative=authoritative,
    )


def _request(
    provider_id: str,
    dataset_id: str,
    *,
    execute: bool = False,
    chunk_bytes: int = 16,
    maximum_object_bytes: int = 1_000_000,
    maximum_total_bytes: int = 2_000_000,
    maximum_concurrency: int = 2,
) -> FetchRequest:
    return FetchRequest(
        request_id="bounded-fetch-test",
        provider_id=provider_id,
        dataset_id=dataset_id,
        start_date=date(2026, 9, 8),
        end_date=date(2026, 9, 8),
        tickers=("AAPL",),
        universe_snapshot_sha256=UNIVERSE_SHA256,
        execute=execute,
        maximum_object_bytes=maximum_object_bytes,
        maximum_total_bytes=maximum_total_bytes,
        maximum_concurrency=maximum_concurrency,
        chunk_bytes=chunk_bytes,
        deterministic_seed=20260831,
    )


def _local_entitlement(provider_id: str, dataset_id: str) -> DataEntitlement:
    return DataEntitlement(
        provider_id,
        dataset_id,
        DataEntitlementState.LOCAL_TEST_ONLY,
    )


def _remote_entitlement(provider_id: str, dataset_id: str) -> DataEntitlement:
    return DataEntitlement(
        source_id=provider_id,
        dataset_id=dataset_id,
        state=DataEntitlementState.AUTHORIZED,
        network_access_authorized=True,
        policy_sha256=POLICY_SHA256,
        approval_record_sha256=APPROVAL_SHA256,
        expires_at_utc=datetime(2099, 1, 1, tzinfo=UTC),
    )


def _coordinator(
    repository: DataRepository,
    provider: DataProvider,
    entitlement: DataEntitlement,
    *,
    retry: RetryPolicy | None = None,
    shutdown: ShutdownSignal | None = None,
    credential_provider: EnvironmentCredentialProvider | None = None,
    sleep: Callable[[float], None] = lambda _seconds: None,
) -> IngestionCoordinator:
    return IngestionCoordinator(
        repository,
        provider,
        entitlement,
        rate_limit=RateLimitPolicy(
            requests_per_window=1_000_000,
            window_ns=1,
            maximum_wait_ns=1_000_000,
        ),
        retry=retry
        or RetryPolicy(
            maximum_attempts=3,
            base_delay_ns=0,
            maximum_delay_ns=0,
            maximum_jitter_ns=0,
        ),
        shutdown=shutdown,
        credential_provider=credential_provider,
        monotonic_ns=lambda: 0,
        sleep=sleep,
        utc_now=lambda: datetime(2026, 9, 9, tzinfo=UTC),
    )


def _mock_object(
    *,
    payload: bytes = b"first\nsecond\nthird\n",
    expected_sha256_override: str | None = None,
    malformed: bool = False,
    resume_mode: ResumeMode = ResumeMode.VERIFIED_RANGE,
) -> MockObject:
    return MockObject(
        locator="http/mock-bucket/aapl/2026-09-08",
        coverage_date=date(2026, 9, 8),
        tickers=("AAPL",),
        payload=payload,
        times=_times(),
        record_count=3,
        expected_sha256_override=expected_sha256_override,
        malformed=malformed,
        resume_mode=resume_mode,
    )


def _times() -> ManifestTimeRange:
    base = 1_789_000_000_000_000_000
    return ManifestTimeRange(
        event_time_min_ns=base,
        event_time_max_ns=base + 1,
        publication_time_min_ns=base + 2,
        publication_time_max_ns=base + 3,
        receive_time_min_ns=base + 4,
        receive_time_max_ns=base + 5,
        processing_time_min_ns=base + 6,
        processing_time_max_ns=base + 7,
        revision_time_min_ns=base + 2,
        revision_time_max_ns=base + 3,
    )


def _mock_provider(
    item: MockObject,
    *,
    failures: tuple[MockFailure, ...] = (),
    credential_reference: str | None = None,
    on_fetch: Callable[[str, int], None] | None = None,
    duplicate: bool = False,
) -> DeterministicMockHttpS3Provider:
    return DeterministicMockHttpS3Provider(
        provider_id="deterministic-mock",
        dataset_id="mock-minute-bars",
        objects=(item,),
        failures={item.locator: failures},
        credential_reference=credential_reference,
        on_fetch=on_fetch,
        duplicate_first_object=duplicate,
    )


def _filesystem_fixture(
    root: Path,
    *,
    nested: bool = False,
) -> tuple[FilesystemReplayProvider, SourceObject, Path, Path]:
    metadata_root = root / "nested" if nested else root
    metadata_root.mkdir(parents=True)
    payload = b"row-one\nrow-two\n"
    data_path = root / "AAPL.data"
    data_path.write_bytes(payload)
    source = replace(
        _mock_provider(_mock_object(payload=payload)).plan(
            _request("deterministic-mock", "mock-minute-bars")
        )[0],
        provider_id="filesystem-replay",
        dataset_id="filesystem-bars",
        locator="AAPL.data",
        record_count=2,
    )
    metadata = source.to_dict()
    metadata.pop("provider_id")
    metadata_path = metadata_root / "AAPL.source.json"
    metadata_path.write_text(json.dumps(metadata, sort_keys=True), encoding="utf-8")
    return (
        FilesystemReplayProvider(root, "filesystem-bars"),
        source,
        metadata_path,
        data_path,
    )


def test_dry_run_is_default_and_never_calls_remote_provider(tmp_path: Path) -> None:
    calls: list[int] = []
    item = _mock_object()
    provider = _mock_provider(item, on_fetch=lambda _locator, call: calls.append(call))
    entitlement = DataEntitlement(
        provider.provider_id,
        provider.dataset_id,
        DataEntitlementState.BLOCKED,
    )
    coordinator = _coordinator(_repository(tmp_path / "data"), provider, entitlement)
    plan = coordinator.plan(
        _request(provider.provider_id, provider.dataset_id), _quota()
    )

    assert plan.dry_run is True
    assert plan.to_dict()["network_access_performed"] is False
    result = coordinator.execute(plan, _quota())
    assert result.status is FetchStatus.PLANNED
    assert result.network_access_performed is False
    assert calls == []


def test_remote_execution_requires_exact_unexpired_authority(tmp_path: Path) -> None:
    provider = _mock_provider(_mock_object())
    request = _request(provider.provider_id, provider.dataset_id, execute=True)
    blocked = DataEntitlement(
        provider.provider_id,
        provider.dataset_id,
        DataEntitlementState.BLOCKED,
    )
    coordinator = _coordinator(_repository(tmp_path / "data"), provider, blocked)
    plan = coordinator.plan(request, _quota())
    with pytest.raises(FetchContractError, match="does not permit") as failure:
        coordinator.execute(plan, _quota())
    assert failure.value.code is FetchErrorCode.UNAUTHORIZED

    authorized = _remote_entitlement(provider.provider_id, provider.dataset_id)
    assert authorized.permits(remote=True, now_utc=datetime(2026, 9, 9, tzinfo=UTC))
    assert not authorized.permits(remote=True, now_utc=datetime(2100, 1, 1, tzinfo=UTC))
    assert authorized.to_dict()["approval_record_sha256"] == APPROVAL_SHA256
    assert authorized.evidence_version == APPROVAL_SHA256


@pytest.mark.parametrize(
    ("changes", "message"),
    [
        ({"network_access_authorized": True}, "cannot grant"),
        (
            {
                "state": DataEntitlementState.AUTHORIZED,
                "network_access_authorized": False,
            },
            "lacks network authority",
        ),
        (
            {
                "state": DataEntitlementState.AUTHORIZED,
                "network_access_authorized": True,
            },
            "lacks immutable evidence",
        ),
        (
            {
                "state": DataEntitlementState.AUTHORIZED,
                "network_access_authorized": True,
                "policy_sha256": POLICY_SHA256,
                "approval_record_sha256": APPROVAL_SHA256,
            },
            "lacks a UTC expiry",
        ),
    ],
)
def test_invalid_entitlement_states_fail_closed(
    changes: dict[str, object], message: str
) -> None:
    values: dict[str, object] = {
        "source_id": "deterministic-mock",
        "dataset_id": "mock-minute-bars",
        "state": DataEntitlementState.BLOCKED,
    }
    values.update(changes)
    with pytest.raises(FetchContractError, match=message):
        DataEntitlement(**values)  # type: ignore[arg-type]


def test_unknown_entitlement_enum_fails_closed() -> None:
    with pytest.raises(FetchContractError, match="state is unknown"):
        DataEntitlement(
            "deterministic-mock",
            "mock-minute-bars",
            "AUTHORIZED",  # type: ignore[arg-type]
        )


def test_synthetic_ingestion_is_deterministic_idempotent_and_verified(
    tmp_path: Path,
) -> None:
    provider = SyntheticMinuteBarProvider()
    repository = _repository(tmp_path / "data")
    coordinator = _coordinator(
        repository,
        provider,
        _local_entitlement(provider.provider_id, provider.dataset_id),
    )
    request = _request(
        provider.provider_id,
        provider.dataset_id,
        execute=True,
        chunk_bytes=65_536,
    )
    first_plan = coordinator.plan(request, _quota())
    second_plan = coordinator.plan(request, _quota())
    assert first_plan.plan_id == second_plan.plan_id
    assert first_plan.total_estimated_bytes > 0
    assert first_plan.admission.admitted
    assert first_plan.provider_health is ProviderHealthState.HEALTHY

    first = coordinator.execute(first_plan, _quota())
    assert first.status is FetchStatus.COMPLETED
    assert first.objects[0].status is FetchStatus.COMPLETED
    assert first.objects[0].manifest_id is not None
    assert first.network_access_performed is False
    assert repository.verify().passed

    second = coordinator.execute(second_plan, _quota())
    assert second.status is FetchStatus.COMPLETED
    assert second.objects[0].status is FetchStatus.ALREADY_PRESENT
    assert second.bytes_received == 0


def test_retry_scripts_are_bounded_seeded_and_socket_free(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    def deny_socket(*_args: object, **_kwargs: object) -> None:
        message = "unit test attempted network access"
        raise AssertionError(message)

    monkeypatch.setattr(socket, "socket", deny_socket)
    item = _mock_object()
    provider = _mock_provider(
        item,
        failures=(
            MockFailure.TIMEOUT,
            MockFailure.RATE_LIMIT,
            MockFailure.PARTIAL_RESPONSE,
        ),
    )
    delays: list[float] = []
    retry = RetryPolicy(
        maximum_attempts=4,
        base_delay_ns=10,
        maximum_delay_ns=1_000,
        maximum_jitter_ns=50,
        deterministic_seed=77,
    )
    coordinator = _coordinator(
        _repository(tmp_path / "data"),
        provider,
        _remote_entitlement(provider.provider_id, provider.dataset_id),
        retry=retry,
        sleep=delays.append,
    )
    request = _request(provider.provider_id, provider.dataset_id, execute=True)
    plan = coordinator.plan(request, _quota())
    result = coordinator.execute(plan, _quota())

    assert result.status is FetchStatus.COMPLETED
    assert result.network_access_performed
    assert result.objects[0].attempts >= 4
    assert len(result.objects[0].retry_delays_ns) == 3
    assert retry.delay_ns(1, plan.objects[0].object_id) == retry.delay_ns(
        1, plan.objects[0].object_id
    )
    assert delays


def test_provider_call_receives_the_bounded_request_timeout(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    item = _mock_object()
    provider = _mock_provider(item)
    original_fetch = provider.fetch_range
    observed: list[int] = []

    def capture_timeout(
        source: SourceObject,
        *,
        offset: int,
        maximum_bytes: int,
        timeout_ns: int,
        credential: SecretValue | None,
    ) -> ingestion_module.ProviderChunk:
        observed.append(timeout_ns)
        return original_fetch(
            source,
            offset=offset,
            maximum_bytes=maximum_bytes,
            timeout_ns=timeout_ns,
            credential=credential,
        )

    monkeypatch.setattr(provider, "fetch_range", capture_timeout)
    coordinator = _coordinator(
        _repository(tmp_path / "data"),
        provider,
        _remote_entitlement(provider.provider_id, provider.dataset_id),
    )
    request = replace(
        _request(provider.provider_id, provider.dataset_id, execute=True),
        request_timeout_ns=123_456_789,
    )
    result = coordinator.execute(coordinator.plan(request, _quota()), _quota())
    assert result.status is FetchStatus.COMPLETED
    assert observed
    assert set(observed) == {123_456_789}


@pytest.mark.parametrize(
    ("item", "failures", "expected_code"),
    [
        (
            _mock_object(expected_sha256_override="12" * 32),
            (),
            FetchErrorCode.HASH_MISMATCH,
        ),
        (_mock_object(malformed=True), (), FetchErrorCode.MALFORMED_OBJECT),
        (
            _mock_object(),
            (MockFailure.SUCCESS, MockFailure.CHANGED_OBJECT),
            FetchErrorCode.OBJECT_CHANGED,
        ),
        (
            _mock_object(),
            (MockFailure.OVERSIZED_OBJECT,),
            FetchErrorCode.OBJECT_TOO_LARGE,
        ),
    ],
)
def test_unverifiable_changed_and_oversized_objects_never_reach_raw(
    tmp_path: Path,
    item: MockObject,
    failures: tuple[MockFailure, ...],
    expected_code: FetchErrorCode,
) -> None:
    provider = _mock_provider(item, failures=failures)
    coordinator = _coordinator(
        _repository(tmp_path / expected_code.value.lower()),
        provider,
        _remote_entitlement(provider.provider_id, provider.dataset_id),
    )
    request = _request(
        provider.provider_id,
        provider.dataset_id,
        execute=True,
        chunk_bytes=8,
    )
    result = coordinator.execute(coordinator.plan(request, _quota()), _quota())
    outcome = result.objects[0]

    assert outcome.error_code is expected_code
    assert outcome.status in {FetchStatus.FAILED, FetchStatus.QUARANTINED}
    assert not any((coordinator.repository.root / "raw").rglob("*.bin"))
    if outcome.status is FetchStatus.QUARANTINED:
        assert outcome.quarantine_path is not None
        assert (coordinator.repository.root / outcome.quarantine_path).is_file()


def test_shutdown_checkpoint_resumes_by_verified_range(tmp_path: Path) -> None:
    shutdown = ShutdownSignal()
    item = _mock_object(payload=b"one\ntwo\nthree\n")

    def stop_after_first(_locator: str, call: int) -> None:
        if call == 1:
            shutdown.request()

    first_provider = _mock_provider(item, on_fetch=stop_after_first)
    repository = _repository(tmp_path / "data")
    request = _request(
        first_provider.provider_id,
        first_provider.dataset_id,
        execute=True,
        chunk_bytes=4,
    )
    first = _coordinator(
        repository,
        first_provider,
        _remote_entitlement(first_provider.provider_id, first_provider.dataset_id),
        shutdown=shutdown,
    )
    first_result = first.execute(first.plan(request, _quota()), _quota())
    assert first_result.status is FetchStatus.STOPPED
    checkpoint = first_result.objects[0].checkpoint_path
    assert checkpoint is not None
    assert (repository.root / checkpoint).is_file()

    second_provider = _mock_provider(item)
    second = _coordinator(
        repository,
        second_provider,
        _remote_entitlement(second_provider.provider_id, second_provider.dataset_id),
    )
    second_plan = second.plan(request, _quota())
    without_resume = second.execute(second_plan, _quota())
    assert without_resume.status is FetchStatus.FAILED
    assert without_resume.objects[0].error_code is FetchErrorCode.CHECKPOINT_CORRUPT
    resumed = second.execute(second_plan, _quota(), resume=True)
    assert resumed.status is FetchStatus.COMPLETED
    assert repository.verify().passed
    assert not (repository.root / checkpoint).exists()


def test_complete_object_restart_discards_verified_partial(tmp_path: Path) -> None:
    shutdown = ShutdownSignal()
    item = _mock_object(resume_mode=ResumeMode.COMPLETE_OBJECT_RESTART)
    first_provider = _mock_provider(
        item, on_fetch=lambda _locator, _call: shutdown.request()
    )
    repository = _repository(tmp_path / "data")
    request = _request(
        first_provider.provider_id,
        first_provider.dataset_id,
        execute=True,
        chunk_bytes=5,
    )
    first = _coordinator(
        repository,
        first_provider,
        _remote_entitlement(first_provider.provider_id, first_provider.dataset_id),
        shutdown=shutdown,
    )
    stopped = first.execute(first.plan(request, _quota()), _quota())
    assert stopped.status is FetchStatus.STOPPED

    second_provider = _mock_provider(item)
    second = _coordinator(
        repository,
        second_provider,
        _remote_entitlement(second_provider.provider_id, second_provider.dataset_id),
    )
    resumed = second.execute(second.plan(request, _quota()), _quota(), resume=True)
    assert resumed.status is FetchStatus.COMPLETED
    assert resumed.objects[0].bytes_received == len(item.payload)


def test_duplicate_plan_is_deduplicated_and_quota_denial_blocks_fetch(
    tmp_path: Path,
) -> None:
    calls: list[int] = []
    item = _mock_object()
    provider = _mock_provider(
        item,
        duplicate=True,
        on_fetch=lambda _locator, call: calls.append(call),
    )
    coordinator = _coordinator(
        _repository(tmp_path / "data"),
        provider,
        _remote_entitlement(provider.provider_id, provider.dataset_id),
    )
    request = _request(provider.provider_id, provider.dataset_id, execute=True)
    plan = coordinator.plan(request, _quota())
    assert plan.duplicate_object_count == 1
    assert len(plan.objects) == 1

    denied = coordinator.plan(request, _quota(used=250 * DECIMAL_GB))
    assert not denied.admission.admitted
    with pytest.raises(FetchContractError, match="did not pass"):
        coordinator.execute(denied, _quota(used=250 * DECIMAL_GB))
    assert calls == []


def test_checkpoint_authentication_and_mismatch_are_rejected(tmp_path: Path) -> None:
    provider = _mock_provider(_mock_object())
    coordinator = _coordinator(
        _repository(tmp_path / "data"),
        provider,
        _remote_entitlement(provider.provider_id, provider.dataset_id),
    )
    plan = coordinator.plan(
        _request(provider.provider_id, provider.dataset_id, execute=True), _quota()
    )
    source = plan.objects[0]
    checkpoint = DownloadCheckpoint(
        plan_id=plan.plan_id,
        request_id=plan.request.request_id,
        object_id=source.object_id,
        provider_id=source.provider_id,
        dataset_id=source.dataset_id,
        locator_sha256=__import__("hashlib")
        .sha256(source.locator.encode())
        .hexdigest(),
        version_id=source.version_id,
        expected_size_bytes=source.estimated_size_bytes,
        completed_bytes=0,
        prefix_sha256=__import__("hashlib").sha256(b"").hexdigest(),
        attempts=0,
        sequence=0,
    )
    assert DownloadCheckpoint.decode(checkpoint.encode()) == checkpoint
    tampered = json.loads(checkpoint.encode())
    tampered["completed_bytes_decimal"] = 1
    with pytest.raises(FetchContractError, match="SHA-256"):
        DownloadCheckpoint.decode(json.dumps(tampered).encode())


def test_environment_credentials_and_recursive_redaction() -> None:
    credential_text = bytes.fromhex(
        # Deterministic redaction fixture, not a credential.
        "63726564656e7469616c2d76616c75652d746861742d6d7573742d6e6f742d6c65616b"  # pragma: allowlist secret  # noqa: E501
    ).decode()
    provider = EnvironmentCredentialProvider(
        {"AEGIS_DATA_TEST_KEY": credential_text}  # pragma: allowlist secret
    )
    value = provider.get("AEGIS_DATA_TEST_KEY")
    assert isinstance(value, SecretValue)
    assert value.reveal() == credential_text
    assert str(value) == "[REDACTED]"
    assert repr(value) == "[REDACTED]"
    assert provider.get("AEGIS_DATA_MISSING") is None
    with pytest.raises(FetchContractError, match="allowlist"):
        provider.get("OTHER_KEY")

    raw = {
        "api_key": credential_text,  # pragma: allowlist secret
        "url": f"https://example.invalid/path?token={credential_text}&safe=yes",
        "nested": [f"Authorization: Bearer-{credential_text}"],
    }
    rendered = json.dumps(redact_structure(raw, (credential_text,)))
    assert credential_text not in rendered
    assert "safe=yes" in redact_text(cast("str", raw["url"]), (credential_text,))


def test_filesystem_replay_filters_and_fetches_manifested_object(
    tmp_path: Path,
) -> None:
    fixture_root = tmp_path / "fixture"
    provider, _, _, _ = _filesystem_fixture(fixture_root, nested=True)
    repository = _repository(tmp_path / "repository")
    coordinator = _coordinator(
        repository,
        provider,
        _local_entitlement(provider.provider_id, provider.dataset_id),
    )
    request = _request(
        provider.provider_id,
        provider.dataset_id,
        execute=True,
        chunk_bytes=4,
    )
    result = coordinator.execute(coordinator.plan(request, _quota()), _quota())
    assert result.status is FetchStatus.COMPLETED
    assert result.network_access_performed is False
    assert repository.verify().passed


def test_filesystem_replay_rejects_unsafe_roots_and_tree_entries(
    tmp_path: Path,
) -> None:
    with pytest.raises(FetchContractError, match="absolute"):
        FilesystemReplayProvider(Path("relative"), "filesystem-bars")
    actual = tmp_path / "actual"
    actual.mkdir()
    linked_root = tmp_path / "linked"
    linked_root.symlink_to(actual, target_is_directory=True)
    with pytest.raises(FetchContractError, match="non-symlink"):
        FilesystemReplayProvider(linked_root, "filesystem-bars")

    tree = tmp_path / "tree"
    provider, _, _, _ = _filesystem_fixture(tree)
    (tree / "unsafe-link").symlink_to(tmp_path)
    with pytest.raises(FetchContractError, match="contains a symlink"):
        provider.plan(_request(provider.provider_id, provider.dataset_id))
    (tree / "unsafe-link").unlink()
    fifo = tree / "special"
    os.mkfifo(fifo)
    with pytest.raises(FetchContractError, match="special file"):
        provider.plan(_request(provider.provider_id, provider.dataset_id))


def test_filesystem_replay_tree_walk_is_bounded_and_wraps_io_failures(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    entry_root = tmp_path / "entry-bound"
    provider, _source, _metadata, _data = _filesystem_fixture(entry_root)
    request = _request(provider.provider_id, provider.dataset_id)
    monkeypatch.setattr(ingestion_module, "MAX_REPLAY_TREE_ENTRIES", 1)
    with pytest.raises(FetchContractError, match="entry bound"):
        provider.plan(request)

    monkeypatch.setattr(ingestion_module, "MAX_REPLAY_TREE_ENTRIES", 10)
    monkeypatch.setattr(ingestion_module, "MAX_REPLAY_TREE_DEPTH", 0)
    deep_root = tmp_path / "depth-bound"
    deep_provider, _source, _metadata, _data = _filesystem_fixture(
        deep_root, nested=True
    )
    deep_request = _request(deep_provider.provider_id, deep_provider.dataset_id)
    with pytest.raises(FetchContractError, match="depth bound"):
        deep_provider.plan(deep_request)

    def broken_scandir(_path: object) -> object:
        message = "synthetic directory failure"
        raise OSError(message)

    monkeypatch.setattr(ingestion_module, "MAX_REPLAY_TREE_DEPTH", 16)
    monkeypatch.setattr(os, "scandir", broken_scandir)
    with pytest.raises(FetchContractError, match="inspected safely"):
        deep_provider.plan(deep_request)


@pytest.mark.parametrize(
    ("mutation", "message"),
    [
        ("oversized", "size bound"),
        ("invalid-json", "not valid JSON"),
        ("missing-field", "fields do not match"),
        ("invalid-date", "values are invalid"),
        ("bad-lineage-shape", "lineage is malformed"),
        ("bad-lineage-value", "lineage is invalid"),
        ("bad-times-shape", "time range is malformed"),
        ("bad-times-fields", "time fields"),
        ("bad-times-value", "time range is invalid"),
        ("size-mismatch", "identity or size"),
        ("path-traversal", "escapes its root"),
        ("missing-object", "locator is unavailable"),
        ("symlink-object", "contains a symlink"),
    ],
)
def test_filesystem_replay_rejects_malformed_sidecars(
    tmp_path: Path, mutation: str, message: str
) -> None:
    root = tmp_path / mutation
    provider, _, metadata_path, data_path = _filesystem_fixture(root)
    metadata = json.loads(metadata_path.read_text())
    if mutation == "oversized":
        metadata_path.write_bytes(b" " * (ingestion_module.MAX_CHECKPOINT_BYTES + 1))
    elif mutation == "invalid-json":
        metadata_path.write_text("not-json", encoding="utf-8")
    elif mutation == "missing-field":
        metadata.pop("version_id")
    elif mutation == "invalid-date":
        metadata["coverage_date"] = "not-a-date"
    elif mutation == "bad-lineage-shape":
        metadata["lineage"] = []
    elif mutation == "bad-lineage-value":
        metadata["lineage"]["relation"] = "UNKNOWN"
    elif mutation == "bad-times-shape":
        metadata["times"] = []
    elif mutation == "bad-times-fields":
        metadata["times"].pop("event_time_min_ns")
    elif mutation == "bad-times-value":
        metadata["times"]["event_time_min_ns"] = -1
    elif mutation == "size-mismatch":
        data_path.write_bytes(b"different-size")
    elif mutation == "path-traversal":
        metadata["locator"] = "../outside"
    elif mutation == "missing-object":
        data_path.unlink()
    elif mutation == "symlink-object":
        data_path.unlink()
        data_path.symlink_to(tmp_path / "outside")
    if mutation not in {
        "oversized",
        "invalid-json",
        "size-mismatch",
        "missing-object",
        "symlink-object",
    }:
        metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
    with pytest.raises(FetchContractError, match=message):
        provider.plan(_request(provider.provider_id, provider.dataset_id))


def test_filesystem_replay_provider_direct_failure_paths(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    provider, source, _, data_path = _filesystem_fixture(tmp_path / "fixture")
    request = _request(provider.provider_id, provider.dataset_id)
    with pytest.raises(FetchContractError, match="does not match"):
        provider.plan(replace(request, dataset_id="other-dataset"))
    with pytest.raises(FetchContractError, match="does not accept"):
        provider.fetch_range(
            source,
            offset=0,
            maximum_bytes=4,
            timeout_ns=1,
            credential=SecretValue("fixture"),
        )
    with pytest.raises(FetchContractError, match="record count"):
        provider.validate_payload(source, b"one\n")
    linked = provider.root / "linked-object"
    linked.symlink_to(data_path)
    with pytest.raises(FetchContractError, match="traverses a symlink"):
        provider._data_path("linked-object")
    linked.unlink()

    real_read = os.read

    def mutate_during_read(descriptor: int, count: int) -> bytes:
        body = real_read(descriptor, count)
        data_path.write_bytes(body + b"changed")
        return body

    monkeypatch.setattr(os, "read", mutate_during_read)
    with pytest.raises(ProviderFetchError, match="changed during read"):
        provider.fetch_range(
            source, offset=0, maximum_bytes=4, timeout_ns=1, credential=None
        )


def test_filesystem_metadata_read_is_race_and_link_safe(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    provider, _source, metadata_path, _data_path = _filesystem_fixture(
        tmp_path / "fixture"
    )
    request = _request(provider.provider_id, provider.dataset_id)
    hardlink = provider.root / "copy.source.json"
    os.link(metadata_path, hardlink)
    with pytest.raises(FetchContractError, match="identity check"):
        provider.plan(request)
    hardlink.unlink()

    real_open = os.open

    def fail_open(*_args: object, **_kwargs: object) -> int:
        message = "synthetic metadata open failure"
        raise OSError(message)

    monkeypatch.setattr(os, "open", fail_open)
    with pytest.raises(FetchContractError, match="read safely"):
        provider._read_metadata(metadata_path)
    monkeypatch.setattr(os, "open", real_open)

    real_read = os.read

    def mutate_metadata(descriptor: int, count: int) -> bytes:
        body = real_read(descriptor, count)
        metadata_path.write_bytes(body + b" ")
        return body

    monkeypatch.setattr(os, "read", mutate_metadata)
    with pytest.raises(FetchContractError, match="changed during read"):
        provider._read_metadata(metadata_path)


def test_plan_rejects_size_scope_identity_and_unavailable_provider(
    tmp_path: Path,
) -> None:
    item = _mock_object()
    provider = _mock_provider(item)
    coordinator = _coordinator(
        _repository(tmp_path / "data"),
        provider,
        _remote_entitlement(provider.provider_id, provider.dataset_id),
    )
    too_small = _request(
        provider.provider_id,
        provider.dataset_id,
        maximum_object_bytes=4,
        chunk_bytes=4,
    )
    with pytest.raises(FetchContractError, match="object exceeds"):
        coordinator.plan(too_small, _quota())

    mismatch = replace(
        _request(provider.provider_id, provider.dataset_id), dataset_id="other-data"
    )
    with pytest.raises(FetchContractError, match="identities do not match"):
        coordinator.plan(mismatch, _quota())

    missing = FilesystemReplayProvider(tmp_path / "missing", "filesystem-bars")
    missing_coordinator = _coordinator(
        _repository(tmp_path / "other-data"),
        missing,
        _local_entitlement(missing.provider_id, missing.dataset_id),
    )
    with pytest.raises(FetchContractError, match="unavailable"):
        missing_coordinator.plan(
            _request(missing.provider_id, missing.dataset_id), _quota()
        )


def test_retry_exhaustion_retains_checkpoint_and_redacts_provider_error(
    tmp_path: Path,
) -> None:
    item = _mock_object()
    provider = _mock_provider(
        item,
        failures=(MockFailure.TIMEOUT, MockFailure.TIMEOUT, MockFailure.TIMEOUT),
    )
    coordinator = _coordinator(
        _repository(tmp_path / "data"),
        provider,
        _remote_entitlement(provider.provider_id, provider.dataset_id),
    )
    request = _request(provider.provider_id, provider.dataset_id, execute=True)
    result = coordinator.execute(coordinator.plan(request, _quota()), _quota())
    outcome = result.objects[0]
    assert result.status is FetchStatus.FAILED
    assert outcome.error_code is FetchErrorCode.TIMEOUT
    assert outcome.attempts == 3
    assert outcome.checkpoint_path is not None
    assert (coordinator.repository.root / outcome.checkpoint_path).is_file()


def test_source_object_and_request_validation_reject_ambiguous_inputs() -> None:
    provider = SyntheticMinuteBarProvider()
    with pytest.raises(FetchContractError, match="duplicates"):
        replace(
            _request(provider.provider_id, provider.dataset_id),
            tickers=("AAPL", "AAPL"),
        )
    source = provider.plan(_request(provider.provider_id, provider.dataset_id))[0]
    with pytest.raises(FetchContractError, match="credentials"):
        replace(source, locator="https://example.invalid/object?token=value")
    with pytest.raises(FetchContractError, match="expected_sha256"):
        replace(source, expected_sha256="bad")
    with pytest.raises(FetchContractError, match="outside"):
        RetryPolicy(maximum_attempts=2).delay_ns(2, source.object_id)


def test_low_level_contract_bounds_fail_closed() -> None:
    with pytest.raises(FetchContractError, match="retry_after_ns"):
        ProviderFetchError(
            FetchErrorCode.TIMEOUT, "bad retry", retryable=True, retry_after_ns=-1
        )
    with pytest.raises(FetchContractError, match="identifier"):
        ingestion_module._require_identifier("UPPER", "identifier")
    with pytest.raises(FetchContractError, match="byte range"):
        ingestion_module._require_byte_count(-1, "bytes")
    with pytest.raises(FetchContractError, match="overflow"):
        ingestion_module._checked_sum([(1 << 63) - 1, 1])
    with pytest.raises(FetchContractError, match="explicitly UTC"):
        ingestion_module._utc_nanoseconds(datetime(2026, 1, 1))  # noqa: DTZ001
    with pytest.raises(FetchContractError, match="date range"):
        ingestion_module._date_range(date(2026, 1, 2), date(2026, 1, 1))
    with pytest.raises(FetchContractError, match="malformed"):
        SecretValue("")
    assert redact_structure(7) == 7
    assert redact_text("http://[invalid") == "[REDACTED]"
    assert redact_text("safe", ("",)) == "safe"


def test_policy_and_request_constructor_bounds() -> None:
    with pytest.raises(FetchContractError, match="expiry must be UTC"):
        replace(
            _remote_entitlement("deterministic-mock", "mock-minute-bars"),
            expires_at_utc=datetime(2099, 1, 1, tzinfo=timezone(timedelta(hours=1))),
        )
    with pytest.raises(FetchContractError, match="rate exceeds"):
        RateLimitPolicy(requests_per_window=1_000_001)
    with pytest.raises(FetchContractError, match="maximum_attempts"):
        RetryPolicy(maximum_attempts=0)
    with pytest.raises(FetchContractError, match="base retry"):
        RetryPolicy(base_delay_ns=2, maximum_delay_ns=1)
    with pytest.raises(FetchContractError, match="retry seed"):
        RetryPolicy(deterministic_seed=True)

    baseline = _request("deterministic-mock", "mock-minute-bars")
    invalid_requests = (
        ({"tickers": ()}, "ticker filter"),
        ({"tickers": ("bad",)}, "malformed"),
        ({"chunk_bytes": 17_000_000}, "chunk_bytes"),
        ({"request_timeout_ns": 0}, "request_timeout_ns"),
        ({"request_timeout_ns": 300_000_000_001}, "implementation bound"),
        ({"maximum_concurrency": 33}, "maximum_concurrency"),
        ({"deterministic_seed": True}, "request seed"),
    )
    for changes, message in invalid_requests:
        with pytest.raises(FetchContractError, match=message):
            replace(baseline, **changes)


def test_source_object_constructor_bounds_and_optional_hash() -> None:
    source = SyntheticMinuteBarProvider().plan(
        _request("synthetic-minute-bars", "synthetic-minute-bars")
    )[0]
    for changes, message in (
        ({"source_version": ""}, "source_version"),
        ({"tickers": ()}, "ticker coverage"),
        ({"tickers": ("bad",)}, "coverage is malformed"),
        ({"estimated_size_bytes": 0}, "estimated_size_bytes"),
        ({"resume_mode": "VERIFIED_RANGE"}, "recovery metadata"),
        ({"lineage": object()}, "recovery metadata"),
    ):
        with pytest.raises(FetchContractError, match=message):
            replace(source, **changes)
    without_hash = replace(source, expected_sha256=None)
    assert without_hash.expected_sha256 is None


def test_provider_health_is_typed_and_rechecked_before_execution(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    provider = _mock_provider(_mock_object())
    coordinator = _coordinator(
        _repository(tmp_path / "degraded"),
        provider,
        _remote_entitlement(provider.provider_id, provider.dataset_id),
    )
    request = _request(provider.provider_id, provider.dataset_id, execute=True)
    plan = coordinator.plan(request, _quota())
    monkeypatch.setattr(provider, "health", lambda: ProviderHealthState.DEGRADED)
    with pytest.raises(FetchContractError, match="does not permit execution"):
        coordinator.execute(plan, _quota())

    monkeypatch.setattr(provider, "health", lambda: "HEALTHY")
    with pytest.raises(FetchContractError, match="unknown health"):
        coordinator.plan(request, _quota())


def test_checkpoint_rejects_all_malformed_envelopes() -> None:
    checkpoint = DownloadCheckpoint(
        plan_id=POLICY_SHA256,
        request_id="bounded-fetch-test",
        object_id=f"object-{APPROVAL_SHA256}",
        provider_id="deterministic-mock",
        dataset_id="mock-minute-bars",
        locator_sha256=UNIVERSE_SHA256,
        version_id="version-1",
        expected_size_bytes=10,
        completed_bytes=0,
        prefix_sha256=UNIVERSE_SHA256,
        attempts=0,
        sequence=0,
    )
    for changes, message in (
        ({"object_id": "bad"}, "object_id"),
        ({"completed_bytes": 11}, "completion exceeds"),
        ({"version_id": ""}, "version_id"),
        ({"schema_version": "2.0.0"}, "schema version"),
    ):
        with pytest.raises(FetchContractError, match=message):
            replace(checkpoint, **changes)
    for encoded, message in (
        (b"", "size"),
        (b"not-json", "valid JSON"),
        (b"[]", "JSON object"),
        (b'{"checkpoint_sha256":"bad"}', "fields"),
    ):
        with pytest.raises(FetchContractError, match=message):
            DownloadCheckpoint.decode(encoded)
    wrong_type = json.loads(checkpoint.encode())
    wrong_type["attempts"] = "zero"
    body = {
        key: value for key, value in wrong_type.items() if key != "checkpoint_sha256"
    }
    wrong_type["checkpoint_sha256"] = (
        __import__("hashlib")
        .sha256(
            json.dumps(
                body, ensure_ascii=True, separators=(",", ":"), sort_keys=True
            ).encode()
        )
        .hexdigest()
    )
    with pytest.raises(FetchContractError, match="byte range"):
        DownloadCheckpoint.decode(json.dumps(wrong_type).encode())


def test_remote_credential_boundary_requires_external_resolution(
    tmp_path: Path,
) -> None:
    item = _mock_object()
    provider = _mock_provider(item, credential_reference="AEGIS_DATA_TEST_KEY")
    request = _request(provider.provider_id, provider.dataset_id, execute=True)
    entitlement = _remote_entitlement(provider.provider_id, provider.dataset_id)
    without_interface = _coordinator(
        _repository(tmp_path / "missing-interface"), provider, entitlement
    )
    plan = without_interface.plan(request, _quota())
    with pytest.raises(FetchContractError, match="credential interface"):
        without_interface.execute(plan, _quota())

    missing = _coordinator(
        _repository(tmp_path / "missing-value"),
        provider,
        entitlement,
        credential_provider=EnvironmentCredentialProvider({}),
    )
    with pytest.raises(FetchContractError, match="credential is unavailable"):
        missing.execute(missing.plan(request, _quota()), _quota())

    available = _coordinator(
        _repository(tmp_path / "available"),
        provider,
        entitlement,
        credential_provider=EnvironmentCredentialProvider(
            {"AEGIS_DATA_TEST_KEY": "fixture-value"}  # pragma: allowlist secret
        ),
    )
    assert (
        available.execute(available.plan(request, _quota()), _quota()).status
        is FetchStatus.COMPLETED
    )


@pytest.mark.parametrize(
    ("failure", "expected"),
    [
        (MockFailure.BAD_OFFSET, FetchErrorCode.PARTIAL_RESPONSE),
        (MockFailure.EMPTY_RESPONSE, FetchErrorCode.PARTIAL_RESPONSE),
        (MockFailure.BAD_COMPLETION, FetchErrorCode.PARTIAL_RESPONSE),
        (MockFailure.NONRETRYABLE, FetchErrorCode.OBJECT_CHANGED),
    ],
)
def test_invalid_provider_ranges_fail_closed(
    tmp_path: Path, failure: MockFailure, expected: FetchErrorCode
) -> None:
    item = _mock_object()
    provider = _mock_provider(item, failures=(failure,))
    coordinator = _coordinator(
        _repository(tmp_path / failure.value.lower()),
        provider,
        _remote_entitlement(provider.provider_id, provider.dataset_id),
    )
    request = _request(provider.provider_id, provider.dataset_id, execute=True)
    result = coordinator.execute(coordinator.plan(request, _quota()), _quota())
    assert result.objects[0].error_code is expected


def test_provider_retry_after_and_local_rate_wait_are_bounded(tmp_path: Path) -> None:
    item = _mock_object()
    throttled = _mock_provider(item, failures=(MockFailure.RATE_LIMIT,))
    retry = RetryPolicy(
        maximum_attempts=2,
        base_delay_ns=0,
        maximum_delay_ns=0,
        maximum_jitter_ns=0,
    )
    coordinator = _coordinator(
        _repository(tmp_path / "retry-after"),
        throttled,
        _remote_entitlement(throttled.provider_id, throttled.dataset_id),
        retry=retry,
    )
    request = _request(throttled.provider_id, throttled.dataset_id, execute=True)
    result = coordinator.execute(coordinator.plan(request, _quota()), _quota())
    assert result.objects[0].error_code is FetchErrorCode.RETRY_EXHAUSTED

    limited = _mock_provider(item)
    limited_coordinator = IngestionCoordinator(
        _repository(tmp_path / "local-limit"),
        limited,
        _remote_entitlement(limited.provider_id, limited.dataset_id),
        rate_limit=RateLimitPolicy(1, 100, 1),
        retry=RetryPolicy(
            maximum_attempts=1,
            base_delay_ns=0,
            maximum_delay_ns=0,
            maximum_jitter_ns=0,
        ),
        monotonic_ns=lambda: 0,
        sleep=lambda _seconds: None,
        utc_now=lambda: datetime(2026, 9, 9, tzinfo=UTC),
    )
    small_chunks = _request(
        limited.provider_id, limited.dataset_id, execute=True, chunk_bytes=4
    )
    result = limited_coordinator.execute(
        limited_coordinator.plan(small_chunks, _quota()), _quota()
    )
    assert result.objects[0].error_code is FetchErrorCode.RATE_LIMITED


def test_unknown_hash_and_record_count_mismatch_paths(tmp_path: Path) -> None:
    item = replace(_mock_object(), omit_expected_sha256=True)
    provider = _mock_provider(item)
    coordinator = _coordinator(
        _repository(tmp_path / "no-hash"),
        provider,
        _remote_entitlement(provider.provider_id, provider.dataset_id),
    )
    request = _request(provider.provider_id, provider.dataset_id, execute=True)
    result = coordinator.execute(coordinator.plan(request, _quota()), _quota())
    assert result.status is FetchStatus.COMPLETED

    wrong_count_item = replace(_mock_object(), validated_record_count_override=2)
    wrong_count = _mock_provider(wrong_count_item)
    wrong_coordinator = _coordinator(
        _repository(tmp_path / "count"),
        wrong_count,
        _remote_entitlement(wrong_count.provider_id, wrong_count.dataset_id),
    )
    wrong_request = _request(
        wrong_count.provider_id, wrong_count.dataset_id, execute=True
    )
    quarantined = wrong_coordinator.execute(
        wrong_coordinator.plan(wrong_request, _quota()), _quota()
    )
    assert quarantined.objects[0].error_code is FetchErrorCode.MALFORMED_OBJECT


def test_synthetic_provider_validation_and_weekend_filter() -> None:
    provider = SyntheticMinuteBarProvider()
    baseline = _request(provider.provider_id, provider.dataset_id)
    weekend = replace(
        baseline,
        start_date=date(2026, 9, 5),
        end_date=date(2026, 9, 7),
    )
    objects = provider.plan(weekend)
    assert len(objects) == 1
    source = objects[0]
    chunk = provider.fetch_range(
        source, offset=0, maximum_bytes=32, timeout_ns=1, credential=None
    )
    assert chunk.data
    with pytest.raises(FetchContractError, match="does not accept"):
        provider.fetch_range(
            source,
            offset=0,
            maximum_bytes=32,
            timeout_ns=1,
            credential=SecretValue("fixture"),
        )
    with pytest.raises(FetchContractError, match="record count"):
        provider.validate_payload(source, b"{}\n")
    valid_payload = ingestion_module._synthetic_payload(
        source.tickers[0], source.coverage_date, baseline.deterministic_seed
    )
    lines = valid_payload.splitlines()
    broken_json = b"not-json\n" + b"\n".join(lines[1:]) + b"\n"
    with pytest.raises(FetchContractError, match="malformed JSON"):
        provider.validate_payload(source, broken_json)
    changed = json.loads(lines[0])
    changed["symbol"] = "MSFT"
    wrong_symbol = (
        json.dumps(changed, separators=(",", ":"), sort_keys=True).encode()
        + b"\n"
        + b"\n".join(lines[1:])
        + b"\n"
    )
    with pytest.raises(FetchContractError, match="out-of-scope"):
        provider.validate_payload(source, wrong_symbol)
    with pytest.raises(FetchContractError, match="does not match"):
        provider.plan(replace(baseline, dataset_id="other-dataset"))


def test_plan_rejects_empty_out_of_scope_count_total_and_hash_collision(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    item = _mock_object()
    provider = _mock_provider(item)
    entitlement = _remote_entitlement(provider.provider_id, provider.dataset_id)
    request = _request(provider.provider_id, provider.dataset_id)

    with pytest.raises(FetchContractError, match="no objects"):
        _coordinator(_repository(tmp_path / "empty"), provider, entitlement).plan(
            replace(request, start_date=date(2026, 9, 9), end_date=date(2026, 9, 9)),
            _quota(),
        )

    source = provider.plan(request)[0]
    monkeypatch.setattr(
        provider, "plan", lambda _request: (replace(source, tickers=("MSFT",)),)
    )
    with pytest.raises(FetchContractError, match="out-of-scope"):
        _coordinator(_repository(tmp_path / "scope"), provider, entitlement).plan(
            request, _quota()
        )

    monkeypatch.setattr(provider, "plan", lambda _request: (source,) * 20_001)
    with pytest.raises(FetchContractError, match="object-count"):
        _coordinator(_repository(tmp_path / "count"), provider, entitlement).plan(
            request, _quota()
        )

    second_item = replace(item, locator="http/mock-bucket/aapl/second")
    two_provider = DeterministicMockHttpS3Provider(
        provider_id=provider.provider_id,
        dataset_id=provider.dataset_id,
        objects=(item, second_item),
    )
    too_small_total = _request(
        provider.provider_id,
        provider.dataset_id,
        maximum_total_bytes=len(item.payload) + 1,
    )
    with pytest.raises(FetchContractError, match="total-byte"):
        _coordinator(_repository(tmp_path / "total"), two_provider, entitlement).plan(
            too_small_total, _quota()
        )

    monkeypatch.setattr(
        SourceObject,
        "object_id",
        property(lambda _self: f"object-{'12' * 32}"),
    )
    with pytest.raises(FetchContractError, match="conflicting object identity"):
        _coordinator(
            _repository(tmp_path / "collision"), two_provider, entitlement
        ).plan(request, _quota())


def test_execution_rejects_invalid_clock_changed_plan_and_storage_failure(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    item = _mock_object()
    provider = _mock_provider(item)
    entitlement = _remote_entitlement(provider.provider_id, provider.dataset_id)
    request = _request(provider.provider_id, provider.dataset_id, execute=True)
    repository = _repository(tmp_path / "clock")
    bad_clock = IngestionCoordinator(
        repository,
        provider,
        entitlement,
        utc_now=lambda: datetime(2026, 9, 9),  # noqa: DTZ001
    )
    with pytest.raises(FetchContractError, match="clock is not UTC"):
        bad_clock.execute(bad_clock.plan(request, _quota()), _quota())

    changed_provider = _mock_provider(item)
    changed = _coordinator(
        _repository(tmp_path / "changed-plan"), changed_provider, entitlement
    )
    plan = changed.plan(request, _quota())
    changed_provider._objects = (replace(item, payload=b"changed\nobject\nbytes\n"),)
    with pytest.raises(FetchContractError, match="no longer matches"):
        changed.execute(plan, _quota())

    broken_repository = _repository(tmp_path / "storage")
    broken = _coordinator(broken_repository, _mock_provider(item), entitlement)
    broken_plan = broken.plan(request, _quota())

    def deny_admission(*_args: object, **_kwargs: object) -> None:
        raise StorageError(StorageErrorCode.IO_FAILURE, "synthetic storage fault")

    monkeypatch.setattr(broken_repository, "acquire_admission", deny_admission)
    with pytest.raises(FetchContractError, match="storage operation failed"):
        broken.execute(broken_plan, _quota())


def test_shutdown_before_batch_marks_every_object_without_fetch(tmp_path: Path) -> None:
    shutdown = ShutdownSignal()
    shutdown.request()
    item = _mock_object()
    provider = _mock_provider(item)
    coordinator = _coordinator(
        _repository(tmp_path / "data"),
        provider,
        _remote_entitlement(provider.provider_id, provider.dataset_id),
        shutdown=shutdown,
    )
    request = _request(provider.provider_id, provider.dataset_id, execute=True)
    result = coordinator.execute(coordinator.plan(request, _quota()), _quota())
    assert result.status is FetchStatus.STOPPED
    assert result.objects[0].attempts == 0


def test_corrupt_checkpoint_and_stale_checkpoint_publication_fail_closed(
    tmp_path: Path,
) -> None:
    shutdown = ShutdownSignal()
    item = _mock_object()
    first_provider = _mock_provider(
        item, on_fetch=lambda _locator, _call: shutdown.request()
    )
    repository = _repository(tmp_path / "corrupt")
    request = _request(
        first_provider.provider_id,
        first_provider.dataset_id,
        execute=True,
        chunk_bytes=4,
    )
    first = _coordinator(
        repository,
        first_provider,
        _remote_entitlement(first_provider.provider_id, first_provider.dataset_id),
        shutdown=shutdown,
    )
    stopped = first.execute(first.plan(request, _quota()), _quota())
    checkpoint_path = repository.root / cast("str", stopped.objects[0].checkpoint_path)
    staged_path = (
        repository.root
        / "tmp"
        / "downloads"
        / f"{first.plan(request, _quota()).objects[0].object_id}.partial"
    )
    staged_path.write_bytes(staged_path.read_bytes() + b"tamper")
    second_provider = _mock_provider(item)
    second = _coordinator(
        repository,
        second_provider,
        _remote_entitlement(second_provider.provider_id, second_provider.dataset_id),
    )
    corrupt = second.execute(second.plan(request, _quota()), _quota(), resume=True)
    assert corrupt.objects[0].error_code is FetchErrorCode.CHECKPOINT_CORRUPT

    fresh_repository = _repository(tmp_path / "stale-new")
    fresh = _coordinator(
        fresh_repository,
        second_provider,
        _remote_entitlement(second_provider.provider_id, second_provider.dataset_id),
    )
    plan = fresh.plan(request, _quota())
    stale = (
        fresh_repository.root
        / "tmp"
        / "checkpoints"
        / f"{plan.objects[0].object_id}.new"
    )
    stale.parent.mkdir(parents=True)
    stale.write_bytes(b"stale")
    failed = fresh.execute(plan, _quota())
    assert failed.objects[0].error_code is FetchErrorCode.CHECKPOINT_CORRUPT
    assert checkpoint_path.is_file()


def test_existing_raw_without_manifest_is_recovered_idempotently(
    tmp_path: Path,
) -> None:
    item = _mock_object()
    provider = _mock_provider(item)
    repository = _repository(tmp_path / "data")
    coordinator = _coordinator(
        repository,
        provider,
        _remote_entitlement(provider.provider_id, provider.dataset_id),
    )
    request = _request(provider.provider_id, provider.dataset_id, execute=True)
    plan = coordinator.plan(request, _quota())
    first = coordinator.execute(plan, _quota())
    manifest_id = cast("str", first.objects[0].manifest_id)
    repository.manifest_path(repository.load_manifest(manifest_id)).unlink()

    recovered = coordinator.execute(plan, _quota())
    assert recovered.objects[0].status is FetchStatus.ALREADY_PRESENT
    assert repository.load_manifest(manifest_id).manifest_id == manifest_id


def test_publication_and_quarantine_storage_faults_are_reported(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    item = _mock_object()
    provider = _mock_provider(item)
    repository = _repository(tmp_path / "publication")
    coordinator = _coordinator(
        repository,
        provider,
        _remote_entitlement(provider.provider_id, provider.dataset_id),
    )
    request = _request(provider.provider_id, provider.dataset_id, execute=True)
    plan = coordinator.plan(request, _quota())

    def fail_manifest(*_args: object, **_kwargs: object) -> None:
        raise StorageError(StorageErrorCode.IO_FAILURE, "manifest write fault")

    monkeypatch.setattr(repository, "publish_manifest", fail_manifest)
    result = coordinator.execute(plan, _quota())
    assert result.objects[0].error_code is FetchErrorCode.IO_FAILURE

    bad_item = _mock_object(expected_sha256_override="34" * 32)
    bad_provider = _mock_provider(bad_item)
    bad_repository = _repository(tmp_path / "quarantine")
    bad = _coordinator(
        bad_repository,
        bad_provider,
        _remote_entitlement(bad_provider.provider_id, bad_provider.dataset_id),
    )
    bad_plan = bad.plan(
        _request(bad_provider.provider_id, bad_provider.dataset_id, execute=True),
        _quota(),
    )

    def fail_object(*_args: object, **_kwargs: object) -> None:
        raise StorageError(StorageErrorCode.IO_FAILURE, "object write fault")

    monkeypatch.setattr(bad_repository, "publish_staged_object", fail_object)
    quarantined = bad.execute(bad_plan, _quota())
    assert quarantined.objects[0].error_code is FetchErrorCode.IO_FAILURE


def test_internal_file_guards_detect_symlink_short_write_and_changed_read(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    directory = tmp_path / "internal"
    directory.mkdir()
    target = directory / "target"
    target.write_bytes(b"payload")
    linked = directory / "linked"
    linked.symlink_to(target)
    with pytest.raises(FetchContractError, match="integrity bounds"):
        IngestionCoordinator._read_bounded(linked, 100)

    append_target = directory / "append"
    append_target.write_bytes(b"")
    real_write = os.write
    monkeypatch.setattr(os, "write", lambda _fd, _value: 0)
    with pytest.raises(FetchContractError, match="short"):
        IngestionCoordinator._append_staged(append_target, b"value")
    monkeypatch.setattr(os, "write", real_write)

    checkpoint = DownloadCheckpoint(
        plan_id=POLICY_SHA256,
        request_id="bounded-fetch-test",
        object_id=f"object-{APPROVAL_SHA256}",
        provider_id="deterministic-mock",
        dataset_id="mock-minute-bars",
        locator_sha256=UNIVERSE_SHA256,
        version_id="version-1",
        expected_size_bytes=1,
        completed_bytes=0,
        prefix_sha256=UNIVERSE_SHA256,
        attempts=0,
        sequence=0,
    )
    checkpoint_path = directory / "checkpoint.json"
    checkpoint_path.with_suffix(".new").write_bytes(b"stale")
    with pytest.raises(FetchContractError, match="stale checkpoint"):
        IngestionCoordinator._save_checkpoint(checkpoint_path, checkpoint)

    changing = directory / "changing"
    changing.write_bytes(b"data")
    real_read = os.read

    def change_while_reading(descriptor: int, count: int) -> bytes:
        payload = real_read(descriptor, count)
        changing.write_bytes(payload + b"x")
        return payload

    monkeypatch.setattr(os, "read", change_while_reading)
    with pytest.raises(FetchContractError, match="changed during read"):
        IngestionCoordinator._read_bounded(changing, 100)


def test_result_serialization_and_checkpoint_type_failure_are_covered() -> None:
    object_result = ObjectFetchResult(
        object_id=f"object-{APPROVAL_SHA256}",
        status=FetchStatus.FAILED,
        bytes_received=7,
        attempts=2,
        error_code=FetchErrorCode.TIMEOUT,
        error_message="https://user:password@example.invalid/object?token=secret",  # pragma: allowlist secret  # noqa: E501
    )
    result = FetchResult(
        plan_id=POLICY_SHA256,
        request_id="bounded-fetch-test",
        status=FetchStatus.FAILED,
        objects=(object_result,),
        network_access_performed=True,
        deterministic_seed=20260831,
    )
    serialized = result.to_dict()
    assert serialized["bytes_received_decimal"] == 7
    assert "password" not in json.dumps(serialized)
    assert serialized["objects"][0]["error_code"] == "TIMEOUT"  # type: ignore[index]

    checkpoint = DownloadCheckpoint(
        plan_id=POLICY_SHA256,
        request_id="bounded-fetch-test",
        object_id=f"object-{APPROVAL_SHA256}",
        provider_id="deterministic-mock",
        dataset_id="mock-minute-bars",
        locator_sha256=UNIVERSE_SHA256,
        version_id="version-1",
        expected_size_bytes=1,
        completed_bytes=0,
        prefix_sha256=UNIVERSE_SHA256,
        attempts=0,
        sequence=0,
    )
    envelope = json.loads(checkpoint.encode())
    envelope["version_id"] = 7
    unsigned = {
        key: value for key, value in envelope.items() if key != "checkpoint_sha256"
    }
    envelope["checkpoint_sha256"] = ingestion_module._sha256(
        ingestion_module._canonical_bytes(unsigned)
    )
    with pytest.raises(FetchContractError, match="field types"):
        DownloadCheckpoint.decode(json.dumps(envelope).encode())


def test_mock_provider_mismatch_and_unexpected_failure_are_fail_closed(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    item = _mock_object()
    provider = _mock_provider(item)
    with pytest.raises(FetchContractError, match="does not match mock"):
        provider.plan(_request("another-provider", provider.dataset_id))

    coordinator = _coordinator(
        _repository(tmp_path / "runtime-failure"),
        provider,
        _remote_entitlement(provider.provider_id, provider.dataset_id),
    )
    request = _request(provider.provider_id, provider.dataset_id, execute=True)
    plan = coordinator.plan(request, _quota())

    def unexpected_failure(*_args: object, **_kwargs: object) -> None:
        message = "unexpected provider failure"
        raise RuntimeError(message)

    monkeypatch.setattr(provider, "fetch_range", unexpected_failure)
    result = coordinator.execute(plan, _quota())
    assert result.objects[0].error_code is FetchErrorCode.IO_FAILURE
    assert result.objects[0].error_message == "unexpected provider failure"


def test_corrupted_existing_source_object_is_rejected(tmp_path: Path) -> None:
    item = _mock_object()
    provider = _mock_provider(item)
    repository = _repository(tmp_path / "data")
    coordinator = _coordinator(
        repository,
        provider,
        _remote_entitlement(provider.provider_id, provider.dataset_id),
    )
    request = _request(provider.provider_id, provider.dataset_id, execute=True)
    plan = coordinator.plan(request, _quota())
    first = coordinator.execute(plan, _quota())
    raw_path = repository.root / cast("str", first.objects[0].storage_path)
    raw_path.write_bytes(b"x" * raw_path.stat().st_size)

    with pytest.raises(FetchContractError, match="failed verification"):
        coordinator.execute(plan, _quota())


def test_checkpoint_publication_detects_short_write(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    checkpoint = DownloadCheckpoint(
        plan_id=POLICY_SHA256,
        request_id="bounded-fetch-test",
        object_id=f"object-{APPROVAL_SHA256}",
        provider_id="deterministic-mock",
        dataset_id="mock-minute-bars",
        locator_sha256=UNIVERSE_SHA256,
        version_id="version-1",
        expected_size_bytes=1,
        completed_bytes=0,
        prefix_sha256=UNIVERSE_SHA256,
        attempts=0,
        sequence=0,
    )
    monkeypatch.setattr(os, "write", lambda _fd, _value: 0)
    with pytest.raises(FetchContractError, match="short checkpoint write"):
        IngestionCoordinator._save_checkpoint(tmp_path / "checkpoint.json", checkpoint)
