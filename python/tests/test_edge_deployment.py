"""Tests for fail-closed colocated edge deployment artifacts."""

from __future__ import annotations

import json
from dataclasses import replace
from typing import TYPE_CHECKING

import pytest

from tools import edge_deployment

if TYPE_CHECKING:
    from pathlib import Path


def test_all_profiles_and_assets_are_non_live() -> None:
    summary = edge_deployment.validate_assets()
    assert summary["profile_count"] == 6
    assert summary["service_count"] == 5
    assert summary["live_transmission_enabled"] is False
    assert summary["automatic_activation"] is False
    assert summary["missing_repository_executables"] == []
    for name in edge_deployment.PROFILE_NAMES:
        profile = edge_deployment.load_named_profile(name)
        assert profile.name == name
        assert profile.trading_mode in {"SIMULATION", "PAPER"}
        assert profile.live_transmission_enabled is False
        assert profile.automatic_activation is False


def test_profile_parser_rejects_live_and_overlapping_dedicated_cpus(
    tmp_path: Path,
) -> None:
    source = edge_deployment.PROFILE_ROOT / "paper.json"
    value = json.loads(source.read_text(encoding="utf-8"))
    value["live_transmission_enabled"] = True
    live_path = tmp_path / "live.json"
    live_path.write_text(json.dumps(value), encoding="utf-8")
    with pytest.raises(ValueError, match="live transmission"):
        edge_deployment.load_profile(live_path)

    value["live_transmission_enabled"] = False
    value["resources"]["cpu_affinity"]["journal"] = "2"
    overlap_path = tmp_path / "overlap.json"
    overlap_path.write_text(json.dumps(value), encoding="utf-8")
    with pytest.raises(ValueError, match="CPU sets overlap"):
        edge_deployment.load_profile(overlap_path)

    duplicate_path = tmp_path / "duplicate.json"
    duplicate_path.write_text('{"name":"development","name":"paper"}', encoding="utf-8")
    with pytest.raises(ValueError, match="duplicate key"):
        edge_deployment.load_profile(duplicate_path)


def test_render_is_deterministic_and_refuses_overwrite(tmp_path: Path) -> None:
    profile = edge_deployment.load_named_profile("paper")
    first = tmp_path / "first"
    second = tmp_path / "second"
    first_manifest = edge_deployment.render_profile(profile, first)
    second_manifest = edge_deployment.render_profile(profile, second)
    assert first_manifest == second_manifest
    assert (first / "environment/aegis-edge.conf").read_bytes() == (
        second / "environment/aegis-edge.conf"
    ).read_bytes()
    core_drop_in = first / ("systemd/aegis-edge-core.service.d/20-aegis-profile.conf")
    content = core_drop_in.read_text(encoding="utf-8")
    assert "CPUAffinity=4-11" in content
    assert "NUMAPolicy=bind" in content
    assert "LimitMEMLOCK=2147483648" in content
    environment = (first / "environment/aegis-edge.conf").read_text(encoding="utf-8")
    assert (
        "AEGIS_DEPLOYMENT_PROFILE_PATH="
        "/etc/aegis-mx/deployment/active-profile.json" in environment
    )
    nic_plan = json.loads(
        (first / "host/nic-queue-plan.json").read_text(encoding="utf-8")
    )
    assert nic_plan["queue_assignments"] == {
        "drop-copy": [3],
        "gateway": [2],
        "market-data-a": [0],
        "market-data-b": [1],
    }
    with pytest.raises(ValueError, match="absent or empty"):
        edge_deployment.render_profile(profile, first)


def test_host_validation_and_unresolved_staging_nic(tmp_path: Path) -> None:
    facts = edge_deployment.load_host_facts(
        edge_deployment.EDGE_ROOT / "host/host-facts.example.json"
    )
    result = edge_deployment.validate_host(
        edge_deployment.load_named_profile("paper"), facts
    )
    assert result["validated"] is True
    assert result["cpu_count_required"] == 12
    with pytest.raises(ValueError, match="NIC mapping is unresolved"):
        edge_deployment.validate_host(
            edge_deployment.load_named_profile("staging"), facts
        )

    staging = json.loads(
        (edge_deployment.PROFILE_ROOT / "staging.json").read_text(encoding="utf-8")
    )
    staging["nic"]["interface"] = "enp1s0f0"
    resolved_path = tmp_path / "resolved-staging.json"
    resolved_path.write_text(json.dumps(staging), encoding="utf-8")
    resolved = edge_deployment.load_profile(resolved_path)
    assert (
        edge_deployment.main(
            ["validate-profile", "--profile-path", resolved_path.as_posix()]
        )
        == 0
    )
    assert edge_deployment.validate_host(resolved, facts)["validated"] is True
    rendered = tmp_path / "rendered"
    edge_deployment.render_profile(resolved, rendered)
    assert (rendered / "deployment/active-profile.json").read_bytes() == (
        resolved_path.read_bytes()
    )


