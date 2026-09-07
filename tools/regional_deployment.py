"""Fail-closed validation for non-hot-path regional Kubernetes assets."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from datetime import UTC, datetime
from pathlib import Path
from typing import Any, Final, cast

import yaml  # type: ignore[import-untyped]

REGIONAL_ROOT: Final = Path("infra/regional")
ZERO_SHA256: Final = "0" * 64
SHA256: Final = re.compile(r"^[0-9a-f]{64}$")
COMMIT_SHA: Final = re.compile(r"^[0-9a-f]{40}$")
IMAGE_DIGEST: Final = re.compile(r"^[^\s]+@sha256:([0-9a-f]{64})$")
CI_CERTIFICATE_IDENTITY: Final = re.compile(
    r"^https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/"
    r"\.github/workflows/container-release\.yml@refs/tags/v[0-9A-Za-z_.-]+$"
)
EXPECTED_NAMESPACES: Final = frozenset(
    {
        "aegis-intelligence",
        "aegis-models",
        "aegis-control",
        "aegis-observability",
        "aegis-research",
    }
)
EXPECTED_DEPLOYMENTS: Final = {
    "news-intelligence": "aegis-intelligence",
    "filings-intelligence": "aegis-intelligence",
    "macro-intelligence": "aegis-intelligence",
    "timesfm-service": "aegis-models",
    "options-analytics": "aegis-models",
    "model-registry": "aegis-control",
    "configuration-control-plane": "aegis-control",
    "dashboards": "aegis-observability",
    "research-api": "aegis-research",
}
SAFE_AUTOSCALING: Final = frozenset(
    {
        "news-intelligence",
        "filings-intelligence",
        "macro-intelligence",
        "options-analytics",
        "research-api",
    }
)
EXPECTED_ARTIFACTS: Final = frozenset(
    {"intelligence", "timeseries", "control-plane", "dashboards", "research", "backup"}
)
FORBIDDEN_WORKLOAD_TOKENS: Final = (
    "edge-core",
    "exchange-gateway",
    "order-gateway",
    "order-router",
    "smart-order-router",
    "pre-trade-risk",
    "order-management-system",
    "execution-engine",
)
MAX_YAML_BYTES: Final = 2_000_000
MAX_JSON_BYTES: Final = 1_000_000


class RegionalDeploymentError(RuntimeError):
    """Raised when a regional deployment asset violates the contract."""


class UniqueKeyLoader(yaml.SafeLoader):  # type: ignore[misc]
    """YAML loader that rejects duplicate mapping keys."""


def _construct_unique_mapping(
    loader: UniqueKeyLoader, node: yaml.MappingNode, *, deep: bool = False
) -> dict[object, object]:
    mapping: dict[object, object] = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=deep)
        if key in mapping:
            msg = f"duplicate YAML key: {key!r}"
            raise RegionalDeploymentError(msg)
        mapping[key] = loader.construct_object(value_node, deep=deep)
    return mapping


UniqueKeyLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG,
    _construct_unique_mapping,
)


def _require(condition: object, message: str) -> None:
    if not bool(condition):
        raise RegionalDeploymentError(message)


def _mapping(value: object, context: str) -> dict[str, Any]:
    _require(isinstance(value, dict), f"{context} must be an object")
    return cast("dict[str, Any]", value)


def _sequence(value: object, context: str) -> list[Any]:
    _require(isinstance(value, list), f"{context} must be a list")
    return cast("list[Any]", value)


def _load_yaml(path: Path) -> list[dict[str, Any]]:
    _require(path.is_file(), f"missing YAML asset: {path}")
    payload = path.read_bytes()
    _require(len(payload) <= MAX_YAML_BYTES, f"YAML asset is too large: {path}")
    try:
        documents = list(yaml.load_all(payload.decode("utf-8"), Loader=UniqueKeyLoader))
    except (UnicodeDecodeError, yaml.YAMLError) as error:
        raise RegionalDeploymentError(f"invalid YAML in {path}: {error}") from error
    result: list[dict[str, Any]] = []
    for index, document in enumerate(documents):
        if document is None:
            continue
        result.append(_mapping(document, f"{path} document {index}"))
    _require(result, f"YAML asset is empty: {path}")
    return result


def _load_json(path: Path) -> dict[str, Any]:
    _require(path.is_file(), f"missing JSON asset: {path}")
    payload = path.read_bytes()
    _require(len(payload) <= MAX_JSON_BYTES, f"JSON asset is too large: {path}")

    def reject_duplicates(pairs: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in pairs:
            if key in result:
                msg = f"duplicate JSON key: {key}"
                raise RegionalDeploymentError(msg)
            result[key] = value
        return result

    try:
        value = json.loads(payload, object_pairs_hook=reject_duplicates)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RegionalDeploymentError(f"invalid JSON in {path}: {error}") from error
    return _mapping(value, str(path))


def _documents(directory: Path) -> list[dict[str, Any]]:
    documents: list[dict[str, Any]] = []
    for path in sorted(directory.glob("*.yaml")):
        if path.name != "kustomization.yaml":
            documents.extend(_load_yaml(path))
    return documents


def _key(document: dict[str, Any]) -> tuple[str, str, str]:
    metadata = _mapping(document.get("metadata"), "metadata")
    return (
        str(document.get("kind", "")),
        str(metadata.get("namespace", "")),
        str(metadata.get("name", "")),
    )


def _by_kind(documents: list[dict[str, Any]], kind: str) -> list[dict[str, Any]]:
    return [document for document in documents if document.get("kind") == kind]


def _validate_kustomization(path: Path, regional_root: Path) -> None:
    document = _load_yaml(path)
    _require(len(document) == 1, f"{path} must contain one Kustomization")
    value = document[0]
    _require(value.get("kind") == "Kustomization", f"{path} has the wrong kind")
    references: list[str] = []
    references.extend(
        str(item) for item in _sequence(value.get("resources"), "resources")
    )
    references.extend(
        str(patch["path"])
        for patch in _sequence(value.get("patches", []), "patches")
        if isinstance(patch, dict) and "path" in patch
    )
    for reference in references:
        _require(
            not Path(reference).is_absolute(), f"absolute Kustomize path: {reference}"
        )
        resolved = (path.parent / reference).resolve()
        _require(
            resolved == regional_root or regional_root in resolved.parents,
            f"Kustomize reference escapes regional root: {reference}",
        )
        _require(resolved.exists(), f"missing Kustomize reference: {reference}")


def _environment(container: dict[str, Any]) -> dict[str, str]:
    result: dict[str, str] = {}
    for item in _sequence(container.get("env", []), "container env"):
        value = _mapping(item, "environment item")
        name = str(value.get("name", ""))
        _require(name not in result, f"duplicate environment variable: {name}")
        result[name] = str(value.get("value", ""))
    return result


def _validate_deployment(document: dict[str, Any]) -> None:
    _, namespace, name = _key(document)
    _require(
        EXPECTED_DEPLOYMENTS.get(name) == namespace, f"unexpected Deployment: {name}"
    )
    metadata = _mapping(document["metadata"], f"{name} metadata")
    labels = _mapping(metadata.get("labels"), f"{name} labels")
    _require(labels.get("aegis-mx.io/plane") == "regional", f"{name}: wrong plane")
    _require(
        labels.get("aegis-mx.io/execution-authority") == "false",
        f"{name}: execution authority is not denied",
    )
    expected_autoscaling = "safe" if name in SAFE_AUTOSCALING else "disabled"
    _require(
        labels.get("aegis-mx.io/autoscaling") == expected_autoscaling,
        f"{name}: unsafe autoscaling declaration",
    )
    spec = _mapping(document.get("spec"), f"{name} spec")
    _require(int(spec.get("replicas", 0)) >= 2, f"{name}: base requires two replicas")
    strategy = _mapping(spec.get("strategy"), f"{name} strategy")
    rolling = _mapping(strategy.get("rollingUpdate"), f"{name} rolling update")
    _require(
        strategy.get("type") == "RollingUpdate"
        and rolling.get("maxUnavailable") == 0
        and rolling.get("maxSurge") == 1,
        f"{name}: rolling update is not availability preserving",
    )
    template = _mapping(spec.get("template"), f"{name} template")
    pod_metadata = _mapping(template.get("metadata"), f"{name} pod metadata")
    pod_labels = _mapping(pod_metadata.get("labels"), f"{name} pod labels")
    _require(pod_labels == labels, f"{name}: pod and deployment labels differ")
    annotations = _mapping(pod_metadata.get("annotations"), f"{name} annotations")
    for annotation in (
        "aegis-mx.io/signature-bundle-sha256",
        "aegis-mx.io/configuration-sha256",
    ):
        _require(
            annotations.get(annotation) == ZERO_SHA256, f"{name}: unsafe base evidence"
        )
    pod = _mapping(template.get("spec"), f"{name} pod spec")
    _require(pod.get("serviceAccountName") == name, f"{name}: wrong service account")
    _require(pod.get("automountServiceAccountToken") is False, f"{name}: token mounted")
    _require(pod.get("enableServiceLinks") is False, f"{name}: service links enabled")
    _require(
        not any(
            pod.get(field, False) for field in ("hostIPC", "hostNetwork", "hostPID")
        ),
        f"{name}: host namespace enabled",
    )
    _require(
        int(pod.get("terminationGracePeriodSeconds", 0)) >= 30, f"{name}: short drain"
    )
    _require(bool(pod.get("affinity")), f"{name}: anti-affinity missing")
    _require(
        bool(pod.get("topologySpreadConstraints")), f"{name}: topology spread missing"
    )
    pod_security = _mapping(pod.get("securityContext"), f"{name} pod security")
    _require(pod_security.get("runAsNonRoot") is True, f"{name}: pod may run as root")
    _require(
        _mapping(pod_security.get("seccompProfile"), "seccomp").get("type")
        == "RuntimeDefault",
        f"{name}: pod seccomp missing",
    )
    containers = _sequence(pod.get("containers"), f"{name} containers")
    _require(len(containers) == 1, f"{name}: exactly one service container is required")
    container = _mapping(containers[0], f"{name} container")
    image_match = IMAGE_DIGEST.fullmatch(str(container.get("image", "")))
    _require(image_match is not None, f"{name}: image is not digest pinned")
    image_match = cast("re.Match[str]", image_match)
    _require(
        image_match.group(1) == ZERO_SHA256, f"{name}: base image must fail closed"
    )
    _require(
        str(container.get("image", "")).startswith("registry.invalid/"),
        f"{name}: unresolved base registry must be non-routable",
    )
    for probe in ("startupProbe", "livenessProbe", "readinessProbe"):
        _require(bool(container.get(probe)), f"{name}: {probe} missing")
    _require(bool(container.get("lifecycle")), f"{name}: graceful shutdown missing")
    resources = _mapping(container.get("resources"), f"{name} resources")
    for bound in ("requests", "limits"):
        values = _mapping(resources.get(bound), f"{name} {bound}")
        _require(
            all(key in values for key in ("cpu", "memory", "ephemeral-storage")),
            f"{name}: incomplete {bound}",
        )
    security = _mapping(container.get("securityContext"), f"{name} security")
    _require(
        security.get("allowPrivilegeEscalation") is False,
        f"{name}: privilege escalation",
    )
    _require(security.get("readOnlyRootFilesystem") is True, f"{name}: writable root")
    _require(security.get("runAsNonRoot") is True, f"{name}: container may run as root")
    _require(
        _mapping(security.get("capabilities"), "capabilities").get("drop") == ["ALL"],
        f"{name}: capabilities not dropped",
    )
    environment = _environment(container)
    _require(environment.get("AEGIS_SERVICE_ROLE") == name, f"{name}: role mismatch")
    _require(
        environment.get("AEGIS_SERVICE_IDENTITY")
        == f"spiffe://aegis-mx/unresolved/{name}",
        f"{name}: unresolved identity mismatch",
    )
    volumes = {
        str(item.get("name")): item
        for item in (
            _mapping(value, f"{name} volume")
            for value in _sequence(pod.get("volumes"), f"{name} volumes")
        )
    }
    identity = _mapping(volumes.get("workload-identity"), f"{name} identity volume")
    csi = _mapping(identity.get("csi"), f"{name} identity CSI")
    _require(
        csi.get("driver") == "secrets-store.csi.k8s.io" and csi.get("readOnly") is True,
        f"{name}: identity must use read-only Secrets Store CSI",
    )
    _require(
        "scratch" in volumes and "runtime-config" in volumes, f"{name}: volumes missing"
    )


def _validate_base(root: Path) -> list[dict[str, Any]]:
    base = root / "base"
    documents = _documents(base)
    keys = [_key(document) for document in documents]
    _require(len(keys) == len(set(keys)), "duplicate base Kubernetes resources")
    _require(not _by_kind(documents, "Secret"), "Secret values cannot be checked in")
    namespaces = _by_kind(documents, "Namespace")
    _require(
        {
            str(_mapping(item["metadata"], "namespace metadata")["name"])
            for item in namespaces
        }
        == EXPECTED_NAMESPACES,
        "regional namespace inventory mismatch",
    )
    for namespace in namespaces:
        metadata = _mapping(namespace["metadata"], "namespace metadata")
        labels = _mapping(metadata.get("labels"), "namespace labels")
        _require(
            labels.get("aegis-mx.io/plane") == "regional", "namespace plane missing"
        )
        _require(
            labels.get("pod-security.kubernetes.io/enforce") == "restricted",
            "namespace does not enforce restricted pod security",
        )
    deployments = _by_kind(documents, "Deployment")
    _require(
        {
            str(_mapping(item["metadata"], "deployment metadata")["name"])
            for item in deployments
        }
        == set(EXPECTED_DEPLOYMENTS),
        "regional deployment inventory mismatch",
    )
    for deployment in deployments:
        _validate_deployment(deployment)
    services = _by_kind(documents, "Service")
    _require(len(services) == len(EXPECTED_DEPLOYMENTS), "service inventory mismatch")
    for service in services:
        spec = _mapping(service.get("spec"), "service spec")
        _require(spec.get("type") == "ClusterIP", "regional service must be ClusterIP")
    policies = _by_kind(documents, "NetworkPolicy")
    default_denies = [item for item in policies if _key(item)[2] == "default-deny"]
    _require(
        len(default_denies) == len(EXPECTED_NAMESPACES),
        "default deny inventory mismatch",
    )
    for policy in default_denies:
        spec = _mapping(policy.get("spec"), "default deny policy")
        _require(
            spec.get("policyTypes") == ["Ingress", "Egress"], "incomplete default deny"
        )
        _require(
            "ingress" not in spec and "egress" not in spec,
            "default deny has exceptions",
        )
    _require(len(_by_kind(documents, "ResourceQuota")) == 5, "resource quotas missing")
    _require(len(_by_kind(documents, "LimitRange")) == 5, "limit ranges missing")
    config_maps = _by_kind(documents, "ConfigMap")
    _require(
        all(item.get("immutable") is True for item in config_maps), "mutable ConfigMap"
    )
    serialized = json.dumps(documents, sort_keys=True).lower()
    for token in FORBIDDEN_WORKLOAD_TOKENS:
        _require(
            token not in serialized, f"forbidden hot-path component present: {token}"
        )
    cron_jobs = _by_kind(documents, "CronJob")
    _require(
        len(cron_jobs) == 1 and _key(cron_jobs[0])[2] == "regional-metadata-backup",
        "backup job missing",
    )
    backup_spec = _mapping(cron_jobs[0].get("spec"), "backup spec")
    _require(backup_spec.get("suspend") is True, "base backup must start suspended")
    _require(
        backup_spec.get("concurrencyPolicy") == "Forbid", "backup concurrency unsafe"
    )
    return documents


def _patch_identities(path: Path, environment: str) -> None:
    documents = _load_yaml(path)
    namespaces = _by_kind(documents, "Namespace")
    _require(len(namespaces) == 5, f"{environment}: namespace patches missing")
    for namespace in namespaces:
        labels = _mapping(
            _mapping(namespace["metadata"], "metadata").get("labels"), "labels"
        )
        _require(
            labels.get("aegis-mx.io/environment") == environment,
            "environment label mismatch",
        )
    configs = _by_kind(documents, "ConfigMap")
    _require(len(configs) == 5, f"{environment}: runtime patches missing")
    for config in configs:
        data = _mapping(config.get("data"), "runtime patch")
        _require(
            data.get("AEGIS_ENVIRONMENT") == environment, "runtime environment mismatch"
        )
    deployments = _by_kind(documents, "Deployment")
    _require(
        len(deployments) == len(EXPECTED_DEPLOYMENTS),
        "identity patch inventory mismatch",
    )
    for deployment in deployments:
        name = _key(deployment)[2]
        spec = _mapping(deployment["spec"], "deployment patch spec")
        template = _mapping(spec["template"], "deployment patch template")
        pod = _mapping(template["spec"], "deployment patch pod")
        container = _mapping(_sequence(pod["containers"], "containers")[0], "container")
        identity = _environment(container).get("AEGIS_SERVICE_IDENTITY")
        _require(
            identity == f"spiffe://aegis-mx/{environment}/{name}",
            "identity patch mismatch",
        )


def _validate_overlays(root: Path) -> tuple[int, int]:
    local = root / "overlays/local"
    production = root / "overlays/production"
    _patch_identities(local / "environment.yaml", "local")
    _patch_identities(production / "environment.yaml", "production")
    local_kustomization = _load_yaml(local / "kustomization.yaml")[0]
    local_patches = _sequence(local_kustomization.get("patches"), "local patches")
    _require(
        any("replicas" in json.dumps(item) for item in local_patches),
        "local replica patch missing",
    )
    production_documents = _documents(production)
    _require(
        not _by_kind(production_documents, "Secret"),
        "production overlay cannot contain Secret values",
    )
    production_serialized = json.dumps(production_documents, sort_keys=True).lower()
    for token in FORBIDDEN_WORKLOAD_TOKENS:
        _require(
            token not in production_serialized,
            f"forbidden production component present: {token}",
        )
    hpas = _by_kind(production_documents, "HorizontalPodAutoscaler")
    _require(
        {_key(item)[2] for item in hpas} == SAFE_AUTOSCALING, "unsafe HPA inventory"
    )
    for hpa in hpas:
        spec = _mapping(hpa.get("spec"), "HPA spec")
        _require(
            int(spec.get("minReplicas", 0)) >= 2, "production HPA minimum is unsafe"
        )
        _require(
            int(spec.get("maxReplicas", 0)) > int(spec["minReplicas"]),
            "HPA range invalid",
        )
        behavior = _mapping(spec.get("behavior"), "HPA behavior")
        scale_down = _mapping(behavior.get("scaleDown"), "HPA scale down")
        _require(
            int(scale_down.get("stabilizationWindowSeconds", 0)) >= 600,
            "HPA scale down is unstable",
        )
    pdbs = _by_kind(production_documents, "PodDisruptionBudget")
    _require(
        {_key(item)[2] for item in pdbs} == set(EXPECTED_DEPLOYMENTS),
        "PDB inventory mismatch",
    )
    policies = _by_kind(production_documents, "ValidatingAdmissionPolicy")
    bindings = _by_kind(production_documents, "ValidatingAdmissionPolicyBinding")
    _require(
        len(policies) == 1 and len(bindings) == 1, "admission policy or binding missing"
    )
    policy_spec = _mapping(policies[0].get("spec"), "admission policy")
    _require(
        policy_spec.get("failurePolicy") == "Fail", "admission policy must fail closed"
    )
    validations = json.dumps(policy_spec.get("validations", []), sort_keys=True)
    _require(
        "signature-bundle-sha256" in validations, "signature admission check missing"
    )
    _require(
        "execution-authority" in validations,
        "execution authority admission check missing",
    )
    production_patches = _load_yaml(production / "production-workloads.yaml")
    timesfm = next(
        item for item in production_patches if _key(item)[2] == "timesfm-service"
    )
    timesfm_pod = _mapping(
        _mapping(_mapping(timesfm["spec"], "timesfm spec")["template"], "template")[
            "spec"
        ],
        "timesfm pod",
    )
    _require(
        timesfm_pod.get("runtimeClassName") == "nvidia", "TimesFM GPU runtime missing"
    )
    timesfm_container = _mapping(
        _sequence(timesfm_pod["containers"], "containers")[0], "container"
    )
    resources = _mapping(timesfm_container.get("resources"), "TimesFM resources")
    requests = _mapping(resources.get("requests"), "TimesFM requests")
    limits = _mapping(resources.get("limits"), "TimesFM limits")
    _require(
        requests.get("nvidia.com/gpu") == "1" and limits.get("nvidia.com/gpu") == "1",
        "TimesFM GPU request and limit must match",
    )
    backup = next(item for item in production_patches if item.get("kind") == "CronJob")
    _require(
        _mapping(backup.get("spec"), "backup patch").get("suspend") is False,
        "production backup disabled",
    )
    return len(hpas), len(pdbs)


def _asset_sha256(root: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        relative = path.relative_to(root).as_posix().encode("utf-8")
        digest.update(len(relative).to_bytes(4, "big"))
        digest.update(relative)
        payload = path.read_bytes()
        digest.update(len(payload).to_bytes(8, "big"))
        digest.update(payload)
    return digest.hexdigest()


def validate_assets(repository_root: Path) -> dict[str, object]:
    """Validate all checked-in regional deployment assets."""
    root = (repository_root / REGIONAL_ROOT).resolve()
    _require(root.is_dir(), f"regional deployment root is absent: {root}")
    for kustomization in sorted(root.rglob("kustomization.yaml")):
        _validate_kustomization(kustomization, root)
    documents = _validate_base(root)
    hpa_count, pdb_count = _validate_overlays(root)
    workload_count = len(_by_kind(documents, "Deployment")) + len(
        _by_kind(documents, "CronJob")
    )
    return {
        "schema_version": 1,
        "asset_sha256": _asset_sha256(root),
        "namespace_count": len(EXPECTED_NAMESPACES),
        "deployment_count": len(EXPECTED_DEPLOYMENTS),
        "workload_image_placeholders": workload_count,
        "hpa_count": hpa_count,
        "pdb_count": pdb_count,
        "production_apply_ready": False,
        "edge_workloads_included": False,
        "live_trading_capable": False,
    }


def validate_release_lock(path: Path) -> dict[str, object]:
    """Validate trusted signature-verifier evidence for one production release."""
    value = _load_json(path)
    expected_keys = {
        "schema_version",
        "environment",
        "source_commit",
        "generated_at_utc",
        "configuration_sha256",
        "verifier",
        "artifacts",
    }
    _require(set(value) == expected_keys, "release lock keys do not match schema v1")
    _require(value["schema_version"] == 1, "unsupported release lock schema")
    _require(
        value["environment"] == "production", "release lock is not production scoped"
    )
    _require(
        COMMIT_SHA.fullmatch(str(value["source_commit"])) is not None,
        "invalid source commit",
    )
    generated_at = str(value["generated_at_utc"])
    try:
        generated_time = datetime.fromisoformat(generated_at)
    except ValueError as error:
        raise RegionalDeploymentError("invalid release generation timestamp") from error
    _require(
        generated_at.endswith("Z") and generated_time.tzinfo == UTC,
        "release generation timestamp must be UTC",
    )
    configuration_sha = str(value["configuration_sha256"])
    _require(
        SHA256.fullmatch(configuration_sha) is not None
        and configuration_sha != ZERO_SHA256,
        "configuration digest is unresolved",
    )
    verifier = _mapping(value["verifier"], "verifier")
    _require(
        set(verifier) == {"certificate_identity", "oidc_issuer"},
        "verifier keys invalid",
    )
    _require(
        CI_CERTIFICATE_IDENTITY.fullmatch(str(verifier["certificate_identity"]))
        is not None,
        "untrusted certificate identity",
    )
    _require(
        verifier["oidc_issuer"] == "https://token.actions.githubusercontent.com",
        "untrusted OIDC issuer",
    )
    artifacts = _sequence(value["artifacts"], "artifacts")
    names: set[str] = set()
    for item in artifacts:
        artifact = _mapping(item, "artifact")
        _require(
            set(artifact)
            == {
                "name",
                "image",
                "signature_bundle_sha256",
                "sbom_sha256",
                "provenance_sha256",
                "verified",
            },
            "artifact keys invalid",
        )
        name = str(artifact["name"])
        _require(name not in names, f"duplicate release artifact: {name}")
        names.add(name)
        image_match = IMAGE_DIGEST.fullmatch(str(artifact["image"]))
        _require(
            image_match is not None and image_match.group(1) != ZERO_SHA256,
            f"{name}: unresolved image",
        )
        _require(
            str(artifact["image"]).startswith("ghcr.io/"),
            f"{name}: unapproved registry",
        )
        for field in ("signature_bundle_sha256", "sbom_sha256", "provenance_sha256"):
            digest = str(artifact[field])
            _require(
                SHA256.fullmatch(digest) is not None and digest != ZERO_SHA256,
                f"{name}: unresolved {field}",
            )
        _require(artifact["verified"] is True, f"{name}: signature is not verified")
    _require(names == EXPECTED_ARTIFACTS, "release artifact inventory mismatch")
    canonical = json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return {
        "schema_version": 1,
        "release_lock_sha256": hashlib.sha256(canonical).hexdigest(),
        "artifact_count": len(artifacts),
        "environment": "production",
        "verified": True,
    }


def main() -> int:
    """Run regional deployment validation commands."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path.cwd())
    subcommands = parser.add_subparsers(dest="command", required=True)
    subcommands.add_parser("validate-assets")
    release = subcommands.add_parser("validate-release-lock")
    release.add_argument("--lock", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        if arguments.command == "validate-assets":
            result = validate_assets(arguments.root.resolve())
        else:
            result = validate_release_lock(arguments.lock.resolve())
    except (OSError, RegionalDeploymentError) as error:
        parser.exit(1, f"regional deployment validation failed: {error}\n")
    print(json.dumps(result, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
