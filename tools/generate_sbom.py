"""Generate a deterministic CycloneDX SBOM for foundation artifacts."""

# Integrity digests are intentionally kept whole for audit review.
# ruff: noqa: E501

from __future__ import annotations

import argparse
import importlib.metadata
import json
import re
import subprocess
import uuid
from pathlib import Path
from typing import Any

PROJECT_NAME = "aegis-mx-foundation"
PROJECT_VERSION = "0.1.0"
NAMESPACE = uuid.UUID("f10deffe-86a7-5bb5-8e3b-4a371aaea0de")
OPENSSL_VERSION_ERROR = "unable to resolve the OpenSSL runtime version"


def component(  # noqa: PLR0913 - mirrors the required CycloneDX identity fields.
    *,
    name: str,
    version: str,
    component_type: str,
    purl: str,
    license_id: str,
    sha256: str = "",
) -> dict[str, Any]:
    """Construct one normalized CycloneDX component."""
    result: dict[str, Any] = {
        "type": component_type,
        "name": name,
        "version": version,
        "purl": purl,
        "licenses": [{"license": {"id": license_id}}],
    }
    if sha256:
        result["hashes"] = [{"alg": "SHA-256", "content": sha256}]
    return result


def python_components() -> list[dict[str, Any]]:
    """Describe every distribution in the active isolated environment."""
    components: list[dict[str, Any]] = []
    for distribution in importlib.metadata.distributions():
        name = distribution.metadata.get("Name")
        if not name:
            continue
        normalized = name.lower().replace("_", "-")
        components.append(
            component(
                name=normalized,
                version=distribution.version,
                component_type="library",
                purl=f"pkg:pypi/{normalized}@{distribution.version}",
                license_id="NOASSERTION",
            )
        )
    return components


def go_components() -> list[dict[str, Any]]:
    """Describe modules selected by the control-plane Go module."""
    process = subprocess.run(
        ["go", "list", "-m", "-json", "all"],
        cwd=Path(__file__).resolve().parents[1] / "control",
        check=True,
        capture_output=True,
        text=True,
    )
    decoder = json.JSONDecoder()
    offset = 0
    components: list[dict[str, Any]] = []
    while offset < len(process.stdout):
        while offset < len(process.stdout) and process.stdout[offset].isspace():
            offset += 1
        if offset >= len(process.stdout):
            break
        module, offset = decoder.raw_decode(process.stdout, offset)
        if not isinstance(module, dict):
            continue
        module_path = str(module.get("Path", ""))
        module_version = str(module.get("Version", PROJECT_VERSION))
        if not module_path:
            continue
        components.append(
            component(
                name=module_path,
                version=module_version,
                component_type="library",
                purl=f"pkg:golang/{module_path}@{module_version}",
                license_id="NOASSERTION",
            )
        )
    return components


def openssl_component() -> dict[str, Any]:
    """Record the libcrypto implementation resolved by the release toolchain."""
    process = subprocess.run(
        ["openssl", "version"], check=True, capture_output=True, text=True
    )
    match = re.match(r"^OpenSSL ([0-9][0-9A-Za-z.+-]*)", process.stdout)
    if match is None:
        raise RuntimeError(OPENSSL_VERSION_ERROR)
    version = match.group(1)
    return component(
        name="openssl",
        version=version,
        component_type="library",
        purl=f"pkg:generic/openssl@{version}",
        license_id="Apache-2.0",
    )


def main() -> int:
    """Write a sorted deterministic CycloneDX document."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()

    dependencies = [
        component(
            name="flatbuffers",
            version="25.12.19",
            component_type="library",
            purl="pkg:github/google/flatbuffers@v25.12.19",
            license_id="Apache-2.0",
            sha256="f81c3162b1046fe8b84b9a0dbdd383e24fdbcf88583b9cb6028f90d04d90696a",  # pragma: allowlist secret
        ),
        component(
            name="googletest",
            version="1.17.0",
            component_type="library",
            purl="pkg:github/google/googletest@v1.17.0",
            license_id="BSD-3-Clause",
            sha256="65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c",  # pragma: allowlist secret
        ),
        component(
            name="benchmark",
            version="1.9.5",
            component_type="library",
            purl="pkg:github/google/benchmark@v1.9.5",
            license_id="Apache-2.0",
            sha256="9631341c82bac4a288bef951f8b26b41f69021794184ece969f8473977eaa340",  # pragma: allowlist secret
        ),
        openssl_component(),
        *python_components(),
        *go_components(),
    ]
    unique_dependencies = {
        (item["purl"], item["version"]): item for item in dependencies
    }
    sorted_dependencies = sorted(
        unique_dependencies.values(), key=lambda item: (item["purl"], item["version"])
    )
    serial = uuid.uuid5(NAMESPACE, f"{PROJECT_NAME}:{PROJECT_VERSION}")
    document: dict[str, Any] = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "serialNumber": f"urn:uuid:{serial}",
        "version": 1,
        "metadata": {
            "properties": [
                {"name": "aegis-mx:reproducible", "value": "true"},
                {"name": "aegis-mx:live-trading-capable", "value": "false"},
            ],
            "component": component(
                name=PROJECT_NAME,
                version=PROJECT_VERSION,
                component_type="application",
                purl=f"pkg:generic/{PROJECT_NAME}@{PROJECT_VERSION}",
                license_id="NOASSERTION",
            ),
        },
        "components": sorted_dependencies,
    }
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(f"PASS: wrote {len(sorted_dependencies)} components to {arguments.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