def _write_health(
    root: Path,
    *,
    service: str,
    mode: str,
    observed_ns: int,
    clock: bool = False,
) -> None:
    value: dict[str, object] = {
        "schema_version": 1,
        "service": service,
        "build_version": "test-build",
        "healthy": True,
        "ready": True,
        "configuration_sha256": "a" * 64,
        "mode": mode,
        "observed_process_monotonic_time_ns": observed_ns,
    }
    if clock:
        value.update(
            {
                "clock_state": "HEALTHY",
                "hardware_timestamp_available": True,
                "ptp_offset_ns": 100,
            }
        )
    root.mkdir(parents=True, exist_ok=True)
    (root / f"{service}.json").write_text(json.dumps(value), encoding="utf-8")


def test_health_is_fresh_mode_bound_and_clock_gated(tmp_path: Path) -> None:
    profile = edge_deployment.load_named_profile("paper")
    now = 10_000_000_000
    _write_health(
        tmp_path,
        service="edge-core",
        mode="PAPER",
        observed_ns=now - 100,
    )
    result = edge_deployment.check_health(
        profile, "edge-core", status_root=tmp_path, now_monotonic_ns=now
    )
    assert result["ready"] is True

    stale_now = now + profile.health.maximum_status_age_ns + 1
    with pytest.raises(ValueError, match="stale"):
        edge_deployment.check_health(
            profile,
            "edge-core",
            status_root=tmp_path,
            now_monotonic_ns=stale_now,
        )

    clock_root = tmp_path / "clock"
    _write_health(
        clock_root,
        service="clock-quality",
        mode="PAPER",
        observed_ns=now - 100,
        clock=True,
    )
    assert edge_deployment.check_health(
        profile,
        "clock-quality",
        status_root=clock_root,
        now_monotonic_ns=now,
    )["ready"]
    unsafe = json.loads((clock_root / "clock-quality.json").read_text(encoding="utf-8"))
    unsafe["clock_state"] = "UNSAFE"
    (clock_root / "clock-quality.json").write_text(json.dumps(unsafe), encoding="utf-8")
    with pytest.raises(ValueError, match="clock quality"):
        edge_deployment.check_health(
            profile,
            "clock-quality",
            status_root=clock_root,
            now_monotonic_ns=now,
        )


def test_aggregate_health_requires_one_configuration_hash(tmp_path: Path) -> None:
    profile = edge_deployment.load_named_profile("development")
    now = 20_000_000_000
    for service in edge_deployment.ROLE_UNITS:
        _write_health(
            tmp_path,
            service=service,
            mode="SIMULATION",
            observed_ns=now - 100,
        )
    assert edge_deployment.check_all_health(
        profile, status_root=tmp_path, now_monotonic_ns=now
    )["ready"]

    journal_path = tmp_path / "journal.json"
    journal = json.loads(journal_path.read_text(encoding="utf-8"))
    journal["configuration_sha256"] = "b" * 64
    journal_path.write_text(json.dumps(journal), encoding="utf-8")
    with pytest.raises(ValueError, match="one configuration hash"):
        edge_deployment.check_all_health(
            profile, status_root=tmp_path, now_monotonic_ns=now
        )


def test_rollback_package_is_reproducible_and_verified(tmp_path: Path) -> None:
    profile = edge_deployment.load_named_profile("production-disabled")
    first = tmp_path / "first.tar.gz"
    second = tmp_path / "second.tar.gz"
    edge_deployment.build_rollback_package(profile, first, 1_600_000_000)
    edge_deployment.build_rollback_package(profile, second, 1_600_000_000)
    assert first.read_bytes() == second.read_bytes()
    result = edge_deployment.verify_rollback_package(first)
    assert result["verified"] is True
    assert result["profile"] == "production-disabled"
    corrupted = tmp_path / "corrupted.tar.gz"
    corrupted.write_bytes(first.read_bytes()[:-32])
    assert (
        edge_deployment.main(["verify-rollback", "--package", corrupted.as_posix()])
        == 2
    )
    appended = tmp_path / "appended.tar.gz"
    appended.write_bytes(first.read_bytes() + b"\x00")
    assert (
        edge_deployment.main(["verify-rollback", "--package", appended.as_posix()]) == 2
    )
    paper_package = tmp_path / "paper-with-host-facts.tar.gz"
    edge_deployment.build_rollback_package(
        edge_deployment.load_named_profile("paper"),
        paper_package,
        1_600_000_000,
        edge_deployment.EDGE_ROOT / "host/host-facts.example.json",
    )
    assert edge_deployment.verify_rollback_package(paper_package)["verified"] is True


def test_inhibit_record_is_atomic_and_disabled(tmp_path: Path) -> None:
    path = tmp_path / "trading.inhibit"
    result = edge_deployment.write_inhibit("unit-failure", path)
    assert result["mode"] == "DISABLED"
    assert result["live_transmission_enabled"] is False
    assert json.loads(path.read_text(encoding="utf-8")) == result


def test_host_validator_rejects_insufficient_resources() -> None:
    facts = edge_deployment.load_host_facts(
        edge_deployment.EDGE_ROOT / "host/host-facts.example.json"
    )
    constrained = replace(facts, hugepage_total=0)
    with pytest.raises(ValueError, match="huge pages"):
        edge_deployment.validate_host(
            edge_deployment.load_named_profile("paper"), constrained
        )
