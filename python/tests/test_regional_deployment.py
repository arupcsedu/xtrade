"""Deployment-contract tests for non-hot-path regional Kubernetes assets."""

from __future__ import annotations

import copy
import json
import shutil
from pathlib import Path

import pytest

from tools.regional_deployment import (
    RegionalDeploymentError,
    validate_assets,
    validate_release_lock,
)

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
NONZERO_SHA256 = "1" * 64


def _regional_copy(tmp_path: Path) -> Path:
    root = tmp_path / "repository"
    shutil.copytree(REPOSITORY_ROOT / "infra/regional", root / "infra/regional")
    return root


def _release_lock() -> dict[str, object]:
    artifacts = [
        {
            "name": name,
            "image": f"ghcr.io/aegis-mx/aegis-mx/{name}@sha256:{NONZERO_SHA256}",
            "signature_bundle_sha256": "2" * 64,
            "sbom_sha256": "3" * 64,
            "provenance_sha256": "4" * 64,
            "verified": True,
        }
        for name in (
            "intelligence",
            "timeseries",
            "control-plane",
            "dashboards",
            "research",
            "backup",
        )
    ]
    return {
        "schema_version": 1,
        "environment": "production",
        "source_commit": "5" * 40,
        "generated_at_utc": "2026-09-05T12:00:00Z",
        "configuration_sha256": "6" * 64,
        "verifier": {
            "certificate_identity": (
                "https://github.com/aegis-mx/aegis-mx/"
                ".github/workflows/container-release.yml@refs/tags/v1.2.3"
            ),
            "oidc_issuer": "https://token.actions.githubusercontent.com",
        },
        "artifacts": artifacts,
    }


def _write_json(path: Path, value: dict[str, object]) -> None:
    path.write_text(json.dumps(value, sort_keys=True), encoding="utf-8")


def test_regional_assets_are_complete_non_live_and_fail_closed() -> None:
    result = validate_assets(REPOSITORY_ROOT)

    assert result["namespace_count"] == 5
    assert result["deployment_count"] == 9
    assert result["workload_image_placeholders"] == 10
    assert result["hpa_count"] == 5
    assert result["pdb_count"] == 9
    assert result["production_apply_ready"] is False
    assert result["edge_workloads_included"] is False
    assert result["live_trading_capable"] is False
    assert len(str(result["asset_sha256"])) == 64


def test_duplicate_yaml_keys_are_rejected(tmp_path: Path) -> None:
    root = _regional_copy(tmp_path)
    workloads = root / "infra/regional/base/workloads.yaml"
    value = workloads.read_text(encoding="utf-8")
    workloads.write_text(
        value.replace(
            "kind: Deployment\nmetadata:",
            "kind: Deployment\nkind: StatefulSet\nmetadata:",
            1,
        ),
        encoding="utf-8",
    )

    with pytest.raises(RegionalDeploymentError, match="duplicate YAML key"):
        validate_assets(root)


def test_execution_authority_and_network_policy_weakening_are_rejected(
    tmp_path: Path,
) -> None:
    authority_root = _regional_copy(tmp_path / "authority")
    workloads = authority_root / "infra/regional/base/workloads.yaml"
    value = workloads.read_text(encoding="utf-8")
    workloads.write_text(
        value.replace(
            'aegis-mx.io/execution-authority: "false"',
            'aegis-mx.io/execution-authority: "true"',
            1,
        ),
        encoding="utf-8",
    )
    with pytest.raises(RegionalDeploymentError, match="execution authority"):
        validate_assets(authority_root)

    network_root = _regional_copy(tmp_path / "network")
    policies = network_root / "infra/regional/base/network-policies.yaml"
    value = policies.read_text(encoding="utf-8")
    policies.write_text(
        value.replace(
            "spec: {podSelector: {}, policyTypes: [Ingress, Egress]}",
            "spec: {podSelector: {}, policyTypes: [Ingress, Egress], egress: [{}]}",
            1,
        ),
        encoding="utf-8",
    )
    with pytest.raises(RegionalDeploymentError, match="default deny has exceptions"):
        validate_assets(network_root)


def test_unsafe_autoscaling_inventory_is_rejected(tmp_path: Path) -> None:
    root = _regional_copy(tmp_path)
    autoscaling = root / "infra/regional/overlays/production/autoscaling.yaml"
    value = autoscaling.read_text(encoding="utf-8")
    autoscaling.write_text(
        value.replace(
            "metadata: {name: research-api, namespace: aegis-research}",
            "metadata: {name: timesfm-service, namespace: aegis-models}",
            1,
        ),
        encoding="utf-8",
    )

    with pytest.raises(RegionalDeploymentError, match="unsafe HPA inventory"):
        validate_assets(root)


def test_release_lock_binds_verified_digest_artifacts(tmp_path: Path) -> None:
    path = tmp_path / "release-lock.json"
    _write_json(path, _release_lock())

    first = validate_release_lock(path)
    second = validate_release_lock(path)

    assert first == second
    assert first["artifact_count"] == 6
    assert first["verified"] is True
    assert len(str(first["release_lock_sha256"])) == 64


@pytest.mark.parametrize(
    ("mutation", "message"),
    [
        ("zero_image", "unresolved image"),
        ("unverified", "signature is not verified"),
        ("registry", "unapproved registry"),
        ("timestamp", "timestamp must be UTC"),
        ("artifact_missing", "artifact inventory mismatch"),
    ],
)
def test_release_lock_rejects_untrusted_evidence(
    tmp_path: Path, mutation: str, message: str
) -> None:
    value = _release_lock()
    artifacts = copy.deepcopy(value["artifacts"])
    assert isinstance(artifacts, list)
    first = artifacts[0]
    assert isinstance(first, dict)
    if mutation == "zero_image":
        first["image"] = f"ghcr.io/aegis-mx/aegis-mx/intelligence@sha256:{'0' * 64}"
    elif mutation == "unverified":
        first["verified"] = False
    elif mutation == "registry":
        first["image"] = (
            f"registry.example.invalid/intelligence@sha256:{NONZERO_SHA256}"
        )
    elif mutation == "timestamp":
        value["generated_at_utc"] = "2026-09-05T08:00:00-04:00"
    else:
        artifacts.pop()
    value["artifacts"] = artifacts
    path = tmp_path / "release-lock.json"
    _write_json(path, value)

    with pytest.raises(RegionalDeploymentError, match=message):
        validate_release_lock(path)


def test_release_lock_rejects_duplicate_json_keys(tmp_path: Path) -> None:
    path = tmp_path / "release-lock.json"
    path.write_text('{"schema_version":1,"schema_version":1}', encoding="utf-8")

    with pytest.raises(RegionalDeploymentError, match="duplicate JSON key"):
        validate_release_lock(path)
