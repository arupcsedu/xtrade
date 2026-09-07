"""Summarize and gate Aegis-MX full-platform performance evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any, Final, cast

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence

REPOSITORY_ROOT: Final = Path(__file__).resolve().parents[1]
DEFAULT_POLICY: Final = REPOSITORY_ROOT / "infra/benchmarks/performance-policy-v1.json"
MAX_INPUT_BYTES: Final = 512 * 1024 * 1024
MAX_LINE_BYTES: Final = 16 * 1024
MAX_NOTES: Final = 16
PARTS_PER_MILLION: Final = 1_000_000

JsonObject = dict[str, Any]


@dataclass(frozen=True, slots=True)
class Policy:
    """Validated regression and completeness policy."""

    policy_id: str
    minimum_samples: int
    scenarios: tuple[str, ...]
    stages: tuple[str, ...]
    latency_limits_ppm: Mapping[str, int]
    minimum_throughput_ratio_ppm: int
    maximum_cpu_increase_ppm: int
    maximum_queue_increase_ppm: int
    allocation_limit: int
    ordinary_drop_limit: int
    allocation_free_stages: frozenset[str]
    safety_blocks: frozenset[tuple[str, str]]
    require_same_cpu: bool
    require_same_build_type: bool
    require_pinning: bool
    require_same_source: bool
    require_real_nic_parity: bool


@dataclass(slots=True)
class Samples:
    """Raw latency observations for one stage/scenario case."""

    values: list[int]


class DuplicateKeyError(ValueError):
    """Raised when untrusted JSON contains duplicate object keys."""


def _reject_duplicate_pairs(pairs: list[tuple[str, Any]]) -> JsonObject:
    output: JsonObject = {}
    for key, value in pairs:
        if key in output:
            raise DuplicateKeyError(f"duplicate JSON key: {key}")
        output[key] = value
    return output


def _read_json(path: Path) -> JsonObject:
    size = path.stat().st_size
    if size <= 0 or size > MAX_INPUT_BYTES:
        raise ValueError(f"invalid input size for {path}: {size}")
    value = json.loads(
        path.read_text(encoding="utf-8"), object_pairs_hook=_reject_duplicate_pairs
    )
    if not isinstance(value, dict):
        raise ValueError(f"expected a JSON object: {path}")
    return cast("JsonObject", value)


def _integer(value: object, name: str, *, minimum: int = 0) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise ValueError(f"{name} must be an integer >= {minimum}")
    return value


def _text(value: object, name: str, *, maximum: int = 512) -> str:
    if not isinstance(value, str) or not value or len(value.encode()) > maximum:
        raise ValueError(f"{name} must be nonempty and at most {maximum} bytes")
    return value


def _sha256_text(value: object, name: str) -> str:
    result = _text(value, name, maximum=64)
    if re.fullmatch(r"[a-f0-9]{64}", result) is None:
        raise ValueError(f"{name} must be a lowercase SHA-256 digest")
    return result


def _boolean(value: object, name: str) -> bool:
    if not isinstance(value, bool):
        raise ValueError(f"{name} must be boolean")
    return value


def _string_tuple(value: object, name: str) -> tuple[str, ...]:
    if not isinstance(value, list) or not value:
        raise ValueError(f"{name} must be a nonempty array")
    output = tuple(_text(item, name, maximum=96) for item in value)
    if len(output) != len(set(output)):
        raise ValueError(f"{name} values must be unique")
    return output


def load_policy(path: Path = DEFAULT_POLICY) -> Policy:
    """Load and strictly validate the benchmark comparison policy."""
    value = _read_json(path)
    if value.get("policy_schema_version") != 1:
        raise ValueError("unsupported performance policy schema version")
    limits_value = value.get("latency_regression_limit_ppm")
    if not isinstance(limits_value, dict):
        raise ValueError("latency limits must be an object")
    percentile_names = ("p50", "p95", "p99", "p99_9", "maximum")
    limits = {
        name: _integer(limits_value.get(name), f"latency limit {name}")
        for name in percentile_names
    }
    safety_value = value.get("required_safety_blocks")
    if not isinstance(safety_value, list):
        raise ValueError("required_safety_blocks must be an array")
    safety_blocks: set[tuple[str, str]] = set()
    for index, item in enumerate(safety_value):
        if not isinstance(item, dict):
            raise ValueError(f"safety block {index} must be an object")
        safety_blocks.add(
            (
                _text(item.get("scenario"), "safety scenario", maximum=64),
                _text(item.get("stage"), "safety stage", maximum=96),
            )
        )
    return Policy(
        policy_id=_text(value.get("policy_id"), "policy_id", maximum=128),
        minimum_samples=_integer(
            value.get("minimum_qualified_samples_per_latency_case"),
            "minimum samples",
            minimum=1,
        ),
        scenarios=_string_tuple(value.get("required_scenarios"), "required_scenarios"),
        stages=_string_tuple(value.get("required_stages"), "required_stages"),
        latency_limits_ppm=limits,
        minimum_throughput_ratio_ppm=_integer(
            value.get("minimum_throughput_ratio_ppm"), "throughput ratio"
        ),
        maximum_cpu_increase_ppm=_integer(
            value.get("maximum_cpu_utilization_increase_ppm"), "CPU increase"
        ),
        maximum_queue_increase_ppm=_integer(
            value.get("maximum_queue_occupancy_increase_ppm"), "queue increase"
        ),
        allocation_limit=_integer(
            value.get("hot_path_allocation_limit"), "allocation limit"
        ),
        ordinary_drop_limit=_integer(
            value.get("ordinary_packet_drop_limit"), "ordinary drop limit"
        ),
        allocation_free_stages=frozenset(
            _string_tuple(value.get("allocation_free_stages"), "allocation stages")
        ),
        safety_blocks=frozenset(safety_blocks),
        require_same_cpu=_boolean(value.get("require_same_cpu_model"), "same CPU"),
        require_same_build_type=_boolean(
            value.get("require_same_build_type"), "same build type"
        ),
        require_pinning=_boolean(
            value.get("require_verified_thread_pinning"), "thread pinning"
        ),
        require_same_source=_boolean(
            value.get("require_same_measurement_source"), "measurement source"
        ),
        require_real_nic_parity=_boolean(
            value.get("require_real_nic_parity"), "real NIC parity"
        ),
    )


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def _percentile(sorted_values: Sequence[int], thousandths: int) -> int:
    if not sorted_values:
        raise ValueError("cannot calculate a percentile without samples")
    index = ((len(sorted_values) - 1) * thousandths) // 1_000
    return sorted_values[index]


def _read_samples(path: Path, policy: Policy) -> dict[tuple[str, str], Samples]:
    if path.stat().st_size <= 0 or path.stat().st_size > MAX_INPUT_BYTES:
        raise ValueError("raw sample file size is invalid")
    valid_scenarios = set(policy.scenarios)
    valid_stages = set(policy.stages)
    output: dict[tuple[str, str], Samples] = {}
    with path.open(encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, start=1):
            if len(line.encode()) > MAX_LINE_BYTES:
                raise ValueError(f"raw sample line {line_number} is too large")
            item = json.loads(line, object_pairs_hook=_reject_duplicate_pairs)
            if not isinstance(item, dict) or set(item) != {
                "latency_ns",
                "scenario",
                "stage",
            }:
                raise ValueError(f"raw sample line {line_number} has invalid fields")
            scenario = _text(item["scenario"], "sample scenario", maximum=64)
            stage = _text(item["stage"], "sample stage", maximum=96)
            if scenario not in valid_scenarios or stage not in valid_stages:
                raise ValueError(f"raw sample line {line_number} has unknown case")
            latency = _integer(item["latency_ns"], "sample latency")
            output.setdefault((scenario, stage), Samples([])).values.append(latency)
    return output


def _optional_integer(value: object, name: str, *, minimum: int = 0) -> int | None:
    if value is None:
        return None
    return _integer(value, name, minimum=minimum)


def _case_measurement(
    case: JsonObject,
    raw: dict[tuple[str, str], Samples],
    policy: Policy,
) -> JsonObject:
    scenario = _text(case.get("scenario"), "case scenario", maximum=64)
    stage = _text(case.get("stage"), "case stage", maximum=96)
    if scenario not in policy.scenarios or stage not in policy.stages:
        raise ValueError(f"unknown benchmark case: {scenario}/{stage}")
    status = _text(case.get("status"), "case status", maximum=32)
    if status not in {"MEASURED", "SAFETY_BLOCKED", "UNAVAILABLE"}:
        raise ValueError(f"invalid case status: {status}")
    operation_count = _integer(case.get("operation_count"), "operation_count")
    elapsed_ns = _integer(case.get("elapsed_ns"), "elapsed_ns")
    cpu_time_ns = _optional_integer(case.get("cpu_time_ns"), "cpu_time_ns")
    allocations = _optional_integer(case.get("allocations"), "allocations")
    cache_misses = _optional_integer(case.get("cache_misses"), "cache_misses")
    cache_reason = case.get("cache_misses_unavailable_reason")
    if cache_reason is not None:
        cache_reason = _text(cache_reason, "cache miss reason")
    queue_capacity = _optional_integer(
        case.get("queue_capacity"), "queue_capacity", minimum=1
    )
    queue_high = _optional_integer(case.get("queue_high_watermark"), "queue high")
    packet_drops = _integer(case.get("packet_drops"), "packet_drops")
    notes_value = case.get("notes")
    if not isinstance(notes_value, list) or len(notes_value) > MAX_NOTES:
        raise ValueError("case notes must be an array of at most 16 items")
    notes = [_text(item, "case note") for item in notes_value]
    observed = raw.get((scenario, stage), Samples([])).values
    latency: JsonObject | None = None
    throughput: float | None = None
    cpu_utilization: int | None = None
    if status == "MEASURED":
        if not observed or operation_count == 0 or elapsed_ns == 0:
            raise ValueError(f"measured case has no evidence: {scenario}/{stage}")
        ordered = sorted(observed)
        latency = {
            "maximum": ordered[-1],
            "p50": _percentile(ordered, 500),
            "p95": _percentile(ordered, 950),
            "p99": _percentile(ordered, 990),
            "p99_9": _percentile(ordered, 999),
        }
        throughput = (operation_count * 1_000_000_000) / elapsed_ns
        if cpu_time_ns is not None:
            cpu_utilization = (cpu_time_ns * PARTS_PER_MILLION) // elapsed_ns
    elif observed:
        raise ValueError(f"non-measured case has latency samples: {scenario}/{stage}")
    if (
        queue_capacity is not None
        and queue_high is not None
        and queue_high > queue_capacity
    ):
        raise ValueError(f"queue high-watermark exceeds capacity: {scenario}/{stage}")
    if cache_misses is None and cache_reason is None:
        cache_reason = "hardware counter not requested for this case"
    if cache_misses is not None and cache_reason is not None:
        raise ValueError(
            "cache miss value and unavailable reason are mutually exclusive"
        )
    return {
        "allocations": allocations,
        "cache_misses": cache_misses,
        "cache_misses_unavailable_reason": cache_reason,
        "cpu_utilization_ppm": cpu_utilization,
        "latency_ns": latency,
        "notes": notes,
        "operation_count": operation_count,
        "packet_drops": packet_drops,
        "queue_capacity": queue_capacity,
        "queue_high_watermark": queue_high,
        "sample_count": len(observed),
        "scenario": scenario,
        "stage": stage,
        "status": status,
        "throughput_events_per_second": throughput,
    }


def _validate_environment(value: object) -> JsonObject:
    if not isinstance(value, dict):
        raise ValueError("environment must be an object")
    result = cast("JsonObject", value)
    required = {
        "build_type",
        "compiler",
        "cpu_governor",
        "cpu_model",
        "hostname",
        "kernel",
        "logical_cpu_count",
        "numa_node",
        "operating_system",
        "pinned_cpu",
        "thread_pinning_verified",
    }
    if set(result) != required:
        raise ValueError("environment fields do not match the report contract")
    for field in ("hostname", "operating_system", "kernel", "cpu_model", "compiler"):
        _text(result.get(field), f"environment {field}")
    _integer(result.get("logical_cpu_count"), "logical_cpu_count", minimum=1)
    _integer(result.get("pinned_cpu"), "pinned_cpu")
    _boolean(result.get("thread_pinning_verified"), "thread_pinning_verified")
    if result.get("build_type") not in {"Release", "RelWithDebInfo"}:
        raise ValueError("benchmark build_type must be optimized")
    numa_node = result.get("numa_node")
    if numa_node is not None:
        _integer(numa_node, "numa_node")
    governor = result.get("cpu_governor")
    if governor is not None:
        _text(governor, "cpu_governor", maximum=128)
    return result


def summarize(
    metadata_path: Path,
    samples_path: Path,
    policy: Policy,
    output_path: Path,
) -> JsonObject:
    """Create a normalized report from sampler metadata and raw NDJSON."""
    metadata = _read_json(metadata_path)
    if metadata.get("sampler_schema_version") != 1:
        raise ValueError("unsupported sampler schema version")
    mode = metadata.get("mode")
    if mode not in {"SIMULATION", "PAPER"}:
        raise ValueError("benchmark mode must be SIMULATION or PAPER")
    source = metadata.get("measurement_source")
    if source not in {"IN_PROCESS_SYNTHETIC", "REAL_NIC_CAPTURE"}:
        raise ValueError("invalid measurement source")
    methodology = metadata.get("methodology")
    if not isinstance(methodology, dict):
        raise ValueError("methodology must be an object")
    warmup = _integer(methodology.get("warmup_iterations"), "warmup", minimum=1)
    requested_samples = _integer(
        methodology.get("samples_per_latency_case"), "samples", minimum=1
    )
    if methodology.get("clock") != "CLOCK_MONOTONIC_RAW":
        raise ValueError("benchmark must use CLOCK_MONOTONIC_RAW")
    if methodology.get("caches_warmed") is not True:
        raise ValueError("benchmark must warm caches")
    raw = _read_samples(samples_path, policy)
    cases = metadata.get("cases")
    if not isinstance(cases, list):
        raise ValueError("sampler cases must be an array")
    measurements = [
        _case_measurement(cast("JsonObject", case), raw, policy)
        for case in cases
        if isinstance(case, dict)
    ]
    if len(measurements) != len(cases):
        raise ValueError("every sampler case must be an object")
    expected = {
        (scenario, stage) for scenario in policy.scenarios for stage in policy.stages
    }
    actual = {
        (cast("str", item["scenario"]), cast("str", item["stage"]))
        for item in measurements
    }
    if actual != expected or len(measurements) != len(actual):
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        raise ValueError(
            f"incomplete or duplicate case matrix: missing={missing}, extra={extra}"
        )
    for required in policy.safety_blocks:
        matching = next(
            item
            for item in measurements
            if (item["scenario"], item["stage"]) == required
        )
        if matching["status"] != "SAFETY_BLOCKED":
            raise ValueError(f"required safety block was not observed: {required}")
    minimum_observed = min(
        cast("int", item["sample_count"])
        for item in measurements
        if item["status"] == "MEASURED"
    )
    cache_available = bool(methodology.get("hardware_cache_misses_available"))
    cache_reason = methodology.get("hardware_cache_misses_unavailable_reason")
    if cache_available and cache_reason is not None:
        raise ValueError("available cache counters cannot have an unavailable reason")
    if not cache_available:
        cache_reason = _text(cache_reason, "hardware cache unavailable reason")
    raw_reference = os.path.relpath(
        samples_path.resolve(), output_path.parent.resolve()
    )
    report: JsonObject = {
        "code_commit": _text(metadata.get("code_commit"), "code_commit", maximum=128),
        "configuration_sha256": _sha256_text(
            metadata.get("configuration_sha256"), "configuration_sha256"
        ),
        "environment": _validate_environment(metadata.get("environment")),
        "measurement_source": source,
        "measurements": measurements,
        "methodology": {
            "allocation_probe_enabled": _boolean(
                methodology.get("allocation_probe_enabled"), "allocation probe"
            ),
            "caches_warmed": True,
            "clock": "CLOCK_MONOTONIC_RAW",
            "hardware_cache_misses_available": cache_available,
            "hardware_cache_misses_unavailable_reason": cache_reason,
            "qualified_sample_count": minimum_observed >= policy.minimum_samples,
            "samples_per_latency_case": requested_samples,
            "warmup_iterations": warmup,
        },
        "mode": mode,
        "raw_samples": {
            "format": "aegis-performance-samples-v1-ndjson",
            "path": raw_reference,
            "record_count": sum(len(item.values) for item in raw.values()),
            "sha256": _sha256_file(samples_path),
        },
        "report_schema_version": 1,
        "seed": _integer(metadata.get("seed"), "seed", minimum=1),
        "suite_id": _text(metadata.get("suite_id"), "suite_id", maximum=128),
    }
    _atomic_write_json(output_path, report)
    return report


def _measurement_map(report: JsonObject) -> dict[tuple[str, str], JsonObject]:
    values = report.get("measurements")
    if not isinstance(values, list):
        raise ValueError("report measurements must be an array")
    output: dict[tuple[str, str], JsonObject] = {}
    for value in values:
        if not isinstance(value, dict):
            raise ValueError("measurement must be an object")
        item = cast("JsonObject", value)
        key = (
            _text(item.get("scenario"), "measurement scenario", maximum=64),
            _text(item.get("stage"), "measurement stage", maximum=96),
        )
        if key in output:
            raise ValueError(f"duplicate measurement: {key}")
        output[key] = item
    return output


def _verify_raw_evidence(report: JsonObject, report_path: Path, policy: Policy) -> None:
    raw_reference = report.get("raw_samples")
    if not isinstance(raw_reference, dict) or set(raw_reference) != {
        "format",
        "path",
        "record_count",
        "sha256",
    }:
        raise ValueError(f"raw evidence reference is invalid: {report_path}")
    if raw_reference.get("format") != "aegis-performance-samples-v1-ndjson":
        raise ValueError(f"raw evidence format is invalid: {report_path}")
    raw_path_text = _text(raw_reference.get("path"), "raw evidence path", maximum=4096)
    expected_hash = _sha256_text(raw_reference.get("sha256"), "raw evidence sha256")
    expected_count = _integer(
        raw_reference.get("record_count"), "raw evidence record_count", minimum=1
    )
    raw_path = Path(raw_path_text)
    if not raw_path.is_absolute():
        raw_path = report_path.parent / raw_path
    observed_hash = _sha256_file(raw_path)
    if observed_hash != expected_hash:
        raise ValueError(f"raw evidence hash mismatch: {raw_path}")
    samples = _read_samples(raw_path, policy)
    observed_count = sum(len(item.values) for item in samples.values())
    if observed_count != expected_count:
        raise ValueError(f"raw evidence record count mismatch: {raw_path}")

    measurements = _measurement_map(report)
    for key, measurement in measurements.items():
        sample_count = _integer(
            measurement.get("sample_count"), f"sample_count for {key}"
        )
        observed = sorted(samples.get(key, Samples([])).values)
        if sample_count != len(observed):
            raise ValueError(f"raw evidence sample count mismatch: {key}")
        status = measurement.get("status")
        if status == "MEASURED":
            if not observed:
                raise ValueError(f"measured report case has no raw evidence: {key}")
            expected_latency = {
                "maximum": observed[-1],
                "p50": _percentile(observed, 500),
                "p95": _percentile(observed, 950),
                "p99": _percentile(observed, 990),
                "p99_9": _percentile(observed, 999),
            }
            if measurement.get("latency_ns") != expected_latency:
                raise ValueError(f"summary latency does not match raw evidence: {key}")
        elif observed:
            raise ValueError(f"non-measured report case has raw evidence: {key}")


def _difference_limit(baseline: int, regression_ppm: int) -> int:
    return baseline + ((baseline * regression_ppm) // PARTS_PER_MILLION)


def _comparison_failure(
    failures: list[JsonObject],
    key: tuple[str, str],
    metric: str,
    baseline: object,
    candidate: object,
    limit: object,
) -> None:
    failures.append(
        {
            "baseline": baseline,
            "candidate": candidate,
            "limit": limit,
            "metric": metric,
            "scenario": key[0],
            "stage": key[1],
        }
    )


def _compare_environment(
    baseline: JsonObject,
    candidate: JsonObject,
    policy: Policy,
    failures: list[JsonObject],
) -> None:
    baseline_environment = cast("JsonObject", baseline["environment"])
    candidate_environment = cast("JsonObject", candidate["environment"])
    checks: list[tuple[bool, str, object, object]] = [
        (
            policy.require_same_cpu,
            "environment.cpu_model",
            baseline_environment.get("cpu_model"),
            candidate_environment.get("cpu_model"),
        ),
        (
            policy.require_same_build_type,
            "environment.build_type",
            baseline_environment.get("build_type"),
            candidate_environment.get("build_type"),
        ),
        (
            policy.require_same_source,
            "measurement_source",
            baseline.get("measurement_source"),
            candidate.get("measurement_source"),
        ),
    ]
    if policy.require_real_nic_parity:
        checks.append(
            (
                True,
                "real_nic_parity",
                baseline.get("measurement_source") == "REAL_NIC_CAPTURE",
                candidate.get("measurement_source") == "REAL_NIC_CAPTURE",
            )
        )
    for enabled, metric, expected, observed in checks:
        if enabled and expected != observed:
            _comparison_failure(
                failures, ("*", "*"), metric, expected, observed, expected
            )
    if policy.require_pinning:
        for label, environment in (
            ("baseline", baseline_environment),
            ("candidate", candidate_environment),
        ):
            if environment.get("thread_pinning_verified") is not True:
                _comparison_failure(
                    failures,
                    ("*", "*"),
                    f"{label}.thread_pinning_verified",
                    True,
                    environment.get("thread_pinning_verified"),
                    True,
                )


def compare_reports(
    baseline: JsonObject, candidate: JsonObject, policy: Policy
) -> JsonObject:
    """Compare qualified, compatible reports and return an auditable gate result."""
    failures: list[JsonObject] = []
    if (
        baseline.get("report_schema_version") != 1
        or candidate.get("report_schema_version") != 1
    ):
        raise ValueError("unsupported report schema version")
    for label, report in (("baseline", baseline), ("candidate", candidate)):
        methodology = report.get("methodology")
        if (
            not isinstance(methodology, dict)
            or methodology.get("qualified_sample_count") is not True
        ):
            _comparison_failure(
                failures,
                ("*", "*"),
                f"{label}.qualified_sample_count",
                True,
                None
                if not isinstance(methodology, dict)
                else methodology.get("qualified_sample_count"),
                True,
            )
    _compare_environment(baseline, candidate, policy, failures)
    baseline_map = _measurement_map(baseline)
    candidate_map = _measurement_map(candidate)
    expected = {
        (scenario, stage) for scenario in policy.scenarios for stage in policy.stages
    }
    if set(baseline_map) != expected or set(candidate_map) != expected:
        raise ValueError("reports do not contain the complete required case matrix")
    for key in sorted(expected):
        reference = baseline_map[key]
        observed = candidate_map[key]
        reference_status = reference.get("status")
        observed_status = observed.get("status")
        if key in policy.safety_blocks and observed_status != "SAFETY_BLOCKED":
            _comparison_failure(
                failures,
                key,
                "safety_status",
                "SAFETY_BLOCKED",
                observed_status,
                "SAFETY_BLOCKED",
            )
            continue
        if reference_status != observed_status:
            _comparison_failure(
                failures,
                key,
                "status",
                reference_status,
                observed_status,
                reference_status,
            )
            continue
        if observed_status != "MEASURED":
            continue
        reference_latency = cast("JsonObject", reference["latency_ns"])
        observed_latency = cast("JsonObject", observed["latency_ns"])
        for percentile, regression_ppm in policy.latency_limits_ppm.items():
            baseline_value = _integer(reference_latency.get(percentile), percentile)
            candidate_value = _integer(observed_latency.get(percentile), percentile)
            limit = _difference_limit(baseline_value, regression_ppm)
            if candidate_value > limit:
                _comparison_failure(
                    failures,
                    key,
                    f"latency_ns.{percentile}",
                    baseline_value,
                    candidate_value,
                    limit,
                )
        baseline_throughput = float(reference["throughput_events_per_second"])
        candidate_throughput = float(observed["throughput_events_per_second"])
        minimum_throughput = (
            baseline_throughput * policy.minimum_throughput_ratio_ppm
        ) / PARTS_PER_MILLION
        if candidate_throughput < minimum_throughput:
            _comparison_failure(
                failures,
                key,
                "throughput_events_per_second",
                baseline_throughput,
                candidate_throughput,
                minimum_throughput,
            )
        baseline_cpu = reference.get("cpu_utilization_ppm")
        candidate_cpu = observed.get("cpu_utilization_ppm")
        if isinstance(baseline_cpu, int) and isinstance(candidate_cpu, int):
            maximum_cpu = _difference_limit(
                baseline_cpu, policy.maximum_cpu_increase_ppm
            )
            if candidate_cpu > maximum_cpu:
                _comparison_failure(
                    failures,
                    key,
                    "cpu_utilization_ppm",
                    baseline_cpu,
                    candidate_cpu,
                    maximum_cpu,
                )
        if key[1] in policy.allocation_free_stages:
            allocations = observed.get("allocations")
            if (
                not isinstance(allocations, int)
                or allocations > policy.allocation_limit
            ):
                _comparison_failure(
                    failures,
                    key,
                    "allocations",
                    policy.allocation_limit,
                    allocations,
                    policy.allocation_limit,
                )
        baseline_queue = reference.get("queue_high_watermark")
        candidate_queue = observed.get("queue_high_watermark")
        if isinstance(baseline_queue, int) and isinstance(candidate_queue, int):
            maximum_queue = _difference_limit(
                baseline_queue, policy.maximum_queue_increase_ppm
            )
            if candidate_queue > maximum_queue:
                _comparison_failure(
                    failures,
                    key,
                    "queue_high_watermark",
                    baseline_queue,
                    candidate_queue,
                    maximum_queue,
                )
        if (
            key[0] == "ordinary"
            and observed.get("packet_drops") != policy.ordinary_drop_limit
        ):
            _comparison_failure(
                failures,
                key,
                "packet_drops",
                policy.ordinary_drop_limit,
                observed.get("packet_drops"),
                policy.ordinary_drop_limit,
            )
    result: JsonObject = {
        "baseline_configuration_sha256": baseline.get("configuration_sha256"),
        "candidate_configuration_sha256": candidate.get("configuration_sha256"),
        "failure_count": len(failures),
        "failures": failures,
        "passed": not failures,
        "policy_id": policy.policy_id,
        "regression_report_schema_version": 1,
    }
    canonical = json.dumps(result, sort_keys=True, separators=(",", ":")).encode()
    result["result_sha256"] = hashlib.sha256(canonical).hexdigest()
    return result


def _atomic_write_json(path: Path, value: Mapping[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(value, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    except BaseException:
        Path(temporary).unlink(missing_ok=True)
        raise


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    summarize_parser = subparsers.add_parser("summarize")
    summarize_parser.add_argument("--metadata", type=Path, required=True)
    summarize_parser.add_argument("--samples", type=Path, required=True)
    summarize_parser.add_argument("--policy", type=Path, default=DEFAULT_POLICY)
    summarize_parser.add_argument("--output", type=Path, required=True)
    compare_parser = subparsers.add_parser("compare")
    compare_parser.add_argument("--baseline", type=Path, required=True)
    compare_parser.add_argument("--candidate", type=Path, required=True)
    compare_parser.add_argument("--policy", type=Path, default=DEFAULT_POLICY)
    compare_parser.add_argument("--output", type=Path, required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    """Run the report summarizer or regression gate."""
    arguments = _parser().parse_args(argv)
    try:
        policy = load_policy(arguments.policy)
        if arguments.command == "summarize":
            result = summarize(
                arguments.metadata, arguments.samples, policy, arguments.output
            )
            print(
                json.dumps(
                    {
                        "output": str(arguments.output),
                        "qualified": result["methodology"]["qualified_sample_count"],
                    },
                    sort_keys=True,
                )
            )
            return 0
        baseline = _read_json(arguments.baseline)
        candidate = _read_json(arguments.candidate)
        _verify_raw_evidence(baseline, arguments.baseline, policy)
        _verify_raw_evidence(candidate, arguments.candidate, policy)
        comparison = compare_reports(baseline, candidate, policy)
        _atomic_write_json(arguments.output, comparison)
        print(json.dumps(comparison, sort_keys=True))
        return 0 if comparison["passed"] else 1
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"PERFORMANCE ERROR: {error}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
