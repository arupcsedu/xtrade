"""Validate, render, package, and health-check colocated edge deployments."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import os
import tarfile
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, Any, Final, cast

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence

REPOSITORY_ROOT: Final = Path(__file__).resolve().parents[1]
EDGE_ROOT: Final = REPOSITORY_ROOT / "infra/edge"
PROFILE_ROOT: Final = EDGE_ROOT / "profiles"
CONTRACT_PATH: Final = EDGE_ROOT / "deployment-contract-v1.json"
DEFAULT_SOURCE_DATE_EPOCH: Final = 1_600_000_000
MAX_JSON_BYTES: Final = 1_048_576
MAX_PACKAGE_BYTES: Final = 128 * 1024 * 1024
MAX_PACKAGE_MEMBERS: Final = 4_096
MAX_HEALTH_BYTES: Final = 65_536
MAX_TEXT_BYTES: Final = 512
MAX_CPU_ID: Final = 4_095
SHA256_LENGTH: Final = 64
SAFE_PATH_CHARACTERS: Final = frozenset(
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/._-"
)
PROFILE_NAMES: Final = (
    "development",
    "ci",
    "replay",
    "paper",
    "staging",
    "production-disabled",
)
PROFILE_MODES: Final = {
    "development": "SIMULATION",
    "ci": "SIMULATION",
    "replay": "SIMULATION",
    "paper": "PAPER",
    "staging": "PAPER",
    "production-disabled": "SIMULATION",
}
ROLE_UNITS: Final = {
    "ptp": "aegis-ptp-ready.service",
    "journal": "aegis-edge-journal.service",
    "edge-core": "aegis-edge-core.service",
    "paper-gateway": "aegis-paper-gateway.service",
    "observability": "aegis-edge-observability.service",
}
REPOSITORY_EXECUTABLE_CONTRACT: Final = {
    "aegis-edge-clock-guard": (
        Path("cpp/edge_services/CMakeLists.txt"),
        "aegis-edge-clock-guard",
    ),
    "aegis-edge-journal": (
        Path("cpp/edge_services/CMakeLists.txt"),
        "aegis-edge-journal",
    ),
    "aegis-edge-core": (
        Path("cpp/edge_services/CMakeLists.txt"),
        "aegis-edge-core",
    ),
    "aegis-paper-gateway": (
        Path("cpp/edge_services/CMakeLists.txt"),
        "aegis-paper-gateway",
    ),
    "aegis-edge-observability": (
        Path("cpp/edge_services/CMakeLists.txt"),
        "aegis-edge-observability",
    ),
    "aegis-edge-config-verify": (
        Path("control/cmd/edge-config-verify/main.go"),
        "func main()",
    ),
}


@dataclass(frozen=True, slots=True)
class DeploymentPaths:
    """Absolute, non-secret filesystem layout."""

    release_root: str
    current_symlink: str
    configuration_path: str
    state_root: str
    journal_root: str
    log_root: str
    runtime_root: str


@dataclass(frozen=True, slots=True)
class ResourcePolicy:
    """CPU, NUMA, huge-page, memory-lock, and descriptor policy."""

    numa_node: int
    dedicated_cpu_sets: bool
    hugepage_size_kb: int
    hugepage_count: int
    memory_lock_bytes: int
    file_descriptor_limit: int
    cpu_affinity: Mapping[str, str]


@dataclass(frozen=True, slots=True)
class NicPolicy:
    """Provider-neutral NIC and receive-queue policy."""

    interface: str | None
    hardware_timestamping_required: bool
    rx_queue_count_required: int
    queue_assignments: Mapping[str, tuple[int, ...]]


@dataclass(frozen=True, slots=True)
class PtpPolicy:
    """PTP service and clock-readiness policy."""

    required: bool
    ptp4l_unit: str
    phc2sys_unit: str
    maximum_clock_quality_age_ns: int
    maximum_absolute_offset_ns: int


@dataclass(frozen=True, slots=True)
class HealthPolicy:
    """Bounded service health publication policy."""

    status_root: str
    maximum_status_age_ns: int


@dataclass(frozen=True, slots=True)
class JournalPolicy:
    """Local journal storage and durability policy."""

    filesystem: str
    dedicated_device_required: bool
    direct_io_preferred: bool
    segment_bytes: int
    sync_policy: str
    retention_days: int


@dataclass(frozen=True, slots=True)
class EdgeProfile:
    """One immutable deployment environment."""

    schema_version: int
    name: str
    trading_mode: str
    live_transmission_enabled: bool
    automatic_activation: bool
    signed_runtime_configuration_required: bool
    paths: DeploymentPaths
    resources: ResourcePolicy
    nic: NicPolicy
    ptp: PtpPolicy
    health: HealthPolicy
    journal: JournalPolicy
    sha256: str
    source_bytes: bytes


@dataclass(frozen=True, slots=True)
class HostFacts:
    """Operator-captured hardware facts used by offline validation."""

    online_cpus: frozenset[int]
    online_numa_nodes: frozenset[int]
    cpu_numa_nodes: Mapping[int, int]
    hugepage_size_kb: int
    hugepage_total: int
    hugepages_by_numa_node: Mapping[int, int]
    interfaces: frozenset[str]
    interface_numa_nodes: Mapping[str, int]
    hardware_timestamp_interfaces: frozenset[str]
    receive_queue_counts: Mapping[str, int]
    sha256: str


def _canonical_bytes(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")


def _sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _sha256(value: object) -> str:
    return _sha256_bytes(_canonical_bytes(value))


def _unique_json_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise ValueError("JSON object contains a duplicate key")
        value[key] = item
    return value


def _decode_json(raw: bytes) -> object:
    return json.loads(raw, object_pairs_hook=_unique_json_object)


def _load_json(path: Path, maximum_bytes: int = MAX_JSON_BYTES) -> Mapping[str, Any]:
    raw = path.read_bytes()
    if not raw or len(raw) > maximum_bytes:
        raise ValueError("JSON document size is invalid")
    value = _decode_json(raw)
    if not isinstance(value, dict):
        raise ValueError("JSON document must be an object")
    return value


def _expect_keys(value: Mapping[str, Any], expected: set[str], field: str) -> None:
    if set(value) != expected:
        raise ValueError(f"{field} has missing or unsupported fields")


def _mapping(value: object, field: str) -> Mapping[str, Any]:
    if not isinstance(value, dict):
        raise ValueError(f"{field} must be an object")
    return value


def _text(value: object, field: str) -> str:
    if (
        not isinstance(value, str)
        or not value
        or len(value) > MAX_TEXT_BYTES
        or any(ord(character) < 32 or ord(character) > 126 for character in value)
    ):
        raise ValueError(f"{field} must be a bounded non-empty string")
    return value


def _boolean(value: object, field: str) -> bool:
    if not isinstance(value, bool):
        raise ValueError(f"{field} must be boolean")
    return value


def _integer(
    value: object,
    field: str,
    *,
    minimum: int = 0,
    maximum: int = (1 << 63) - 1,
) -> int:
    if (
        not isinstance(value, int)
        or isinstance(value, bool)
        or value < minimum
        or value > maximum
    ):
        raise ValueError(f"{field} is outside its integer bounds")
    return value


def _optional_text(value: object, field: str) -> str | None:
    return None if value is None else _text(value, field)


def parse_cpu_set(value: str) -> frozenset[int]:
    """Parse a Linux CPU-list string with strict bounds and no ambiguity."""
    if not value or any(character.isspace() for character in value):
        raise ValueError("CPU set is empty or contains whitespace")
    cpus: set[int] = set()
    for token in value.split(","):
        parts = token.split("-")
        if len(parts) == 1:
            start = end = _integer_text(parts[0])
        elif len(parts) == 2:
            start = _integer_text(parts[0])
            end = _integer_text(parts[1])
        else:
            raise ValueError("CPU set range is malformed")
        if start > end:
            raise ValueError("CPU set range is descending")
        cpus.update(range(start, end + 1))
        if len(cpus) > MAX_CPU_ID + 1:
            raise ValueError("CPU set is too large")
    return frozenset(cpus)


def _integer_text(value: str) -> int:
    if not value.isascii() or not value.isdecimal():
        raise ValueError("CPU identifier is malformed")
    number = int(value)
    if number > MAX_CPU_ID:
        raise ValueError("CPU identifier exceeds the supported bound")
    return number


def _absolute_path(value: object, field: str, prefixes: tuple[str, ...]) -> str:
    text = _text(value, field)
    path = PurePosixPath(text)
    if (
        not path.is_absolute()
        or ".." in path.parts
        or not text.startswith(prefixes)
        or any(character not in SAFE_PATH_CHARACTERS for character in text)
    ):
        raise ValueError(f"{field} is not an approved absolute path")
    return text


def _parse_profile(
    value: Mapping[str, Any], digest: str, source_bytes: bytes
) -> EdgeProfile:
    _expect_keys(
        value,
        {
            "profile_schema_version",
            "name",
            "trading_mode",
            "live_transmission_enabled",
            "automatic_activation",
            "signed_runtime_configuration_required",
            "paths",
            "resources",
            "nic",
            "ptp",
            "health",
            "journal",
        },
        "profile",
    )
    paths_value = _mapping(value["paths"], "paths")
    _expect_keys(
        paths_value,
        {
            "release_root",
            "current_symlink",
            "configuration_path",
            "state_root",
            "journal_root",
            "log_root",
            "runtime_root",
        },
        "paths",
    )
    paths = DeploymentPaths(
        release_root=_absolute_path(
            paths_value["release_root"], "release_root", ("/opt/",)
        ),
        current_symlink=_absolute_path(
            paths_value["current_symlink"], "current_symlink", ("/opt/",)
        ),
        configuration_path=_absolute_path(
            paths_value["configuration_path"], "configuration_path", ("/etc/",)
        ),
        state_root=_absolute_path(paths_value["state_root"], "state_root", ("/var/",)),
        journal_root=_absolute_path(
            paths_value["journal_root"], "journal_root", ("/var/",)
        ),
        log_root=_absolute_path(paths_value["log_root"], "log_root", ("/var/",)),
        runtime_root=_absolute_path(
            paths_value["runtime_root"], "runtime_root", ("/run/",)
        ),
    )

    resources_value = _mapping(value["resources"], "resources")
    _expect_keys(
        resources_value,
        {
            "numa_node",
            "dedicated_cpu_sets",
            "hugepage_size_kb",
            "hugepage_count",
            "memory_lock_bytes",
            "file_descriptor_limit",
            "cpu_affinity",
        },
        "resources",
    )
    affinity_value = _mapping(resources_value["cpu_affinity"], "cpu_affinity")
    if set(affinity_value) != set(ROLE_UNITS):
        raise ValueError("CPU affinity must define every service role")
    affinity = {
        role: _text(cpu_set, f"cpu_affinity.{role}")
        for role, cpu_set in affinity_value.items()
    }
    parsed_sets = {role: parse_cpu_set(cpu_set) for role, cpu_set in affinity.items()}
    dedicated = _boolean(resources_value["dedicated_cpu_sets"], "dedicated_cpu_sets")
    if dedicated:
        allocated: set[int] = set()
        for role in ROLE_UNITS:
            if allocated & parsed_sets[role]:
                raise ValueError("dedicated CPU sets overlap")
            allocated.update(parsed_sets[role])
    resources = ResourcePolicy(
        numa_node=_integer(resources_value["numa_node"], "numa_node", maximum=1_024),
        dedicated_cpu_sets=dedicated,
        hugepage_size_kb=_integer(
            resources_value["hugepage_size_kb"],
            "hugepage_size_kb",
            minimum=2_048,
            maximum=1_048_576,
        ),
        hugepage_count=_integer(
            resources_value["hugepage_count"], "hugepage_count", maximum=1_048_576
        ),
        memory_lock_bytes=_integer(
            resources_value["memory_lock_bytes"],
            "memory_lock_bytes",
            maximum=(1 << 63) - 1,
        ),
        file_descriptor_limit=_integer(
            resources_value["file_descriptor_limit"],
            "file_descriptor_limit",
            minimum=1_024,
            maximum=16_777_216,
        ),
        cpu_affinity=affinity,
    )
    hugepage_bytes = resources.hugepage_size_kb * 1_024 * resources.hugepage_count
    if hugepage_bytes > resources.memory_lock_bytes:
        raise ValueError("memory-lock limit does not cover configured huge pages")

    nic_value = _mapping(value["nic"], "nic")
    _expect_keys(
        nic_value,
        {
            "interface",
            "hardware_timestamping_required",
            "rx_queue_count_required",
            "queue_assignments",
        },
        "nic",
    )
    queue_values = _mapping(nic_value["queue_assignments"], "queue_assignments")
    queue_assignments: dict[str, tuple[int, ...]] = {}
    assigned_queues: set[int] = set()
    for channel, raw_queues in queue_values.items():
        channel_name = _text(channel, "queue channel")
        if not isinstance(raw_queues, list) or len(raw_queues) > 256:
            raise ValueError("queue assignment must be a bounded array")
        queues = tuple(
            _integer(item, "receive queue", maximum=65_535) for item in raw_queues
        )
        if len(set(queues)) != len(queues) or assigned_queues.intersection(queues):
            raise ValueError("receive queue is assigned more than once")
        assigned_queues.update(queues)
        queue_assignments[channel_name] = queues
    interface = _optional_text(nic_value["interface"], "nic.interface")
    hardware_timestamping_required = _boolean(
        nic_value["hardware_timestamping_required"],
        "hardware_timestamping_required",
    )
    queue_count = _integer(
        nic_value["rx_queue_count_required"],
        "rx_queue_count_required",
        maximum=65_536,
    )
    if interface is None and (hardware_timestamping_required or queue_assignments):
        raise ValueError("NIC requirements need an interface")
    if assigned_queues and max(assigned_queues) >= queue_count:
        raise ValueError("receive queue assignment exceeds the required queue count")
    nic = NicPolicy(
        interface=interface,
        hardware_timestamping_required=hardware_timestamping_required,
        rx_queue_count_required=queue_count,
        queue_assignments=queue_assignments,
    )

    ptp_value = _mapping(value["ptp"], "ptp")
    _expect_keys(
        ptp_value,
        {
            "required",
            "ptp4l_unit",
            "phc2sys_unit",
            "maximum_clock_quality_age_ns",
            "maximum_absolute_offset_ns",
        },
        "ptp",
    )
    ptp = PtpPolicy(
        required=_boolean(ptp_value["required"], "ptp.required"),
        ptp4l_unit=_text(ptp_value["ptp4l_unit"], "ptp4l_unit"),
        phc2sys_unit=_text(ptp_value["phc2sys_unit"], "phc2sys_unit"),
        maximum_clock_quality_age_ns=_integer(
            ptp_value["maximum_clock_quality_age_ns"],
            "maximum_clock_quality_age_ns",
            minimum=1,
        ),
        maximum_absolute_offset_ns=_integer(
            ptp_value["maximum_absolute_offset_ns"],
            "maximum_absolute_offset_ns",
            minimum=1,
        ),
    )

    health_value = _mapping(value["health"], "health")
    _expect_keys(health_value, {"status_root", "maximum_status_age_ns"}, "health")
    health = HealthPolicy(
        status_root=_absolute_path(
            health_value["status_root"], "health.status_root", ("/run/",)
        ),
        maximum_status_age_ns=_integer(
            health_value["maximum_status_age_ns"],
            "maximum_status_age_ns",
            minimum=1,
        ),
    )

    journal_value = _mapping(value["journal"], "journal")
    _expect_keys(
        journal_value,
        {
            "filesystem",
            "dedicated_device_required",
            "direct_io_preferred",
            "segment_bytes",
            "sync_policy",
            "retention_days",
        },
        "journal",
    )
    filesystem = _text(journal_value["filesystem"], "journal.filesystem")
    sync_policy = _text(journal_value["sync_policy"], "journal.sync_policy")
    if filesystem not in {"tmpfs", "xfs", "ext4"}:
        raise ValueError("journal filesystem is unsupported")
    if sync_policy not in {"EVERY_RECORD", "PERIODIC", "ON_ROTATION"}:
        raise ValueError("journal sync policy is unsupported")
    journal = JournalPolicy(
        filesystem=filesystem,
        dedicated_device_required=_boolean(
            journal_value["dedicated_device_required"],
            "dedicated_device_required",
        ),
        direct_io_preferred=_boolean(
            journal_value["direct_io_preferred"], "direct_io_preferred"
        ),
        segment_bytes=_integer(
            journal_value["segment_bytes"],
            "journal.segment_bytes",
            minimum=1_048_576,
            maximum=1 << 40,
        ),
        sync_policy=sync_policy,
        retention_days=_integer(
            journal_value["retention_days"],
            "journal.retention_days",
            minimum=1,
            maximum=3_650,
        ),
    )

    name = _text(value["name"], "name")
    mode = _text(value["trading_mode"], "trading_mode")
    live_enabled = _boolean(
        value["live_transmission_enabled"], "live_transmission_enabled"
    )
    automatic_activation = _boolean(
        value["automatic_activation"], "automatic_activation"
    )
    signed_required = _boolean(
        value["signed_runtime_configuration_required"],
        "signed_runtime_configuration_required",
    )
    schema_version = _integer(
        value["profile_schema_version"],
        "profile_schema_version",
        minimum=1,
        maximum=1,
    )
    if schema_version != 1 or name not in PROFILE_NAMES:
        raise ValueError("profile identity or schema version is unsupported")
    if mode != PROFILE_MODES.get(name):
        raise ValueError("deployment profile has an unsafe or mismatched mode")
    if live_enabled or automatic_activation:
        raise ValueError("live transmission and automatic activation are forbidden")
    if signed_required and not paths.configuration_path.endswith(".signed.json"):
        raise ValueError("signed profile requires a signed configuration path")
    if not paths.journal_root.startswith(f"{paths.state_root}/"):
        raise ValueError("journal root must be contained by the state root")
    if not health.status_root.startswith(f"{paths.runtime_root}/"):
        raise ValueError("health root must be contained by the runtime root")
    if name in {"staging", "production-disabled"} and (
        not resources.dedicated_cpu_sets
        or not ptp.required
        or not signed_required
        or nic.interface is None
        or not journal.dedicated_device_required
        or not journal.direct_io_preferred
    ):
        raise ValueError("staging-class profile omits a fail-closed prerequisite")
    if name == "paper" and (
        not resources.dedicated_cpu_sets
        or not ptp.required
        or not signed_required
        or not journal.dedicated_device_required
    ):
        raise ValueError("paper profile omits a fail-closed prerequisite")
    return EdgeProfile(
        schema_version=schema_version,
        name=name,
        trading_mode=mode,
        live_transmission_enabled=live_enabled,
        automatic_activation=automatic_activation,
        signed_runtime_configuration_required=signed_required,
        paths=paths,
        resources=resources,
        nic=nic,
        ptp=ptp,
        health=health,
        journal=journal,
        sha256=digest,
        source_bytes=source_bytes,
    )


def load_profile(path: Path) -> EdgeProfile:
    """Load and validate one immutable environment profile."""
    raw = path.read_bytes()
    if not raw or len(raw) > MAX_JSON_BYTES:
        raise ValueError("profile size is invalid")
    value = _decode_json(raw)
    if not isinstance(value, dict):
        raise ValueError("profile must be a JSON object")
    return _parse_profile(value, _sha256_bytes(raw), raw)


def load_named_profile(name: str) -> EdgeProfile:
    """Load one of the six checked-in profiles by exact name."""
    if name not in PROFILE_NAMES:
        raise ValueError("unknown edge deployment profile")
    profile = load_profile(PROFILE_ROOT / f"{name}.json")
    if profile.name != name:
        raise ValueError("profile file identity does not match its name")
    if name in {"staging", "production-disabled"} and (
        profile.nic.interface != "OPERATOR_REQUIRED"
    ):
        raise ValueError("checked-in staging profile must retain its NIC placeholder")
    return profile


def profile_as_environment(profile: EdgeProfile) -> str:
    """Render a secret-free environment file for systemd."""
    values = {
        "AEGIS_CONFIGURATION_PATH": profile.paths.configuration_path,
        "AEGIS_DEPLOYMENT_PROFILE": profile.name,
        "AEGIS_DEPLOYMENT_MANIFEST_PATH": (
            "/etc/aegis-mx/deployment/deployment-manifest.json"
        ),
        "AEGIS_DEPLOYMENT_PROFILE_PATH": (
            "/etc/aegis-mx/deployment/active-profile.json"
        ),
        "AEGIS_HEALTH_ROOT": profile.health.status_root,
        "AEGIS_JOURNAL_ROOT": profile.paths.journal_root,
        "AEGIS_LIVE_TRANSMISSION": "DISABLED",
        "AEGIS_LOG_ROOT": profile.paths.log_root,
        "AEGIS_PROFILE_SHA256": profile.sha256,
        "AEGIS_RUNTIME_ROOT": profile.paths.runtime_root,
        "AEGIS_STATE_ROOT": profile.paths.state_root,
        "AEGIS_TRADING_MODE": profile.trading_mode,
    }
    return "".join(f"{key}={values[key]}\n" for key in sorted(values))


def _write_file(path: Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)


def render_profile(profile: EdgeProfile, output: Path) -> Mapping[str, object]:
    """Render deterministic systemd drop-ins and host assignment plans."""
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise ValueError("render output directory must be absent or empty")
    output.mkdir(parents=True, exist_ok=True)
    rendered: list[Path] = []
    environment_path = output / "environment/aegis-edge.conf"
    _write_file(environment_path, profile_as_environment(profile).encode("utf-8"))
    rendered.append(environment_path)
    active_profile_path = output / "deployment/active-profile.json"
    _write_file(active_profile_path, profile.source_bytes)
    rendered.append(active_profile_path)
    for role, unit in ROLE_UNITS.items():
        memory_lock = profile.resources.memory_lock_bytes
        drop_in = (
            "[Service]\n"
            f"CPUAffinity={profile.resources.cpu_affinity[role]}\n"
            "NUMAPolicy=bind\n"
            f"NUMAMask={profile.resources.numa_node}\n"
            f"LimitMEMLOCK={memory_lock}\n"
            f"LimitNOFILE={profile.resources.file_descriptor_limit}\n"
            "EnvironmentFile=/etc/aegis-mx/environment/aegis-edge.conf\n"
        )
        path = output / f"systemd/{unit}.d/20-aegis-profile.conf"
        _write_file(path, drop_in.encode("utf-8"))
        rendered.append(path)
    nic_plan = {
        "hardware_timestamping_required": profile.nic.hardware_timestamping_required,
        "interface": profile.nic.interface,
        "profile_sha256": profile.sha256,
        "queue_assignments": {
            key: list(value)
            for key, value in sorted(profile.nic.queue_assignments.items())
        },
        "rx_queue_count_required": profile.nic.rx_queue_count_required,
    }
    nic_path = output / "host/nic-queue-plan.json"
    _write_file(
        nic_path, json.dumps(nic_plan, indent=2, sort_keys=True).encode() + b"\n"
    )
    rendered.append(nic_path)
    file_hashes = {
        path.relative_to(output).as_posix(): _sha256_bytes(path.read_bytes())
        for path in sorted(rendered)
    }
    manifest: dict[str, object] = {
        "automatic_activation": False,
        "files": file_hashes,
        "live_transmission_enabled": False,
        "profile": profile.name,
        "profile_sha256": profile.sha256,
        "schema_version": 1,
        "trading_mode": profile.trading_mode,
    }
    manifest["manifest_sha256"] = _sha256(manifest)
    manifest_path = output / "deployment/deployment-manifest.json"
    _write_file(
        manifest_path,
        json.dumps(manifest, indent=2, sort_keys=True).encode("utf-8") + b"\n",
    )
    return manifest


def _asset_paths() -> tuple[Path, ...]:
    roots = (
        EDGE_ROOT / "systemd",
        EDGE_ROOT / "host",
        EDGE_ROOT / "logrotate",
        EDGE_ROOT / "sysusers.d",
        EDGE_ROOT / "tmpfiles.d",
    )
    return tuple(
        sorted(path for root in roots for path in root.rglob("*") if path.is_file())
    )


def validate_assets() -> Mapping[str, object]:
    """Validate every profile, unit contract, and host configuration asset."""
    expected_profile_paths = {PROFILE_ROOT / f"{name}.json" for name in PROFILE_NAMES}
    actual_profile_paths = {path for path in PROFILE_ROOT.rglob("*") if path.is_file()}
    if actual_profile_paths != expected_profile_paths:
        raise ValueError("profile directory contains an unsupported environment")
    profiles = [load_named_profile(name) for name in PROFILE_NAMES]
    contract = _load_json(CONTRACT_PATH)
    _expect_keys(contract, {"schema_version", "services"}, "deployment contract")
    contract_version = _integer(
        contract["schema_version"],
        "deployment contract schema_version",
        minimum=1,
        maximum=1,
    )
    if contract_version != 1 or not isinstance(contract["services"], list):
        raise ValueError("deployment contract version or services are invalid")
    service_rows = cast("list[object]", contract["services"])
    seen_units: set[str] = set()
    seen_startup: set[int] = set()
    seen_shutdown: set[int] = set()
    required_executables = {"aegis-edge-config-verify"}
    for raw_row in service_rows:
        row = _mapping(raw_row, "service contract")
        _expect_keys(
            row,
            {
                "role",
                "unit",
                "binary",
                "startup_order",
                "shutdown_order",
                "required_directives",
            },
            "service contract",
        )
        role = _text(row["role"], "service role")
        unit = _text(row["unit"], "service unit")
        binary = _text(row["binary"], "service binary")
        required_executables.add(PurePosixPath(binary).name)
        startup = _integer(row["startup_order"], "startup_order", minimum=1)
        shutdown = _integer(row["shutdown_order"], "shutdown_order", minimum=1)
        directives = row["required_directives"]
        if (
            role not in ROLE_UNITS
            or ROLE_UNITS[role] != unit
            or unit in seen_units
            or startup in seen_startup
            or shutdown in seen_shutdown
            or not binary.startswith("/opt/aegis-mx/current/bin/")
            or not isinstance(directives, list)
            or not directives
        ):
            raise ValueError("service ordering or identity is invalid")
        unit_path = EDGE_ROOT / "systemd" / unit
        unit_text = unit_path.read_text(encoding="utf-8")
        mandatory_directives = (
            "User=aegis-edge",
            "Group=aegis-edge",
            "EnvironmentFile=/etc/aegis-mx/environment/aegis-edge.conf",
            "validate-host --profile-path",
            "aegis-edge-config-verify",
            "LimitCORE=0",
            "CapabilityBoundingSet=",
            "NoNewPrivileges=yes",
            "ProtectSystem=strict",
        )
        if binary not in unit_text or any(
            directive not in unit_text for directive in mandatory_directives
        ):
            raise ValueError(f"{unit} omits its binary or security preflight")
        for directive in directives:
            if _text(directive, "required directive") not in unit_text:
                raise ValueError(f"{unit} omits a required directive")
        if any(
            forbidden in unit_text
            for forbidden in (
                "LIVE_ACTIVE",
                "AEGIS_ENABLE_LIVE_TRADING",
                "http://",
                "https://",
            )
        ):
            raise ValueError("systemd unit contains a forbidden live/network value")
        seen_units.add(unit)
        seen_startup.add(startup)
        seen_shutdown.add(shutdown)
    if seen_units != set(ROLE_UNITS.values()):
        raise ValueError("deployment contract omits a service role")
    auxiliary_requirements = {
        "aegis-edge-inhibit-initialize.service": (
            "Before=aegis-ptp-ready.service",
            "startup-default",
            "RemainAfterExit=yes",
        ),
        "aegis-edge-health.service": ("OnFailure=aegis-edge-inhibit@%n",),
        "aegis-edge.target": ("Requires=aegis-edge-inhibit-initialize.service",),
    }
    for filename, requirements in auxiliary_requirements.items():
        content = (EDGE_ROOT / "systemd" / filename).read_text(encoding="utf-8")
        if any(requirement not in content for requirement in requirements):
            raise ValueError(f"{filename} omits a fail-closed lifecycle directive")
    for systemd_path in (EDGE_ROOT / "systemd").glob("*"):
        if systemd_path.is_file() and any(
            forbidden in systemd_path.read_text(encoding="utf-8")
            for forbidden in (
                "LIVE_ACTIVE",
                "AEGIS_ENABLE_LIVE_TRADING",
                "http://",
                "https://",
            )
        ):
            raise ValueError("systemd asset contains a forbidden live/network value")
    assets = _asset_paths()
    required_assets = {
        EDGE_ROOT / "host/sysctl.d/90-aegis-edge.conf",
        EDGE_ROOT / "logrotate/aegis-mx",
        EDGE_ROOT / "sysusers.d/aegis-mx.conf",
        EDGE_ROOT / "tmpfiles.d/aegis-mx.conf",
        EDGE_ROOT / "host/kernel-command-line.example",
    }
    if not required_assets <= set(assets):
        raise ValueError("host deployment assets are incomplete")
    hashes = {
        path.relative_to(REPOSITORY_ROOT).as_posix(): _sha256_bytes(path.read_bytes())
        for path in assets
    }
    missing_repository_executables = sorted(
        name
        for name in required_executables
        if name not in REPOSITORY_EXECUTABLE_CONTRACT
        or not (REPOSITORY_ROOT / REPOSITORY_EXECUTABLE_CONTRACT[name][0]).is_file()
        or REPOSITORY_EXECUTABLE_CONTRACT[name][1]
        not in (REPOSITORY_ROOT / REPOSITORY_EXECUTABLE_CONTRACT[name][0]).read_text(
            encoding="utf-8"
        )
    )
    return {
        "asset_count": len(assets),
        "asset_sha256": _sha256(hashes),
        "automatic_activation": False,
        "live_transmission_enabled": False,
        "missing_repository_executables": missing_repository_executables,
        "profile_count": len(profiles),
        "profiles_sha256": _sha256([profile.sha256 for profile in profiles]),
        "schema_version": 1,
        "service_count": len(seen_units),
    }


def _collect_rollback_entries(
    profile: EdgeProfile,
    rendered_root: Path,
    host_facts_bytes: bytes | None,
) -> dict[str, bytes]:
    entries: dict[str, bytes] = {}
    for path in _asset_paths():
        relative = path.relative_to(EDGE_ROOT).as_posix()
        entries[f"edge/{relative}"] = path.read_bytes()
    entries["profiles/active.json"] = profile.source_bytes
    entries["edge/deployment-contract-v1.json"] = CONTRACT_PATH.read_bytes()
    for schema_name in (
        "edge-deployment-profile-v1.schema.json",
        "edge-health-v1.schema.json",
        "edge-host-facts-v1.schema.json",
        "edge-rollback-manifest-v1.schema.json",
    ):
        entries[f"schemas/{schema_name}"] = (
            REPOSITORY_ROOT / "schemas" / schema_name
        ).read_bytes()
    entries["tools/edge_deployment.py"] = (
        REPOSITORY_ROOT / "tools/edge_deployment.py"
    ).read_bytes()
    if host_facts_bytes is not None:
        entries["host/validated-facts.json"] = host_facts_bytes
    for path in sorted(rendered_root.rglob("*")):
        if path.is_file():
            entries[f"rendered/{path.relative_to(rendered_root).as_posix()}"] = (
                path.read_bytes()
            )
    instructions = REPOSITORY_ROOT / "docs/operations/colocated-edge-rollback.md"
    entries["ROLLBACK-INSTRUCTIONS.md"] = instructions.read_bytes()
    return entries


def _tar_info(name: str, size: int, source_date_epoch: int) -> tarfile.TarInfo:
    info = tarfile.TarInfo(name)
    info.size = size
    info.mode = 0o640
    info.uid = 0
    info.gid = 0
    info.uname = "root"
    info.gname = "root"
    info.mtime = source_date_epoch
    return info


def _encode_archive(entries: Mapping[str, bytes], source_date_epoch: int) -> bytes:
    output = io.BytesIO()
    with (
        gzip.GzipFile(
            filename="",
            mode="wb",
            compresslevel=9,
            fileobj=output,
            mtime=source_date_epoch,
        ) as compressed,
        tarfile.open(
            fileobj=compressed,
            mode="w",
            format=tarfile.USTAR_FORMAT,
        ) as archive,
    ):
        for name, payload in sorted(entries.items()):
            archive.addfile(
                _tar_info(name, len(payload), source_date_epoch),
                io.BytesIO(payload),
            )
    return output.getvalue()


def build_rollback_package(
    profile: EdgeProfile,
    output: Path,
    source_date_epoch: int,
    host_facts_path: Path | None = None,
) -> Mapping[str, object]:
    """Build a deterministic, non-activating rollback package."""
    if source_date_epoch <= 0:
        raise ValueError("SOURCE_DATE_EPOCH must be positive")
    host_facts_bytes: bytes | None = None
    if host_facts_path is not None:
        host_facts_bytes = host_facts_path.read_bytes()
        if not host_facts_bytes or len(host_facts_bytes) > MAX_JSON_BYTES:
            raise ValueError("host facts size is invalid")
        validate_host(
            profile,
            _parse_host_facts(
                _mapping(_decode_json(host_facts_bytes), "host facts"),
                _sha256_bytes(host_facts_bytes),
            ),
        )
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="aegis-edge-render.") as temp:
        render_root = Path(temp)
        render_profile(profile, render_root)
        entries = _collect_rollback_entries(
            profile,
            render_root,
            host_facts_bytes,
        )
    file_hashes = {
        name: _sha256_bytes(payload) for name, payload in sorted(entries.items())
    }
    manifest: dict[str, object] = {
        "automatic_activation": False,
        "files": file_hashes,
        "live_transmission_enabled": False,
        "profile": profile.name,
        "profile_sha256": profile.sha256,
        "schema_version": 1,
        "source_date_epoch": source_date_epoch,
        "trading_mode": profile.trading_mode,
    }
    manifest["manifest_sha256"] = _sha256(manifest)
    entries["rollback-manifest.json"] = (
        json.dumps(manifest, indent=2, sort_keys=True).encode() + b"\n"
    )
    package_bytes = _encode_archive(entries, source_date_epoch)
    if len(package_bytes) > MAX_PACKAGE_BYTES:
        raise ValueError("rollback package exceeds its size bound")
    temporary = output.with_name(f".{output.name}.{os.getpid()}.tmp")
    try:
        temporary.write_bytes(package_bytes)
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
    result: dict[str, object] = {
        "file_count": len(entries),
        "package": output.as_posix(),
        "package_sha256": _sha256_bytes(output.read_bytes()),
        "profile": profile.name,
    }
    if host_facts_bytes is not None:
        result["host_facts_sha256"] = _sha256_bytes(host_facts_bytes)
    return result


def verify_rollback_package(path: Path) -> Mapping[str, object]:
    """Verify package framing, safe paths, manifest, and every payload hash."""
    package_bytes = path.read_bytes()
    size = len(package_bytes)
    if size <= 0 or size > MAX_PACKAGE_BYTES:
        raise ValueError("rollback package size is invalid")
    with gzip.GzipFile(fileobj=io.BytesIO(package_bytes), mode="rb") as compressed:
        tar_bytes = compressed.read(MAX_PACKAGE_BYTES + 1)
    if len(tar_bytes) > MAX_PACKAGE_BYTES:
        raise ValueError("rollback package expands beyond its size bound")
    entries: dict[str, bytes] = {}
    total_size = 0
    with tarfile.open(fileobj=io.BytesIO(tar_bytes), mode="r:") as archive:
        for index, member in enumerate(archive):
            if index >= MAX_PACKAGE_MEMBERS:
                raise ValueError("rollback archive has too many members")
            pure = PurePosixPath(member.name)
            total_size += member.size
            if (
                not member.isfile()
                or pure.is_absolute()
                or ".." in pure.parts
                or member.name in entries
                or member.size > MAX_PACKAGE_BYTES
                or total_size > MAX_PACKAGE_BYTES
            ):
                raise ValueError("rollback archive member is unsafe")
            stream = archive.extractfile(member)
            if stream is None:
                raise ValueError("rollback archive member cannot be read")
            entries[member.name] = stream.read()
    raw_manifest = entries.pop("rollback-manifest.json", None)
    if raw_manifest is None:
        raise ValueError("rollback manifest is missing")
    manifest = _decode_json(raw_manifest)
    if not isinstance(manifest, dict):
        raise ValueError("rollback manifest must be an object")
    _expect_keys(
        manifest,
        {
            "automatic_activation",
            "files",
            "live_transmission_enabled",
            "manifest_sha256",
            "profile",
            "profile_sha256",
            "schema_version",
            "source_date_epoch",
            "trading_mode",
        },
        "rollback manifest",
    )
    claimed_manifest_hash = manifest.pop("manifest_sha256", None)
    if (
        not isinstance(claimed_manifest_hash, str)
        or len(claimed_manifest_hash) != SHA256_LENGTH
        or claimed_manifest_hash != _sha256(manifest)
    ):
        raise ValueError("rollback manifest hash is invalid")
    expected_files = manifest.get("files")
    if not isinstance(expected_files, dict) or set(expected_files) != set(entries):
        raise ValueError("rollback manifest file set is invalid")
    for name, payload in entries.items():
        expected_digest = expected_files[name]
        if (
            not isinstance(expected_digest, str)
            or len(expected_digest) != SHA256_LENGTH
            or expected_digest != _sha256_bytes(payload)
        ):
            raise ValueError("rollback payload hash is invalid")
    profile_name = manifest.get("profile")
    profile_digest = manifest.get("profile_sha256")
    manifest_schema_version = _integer(
        manifest.get("schema_version"),
        "rollback schema_version",
        minimum=1,
        maximum=1,
    )
    source_date_epoch = _integer(
        manifest.get("source_date_epoch"),
        "rollback source_date_epoch",
        minimum=1,
    )
    active_profile_bytes = entries.get("profiles/active.json", b"")
    active_profile = _decode_json(active_profile_bytes or b"{}")
    if not isinstance(active_profile, dict):
        raise ValueError("embedded rollback profile must be an object")
    validated_profile = _parse_profile(
        active_profile,
        _sha256_bytes(active_profile_bytes),
        active_profile_bytes,
    )
    embedded_host_facts = entries.get("host/validated-facts.json")
    if embedded_host_facts is not None:
        validate_host(
            validated_profile,
            _parse_host_facts(
                _mapping(_decode_json(embedded_host_facts), "embedded host facts"),
                _sha256_bytes(embedded_host_facts),
            ),
        )
    if (
        manifest_schema_version != 1
        or profile_name not in PROFILE_NAMES
        or validated_profile.name != profile_name
        or validated_profile.trading_mode != manifest.get("trading_mode")
        or profile_digest != validated_profile.sha256
        or manifest.get("live_transmission_enabled") is not False
        or manifest.get("automatic_activation") is not False
        or manifest.get("trading_mode") not in {"SIMULATION", "PAPER"}
    ):
        raise ValueError("rollback manifest violates deployment safety")
    canonical_manifest = dict(manifest)
    canonical_manifest["manifest_sha256"] = claimed_manifest_hash
    canonical_manifest_bytes = (
        json.dumps(canonical_manifest, indent=2, sort_keys=True).encode() + b"\n"
    )
    if canonical_manifest_bytes != raw_manifest:
        raise ValueError("rollback manifest encoding is not canonical")
    canonical_entries = dict(entries)
    canonical_entries["rollback-manifest.json"] = raw_manifest
    if (
        _encode_archive(
            canonical_entries,
            source_date_epoch,
        )
        != package_bytes
    ):
        raise ValueError("rollback package encoding is not canonical")
    result: dict[str, object] = {
        "file_count": len(entries) + 1,
        "package_sha256": _sha256_bytes(package_bytes),
        "profile": profile_name,
        "verified": True,
    }
    if embedded_host_facts is not None:
        result["host_facts_sha256"] = _sha256_bytes(embedded_host_facts)
    return result


def _health_path(profile: EdgeProfile, service: str, status_root: Path | None) -> Path:
    root = Path(profile.health.status_root) if status_root is None else status_root
    return root / f"{service}.json"


def check_health(
    profile: EdgeProfile,
    service: str,
    *,
    status_root: Path | None = None,
    now_monotonic_ns: int | None = None,
) -> Mapping[str, object]:
    """Validate one fail-closed service status record."""
    if service == "clock-quality" and not profile.ptp.required:
        return {"ready": True, "service": service, "status": "NOT_REQUIRED"}
    if service not in {*ROLE_UNITS, "clock-quality"}:
        raise ValueError("health service identity is unsupported")
    value = _load_json(
        _health_path(profile, service, status_root), maximum_bytes=MAX_HEALTH_BYTES
    )
    required = {
        "schema_version",
        "service",
        "build_version",
        "healthy",
        "ready",
        "configuration_sha256",
        "mode",
        "observed_process_monotonic_time_ns",
    }
    if service == "clock-quality":
        required.update(
            {
                "clock_state",
                "hardware_timestamp_available",
                "ptp_offset_ns",
            }
        )
    _expect_keys(value, required, "health status")
    observed = _integer(
        value["observed_process_monotonic_time_ns"],
        "observed_process_monotonic_time_ns",
        minimum=1,
    )
    schema_version = _integer(
        value["schema_version"], "health.schema_version", minimum=1, maximum=1
    )
    build_version = _text(value["build_version"], "build_version")
    reported_service = _text(value["service"], "service")
    reported_mode = _text(value["mode"], "mode")
    current = time.monotonic_ns() if now_monotonic_ns is None else now_monotonic_ns
    config_hash = _text(value["configuration_sha256"], "configuration_sha256")
    maximum_age = (
        profile.ptp.maximum_clock_quality_age_ns
        if service == "clock-quality"
        else profile.health.maximum_status_age_ns
    )
    if (
        current < observed
        or current - observed > maximum_age
        or schema_version != 1
        or reported_service != service
        or not _boolean(value["healthy"], "healthy")
        or not _boolean(value["ready"], "ready")
        or len(config_hash) != SHA256_LENGTH
        or any(character not in "0123456789abcdef" for character in config_hash)
        or reported_mode != profile.trading_mode
    ):
        raise ValueError("service health is stale, malformed, or not ready")
    if service == "clock-quality":
        offset = value["ptp_offset_ns"]
        if (
            _text(value["clock_state"], "clock_state") != "HEALTHY"
            or value["hardware_timestamp_available"] is not True
            or not isinstance(offset, int)
            or isinstance(offset, bool)
            or abs(offset) > profile.ptp.maximum_absolute_offset_ns
        ):
            raise ValueError("clock quality does not permit edge readiness")
    return {
        "age_ns": current - observed,
        "build_version": build_version,
        "configuration_sha256": config_hash,
        "ready": True,
        "service": service,
        "status": "HEALTHY",
    }


def check_all_health(
    profile: EdgeProfile,
    *,
    status_root: Path | None = None,
    now_monotonic_ns: int | None = None,
) -> Mapping[str, object]:
    """Validate PTP and every edge role in deterministic startup order."""
    services = ("clock-quality", *ROLE_UNITS)
    results = [
        check_health(
            profile,
            service,
            status_root=status_root,
            now_monotonic_ns=now_monotonic_ns,
        )
        for service in services
    ]
    configuration_hashes = {
        result["configuration_sha256"]
        for result in results
        if "configuration_sha256" in result
    }
    if len(configuration_hashes) != 1:
        raise ValueError("edge services do not share one configuration hash")
    return {"profile": profile.name, "ready": True, "services": results}


def load_host_facts(path: Path) -> HostFacts:
    """Load an operator-captured hardware inventory without probing the host."""
    raw = path.read_bytes()
    if not raw or len(raw) > MAX_JSON_BYTES:
        raise ValueError("host facts size is invalid")
    return _parse_host_facts(
        _mapping(_decode_json(raw), "host facts"),
        _sha256_bytes(raw),
    )


def _parse_host_facts(value: Mapping[str, Any], digest: str) -> HostFacts:
    _expect_keys(
        value,
        {
            "host_facts_schema_version",
            "online_cpus",
            "online_numa_nodes",
            "cpu_numa_nodes",
            "hugepage_size_kb",
            "hugepage_total",
            "hugepages_by_numa_node",
            "interfaces",
            "interface_numa_nodes",
            "hardware_timestamp_interfaces",
            "receive_queue_counts",
        },
        "host facts",
    )
    _integer(
        value["host_facts_schema_version"],
        "host_facts_schema_version",
        minimum=1,
        maximum=1,
    )

    def integer_set(field: str) -> frozenset[int]:
        raw = value[field]
        if not isinstance(raw, list) or not raw:
            raise ValueError(f"{field} must be a non-empty array")
        parsed = tuple(_integer(item, field, maximum=MAX_CPU_ID) for item in raw)
        if len(set(parsed)) != len(parsed):
            raise ValueError(f"{field} contains duplicate values")
        return frozenset(parsed)

    def text_set(field: str) -> frozenset[str]:
        raw = value[field]
        if not isinstance(raw, list):
            raise ValueError(f"{field} must be an array")
        parsed = tuple(_text(item, field) for item in raw)
        if len(set(parsed)) != len(parsed):
            raise ValueError(f"{field} contains duplicate values")
        return frozenset(parsed)

    queue_values = _mapping(value["receive_queue_counts"], "receive_queue_counts")
    queues = {
        _text(interface, "queue interface"): _integer(
            count, "queue count", maximum=65_536
        )
        for interface, count in queue_values.items()
    }

    def numeric_node_mapping(field: str, *, maximum_value: int) -> Mapping[int, int]:
        raw = _mapping(value[field], field)
        return {
            _integer_text(key): _integer(node, field, maximum=maximum_value)
            for key, node in raw.items()
        }

    def interface_node_mapping(field: str) -> Mapping[str, int]:
        raw = _mapping(value[field], field)
        return {
            _text(key, field): _integer(node, field, maximum=1_024)
            for key, node in raw.items()
        }

    facts = HostFacts(
        online_cpus=integer_set("online_cpus"),
        online_numa_nodes=integer_set("online_numa_nodes"),
        cpu_numa_nodes=numeric_node_mapping("cpu_numa_nodes", maximum_value=1_024),
        hugepage_size_kb=_integer(
            value["hugepage_size_kb"], "hugepage_size_kb", minimum=2_048
        ),
        hugepage_total=_integer(
            value["hugepage_total"], "hugepage_total", maximum=1_048_576
        ),
        hugepages_by_numa_node=numeric_node_mapping(
            "hugepages_by_numa_node", maximum_value=1_048_576
        ),
        interfaces=text_set("interfaces"),
        interface_numa_nodes=interface_node_mapping("interface_numa_nodes"),
        hardware_timestamp_interfaces=text_set("hardware_timestamp_interfaces"),
        receive_queue_counts=queues,
        sha256=digest,
    )
    if (
        set(facts.cpu_numa_nodes) != set(facts.online_cpus)
        or not set(facts.cpu_numa_nodes.values()) <= set(facts.online_numa_nodes)
        or not set(facts.hugepages_by_numa_node) <= set(facts.online_numa_nodes)
        or set(facts.interface_numa_nodes) != set(facts.interfaces)
        or not set(facts.interface_numa_nodes.values()) <= set(facts.online_numa_nodes)
        or not facts.hardware_timestamp_interfaces <= facts.interfaces
        or set(facts.receive_queue_counts) != set(facts.interfaces)
    ):
        raise ValueError("host facts contain an incomplete or inconsistent topology")
    return facts


def validate_host(profile: EdgeProfile, facts: HostFacts) -> Mapping[str, object]:
    """Fail closed when hardware facts cannot satisfy one profile."""
    required_cpus = frozenset(
        cpu
        for cpu_set in profile.resources.cpu_affinity.values()
        for cpu in parse_cpu_set(cpu_set)
    )
    if not required_cpus <= facts.online_cpus:
        raise ValueError("profile CPU affinity exceeds online CPUs")
    if profile.resources.numa_node not in facts.online_numa_nodes:
        raise ValueError("profile NUMA node is offline")
    if any(
        facts.cpu_numa_nodes.get(cpu) != profile.resources.numa_node
        for cpu in required_cpus
    ):
        raise ValueError("profile CPU affinity is not local to its NUMA node")
    if profile.resources.hugepage_count > 0 and (
        facts.hugepage_size_kb != profile.resources.hugepage_size_kb
        or facts.hugepage_total < profile.resources.hugepage_count
        or facts.hugepages_by_numa_node.get(profile.resources.numa_node, 0)
        < profile.resources.hugepage_count
    ):
        raise ValueError("configured huge pages do not satisfy the profile")
    interface = profile.nic.interface
    if interface == "OPERATOR_REQUIRED":
        raise ValueError("operator NIC mapping is unresolved")
    if interface is not None:
        if interface not in facts.interfaces:
            raise ValueError("profile NIC is unavailable")
        if facts.interface_numa_nodes.get(interface) != profile.resources.numa_node:
            raise ValueError("profile NIC is not local to its NUMA node")
        if (
            profile.nic.hardware_timestamping_required
            and interface not in facts.hardware_timestamp_interfaces
        ):
            raise ValueError("profile NIC lacks verified hardware timestamping")
        if (
            facts.receive_queue_counts.get(interface, 0)
            < profile.nic.rx_queue_count_required
        ):
            raise ValueError("profile NIC has too few receive queues")
    return {
        "cpu_count_required": len(required_cpus),
        "hugepage_count_required": profile.resources.hugepage_count,
        "host_facts_sha256": facts.sha256,
        "interface": interface,
        "numa_node": profile.resources.numa_node,
        "profile": profile.name,
        "validated": True,
    }


def write_inhibit(reason: str, output: Path) -> Mapping[str, object]:
    """Atomically publish a local fail-closed inhibit record."""
    reason = _text(reason, "inhibit reason")
    record = {
        "live_transmission_enabled": False,
        "mode": "DISABLED",
        "observed_process_monotonic_time_ns": time.monotonic_ns(),
        "reason": reason,
        "schema_version": 1,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(f".{output.name}.{os.getpid()}.tmp")
    try:
        temporary.write_bytes(_canonical_bytes(record) + b"\n")
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
    return record


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Manage fail-closed Aegis-MX colocated edge artifacts."
    )
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("validate-assets")
    profile_parser = commands.add_parser("validate-profile")
    _add_profile_selector(profile_parser)
    render_parser = commands.add_parser("render")
    _add_profile_selector(render_parser)
    render_parser.add_argument("--output", type=Path, required=True)
    package_parser = commands.add_parser("package-rollback")
    _add_profile_selector(package_parser)
    package_parser.add_argument("--output", type=Path, required=True)
    package_parser.add_argument("--facts", type=Path)
    package_parser.add_argument(
        "--source-date-epoch", type=int, default=DEFAULT_SOURCE_DATE_EPOCH
    )
    verify_parser = commands.add_parser("verify-rollback")
    verify_parser.add_argument("--package", type=Path, required=True)
    health_parser = commands.add_parser("check-health")
    _add_profile_selector(health_parser)
    health_parser.add_argument("--service", required=True)
    health_parser.add_argument("--status-root", type=Path)
    all_health_parser = commands.add_parser("check-all-health")
    _add_profile_selector(all_health_parser)
    all_health_parser.add_argument("--status-root", type=Path)
    host_parser = commands.add_parser("validate-host")
    _add_profile_selector(host_parser)
    host_parser.add_argument("--facts", type=Path, required=True)
    inhibit_parser = commands.add_parser("inhibit")
    inhibit_parser.add_argument("--reason", required=True)
    inhibit_parser.add_argument("--output", type=Path, required=True)
    return parser


def _add_profile_selector(parser: argparse.ArgumentParser) -> None:
    selector = parser.add_mutually_exclusive_group(required=True)
    selector.add_argument("--profile", choices=PROFILE_NAMES)
    selector.add_argument("--profile-path", type=Path)


def _selected_profile(arguments: argparse.Namespace) -> EdgeProfile:
    path = cast("Path | None", getattr(arguments, "profile_path", None))
    if path is not None:
        return load_profile(path)
    return load_named_profile(cast("str", arguments.profile))


def main(argv: Sequence[str] | None = None) -> int:
    """Run one command and return zero only for a validated outcome."""
    arguments = _parser().parse_args(argv)
    try:
        if arguments.command == "validate-assets":
            result = validate_assets()
        elif arguments.command == "validate-profile":
            profile = _selected_profile(arguments)
            result = {
                "live_transmission_enabled": profile.live_transmission_enabled,
                "profile": profile.name,
                "profile_sha256": profile.sha256,
                "trading_mode": profile.trading_mode,
                "validated": True,
            }
        elif arguments.command == "render":
            result = render_profile(_selected_profile(arguments), arguments.output)
        elif arguments.command == "package-rollback":
            result = build_rollback_package(
                _selected_profile(arguments),
                arguments.output,
                arguments.source_date_epoch,
                arguments.facts,
            )
        elif arguments.command == "verify-rollback":
            result = verify_rollback_package(arguments.package)
        elif arguments.command == "check-health":
            result = check_health(
                _selected_profile(arguments),
                arguments.service,
                status_root=arguments.status_root,
            )
        elif arguments.command == "check-all-health":
            result = check_all_health(
                _selected_profile(arguments),
                status_root=arguments.status_root,
            )
        elif arguments.command == "validate-host":
            result = validate_host(
                _selected_profile(arguments),
                load_host_facts(arguments.facts),
            )
        elif arguments.command == "inhibit":
            result = write_inhibit(arguments.reason, arguments.output)
        else:
            raise ValueError("unsupported deployment command")
    except (
        EOFError,
        OSError,
        ValueError,
        json.JSONDecodeError,
        tarfile.TarError,
    ) as error:
        print(f"EDGE DEPLOYMENT ERROR: {error}")
        return 2
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
