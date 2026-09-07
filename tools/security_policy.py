"""Validate repository security invariants without external credentials."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any, Final, cast

import yaml  # type: ignore[import-untyped]

ACTION_PIN: Final = re.compile(r"^[^@\s]+@[0-9a-f]{40}$")
IMAGE_DIGEST: Final = re.compile(r"^[^\s]+@sha256:[0-9a-f]{64}$")
REQUIRED_SECURITY_DOCS: Final = (
    "docs/security/threat-model.md",
    "docs/security/trust-boundaries.md",
    "docs/security/incident-response.md",
)
SENSITIVE_LOG_TOKENS: Final = (
    "BROKER_API_KEY",
    "BROKER_PASSWORD",
    "EXCHANGE_API_KEY",
    "EXCHANGE_PASSWORD",
    "EXCHANGE_PRIVATE_KEY",
)
EXPECTED_TIMESERIES_DEPLOYMENTS: Final = 3
TLS_SECRET_MODE: Final = 0o440


class SecurityPolicyError(RuntimeError):
    """Raised when a checked security invariant is absent or unsafe."""


def _require(condition: object, message: str) -> None:
    if not bool(condition):
        raise SecurityPolicyError(message)


def check_workflows(root: Path) -> int:
    """Require immutable action references and least-privilege default permissions."""
    count = 0
    for path in sorted((root / ".github/workflows").glob("*.yml")):
        payload = path.read_text(encoding="utf-8")
        document = cast("dict[str, Any]", yaml.safe_load(payload))
        permissions = document.get("permissions")
        _require(isinstance(permissions, dict), f"{path}: explicit permissions missing")
        for line in payload.splitlines():
            stripped = line.strip()
            if not stripped.startswith("uses:") and " uses:" not in f" {stripped}":
                continue
            reference = stripped.split("uses:", 1)[1].strip().split(" #", 1)[0]
            _require(
                ACTION_PIN.fullmatch(reference) is not None,
                f"{path}: unpinned action {reference}",
            )
            count += 1
    _require(count > 0, "no GitHub Actions were inspected")
    return count


def check_timeseries_manifest(root: Path) -> int:
    """Require mTLS secret mounts, non-root containers, and default-deny networking."""
    path = root / "infra/timeseries_forecast/kubernetes.yaml"
    documents = tuple(yaml.safe_load_all(path.read_text(encoding="utf-8")))
    deployments = [item for item in documents if item.get("kind") == "Deployment"]
    _require(
        len(deployments) == EXPECTED_TIMESERIES_DEPLOYMENTS,
        "expected three time-series deployments",
    )
    for deployment in deployments:
        spec = deployment["spec"]["template"]["spec"]
        _require(
            spec.get("automountServiceAccountToken") is False,
            "service account token must be disabled",
        )
        _require(
            spec.get("enableServiceLinks") is False, "service links must be disabled"
        )
        _require(
            not any(
                spec.get(name, False) for name in ("hostIPC", "hostNetwork", "hostPID")
            ),
            "host namespaces must be disabled",
        )
        pod_security = spec.get("securityContext", {})
        _require(
            pod_security.get("runAsNonRoot") is True,
            "pod security context may run as root",
        )
        _require(
            pod_security.get("seccompProfile", {}).get("type") == "RuntimeDefault",
            "pod does not use the runtime-default seccomp profile",
        )
        containers = spec.get("containers", [])
        _require(
            len(containers) == 1, "deployment must have one bounded service container"
        )
        container = containers[0]
        _require(
            IMAGE_DIGEST.fullmatch(container.get("image", "")) is not None,
            "container image is not digest pinned",
        )
        security = container.get("securityContext", {})
        _require(
            security.get("allowPrivilegeEscalation") is False,
            "privilege escalation is enabled",
        )
        _require(
            security.get("readOnlyRootFilesystem") is True,
            "root filesystem is writable",
        )
        _require(security.get("runAsNonRoot") is True, "container may run as root")
        _require(
            security.get("capabilities", {}).get("drop") == ["ALL"],
            "Linux capabilities are not fully dropped",
        )
        env = {item["name"]: item.get("value", "") for item in container.get("env", [])}
        _require(
            str(env.get("AEGIS_SERVICE_IDENTITY", "")).startswith("spiffe://aegis-mx/"),
            "service identity is absent",
        )
        _require(
            bool(env.get("AEGIS_TLS_CLIENT_BINDINGS")),
            "client RBAC bindings are absent",
        )
        mounts = {item["name"]: item for item in container.get("volumeMounts", [])}
        _require(
            mounts.get("mtls", {}).get("readOnly") is True,
            "mTLS secret mount is not read-only",
        )
        volumes = {item["name"]: item for item in spec.get("volumes", [])}
        secret = volumes.get("mtls", {}).get("secret", {})
        _require(
            secret.get("optional") is False and bool(secret.get("secretName")),
            "mTLS secret is optional or unnamed",
        )
        _require(
            secret.get("defaultMode") == TLS_SECRET_MODE,
            "mTLS secret permissions must be group-readable and not world-readable",
        )
    policies = [item for item in documents if item.get("kind") == "NetworkPolicy"]
    _require(len(policies) == 1, "one time-series NetworkPolicy is required")
    policy = policies[0]["spec"]
    _require(
        policy.get("policyTypes") == ["Ingress", "Egress"],
        "network policy must control ingress and egress",
    )
    _require(
        "egress" not in policy,
        "time-series service must have no network egress by default",
    )
    ingress = policy.get("ingress", [])
    _require(
        bool(ingress) and ingress[0].get("from"),
        "trusted mTLS ingress selector is absent",
    )
    return len(deployments)


def check_container(root: Path) -> None:
    """Require a digest base, non-root runtime, and secret-free environment defaults."""
    path = root / "infra/timeseries_forecast/Dockerfile"
    payload = path.read_text(encoding="utf-8")
    first = payload.splitlines()[0]
    _require("@sha256:" in first, "time-series container base is not digest pinned")
    _require("apt-get" not in payload, "container resolves mutable OS packages")
    _require(
        "ARG SOURCE_DATE_EPOCH" in payload,
        "container does not declare its reproducible-build epoch",
    )
    _require("USER 65532:65532" in payload, "time-series container is not non-root")
    _require(
        "AEGIS_TLS_SECRET_ROOT" in payload, "mounted secret provider is not configured"
    )
    for token in SENSITIVE_LOG_TOKENS:
        _require(token not in payload, f"container embeds sensitive setting {token}")


def check_sbom(path: Path) -> int:
    """Validate CycloneDX content and required native dependencies."""
    value = json.loads(path.read_text(encoding="utf-8"))
    _require(value.get("bomFormat") == "CycloneDX", "SBOM format is not CycloneDX")
    _require(value.get("specVersion") == "1.5", "SBOM schema version is unexpected")
    components = value.get("components", [])
    names = {item.get("name") for item in components}
    for required in ("flatbuffers", "googletest", "benchmark"):
        _require(required in names, f"SBOM omits {required}")
    _require(
        all(item.get("purl") and item.get("version") for item in components),
        "SBOM component lacks package identity",
    )
    return len(components)


def main() -> int:
    """Run static security checks and optionally validate one generated SBOM."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--sbom", type=Path)
    arguments = parser.parse_args()
    root = arguments.root.resolve()
    for relative in REQUIRED_SECURITY_DOCS:
        _require(
            (root / relative).is_file(),
            f"required security document is absent: {relative}",
        )
    action_count = check_workflows(root)
    deployment_count = check_timeseries_manifest(root)
    check_container(root)
    sbom_count = 0 if arguments.sbom is None else check_sbom(arguments.sbom)
    print(
        "PASS: security policy validated "
        f"{action_count} action pins, {deployment_count} hardened deployments, "
        f"and {sbom_count} SBOM components"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
