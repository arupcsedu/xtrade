"""Deterministic, simulation-only chaos scenario runner for Aegis-MX."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from dataclasses import dataclass
from enum import StrEnum
from pathlib import Path
from typing import TYPE_CHECKING, Any, Final, cast

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence

REPOSITORY_ROOT: Final = Path(__file__).resolve().parents[1]
DEFAULT_CATALOG: Final = REPOSITORY_ROOT / "infra/chaos/scenario-catalog-v1.json"
DEFAULT_SEED: Final = 20_260_828
MAX_CATALOG_BYTES: Final = 1_048_576
MAX_SCENARIOS: Final = 128
MAX_ITERATIONS: Final = 100_000
MAX_TEXT_BYTES: Final = 256
MAX_STRING_ITEMS: Final = 32
MIN_COMBINATION_SIZE: Final = 2
REPORT_SCHEMA_VERSION: Final = 1
SIMULATION_MODE: Final = "SIMULATION"

Scalar = bool | int | str


class FaultKind(StrEnum):
    """Closed fault vocabulary supported by the reference harness."""

    PACKET_LOSS = "packet-loss"
    PACKET_DUPLICATION = "packet-duplication"
    PACKET_REORDERING = "packet-reordering"
    FEED_GAP = "feed-gap"
    FEED_AB_DISAGREEMENT = "feed-a-b-disagreement"
    STALE_FEED = "stale-feed"
    CLOCK_DRIFT = "clock-drift"
    CLOCK_JUMP = "clock-jump"
    CPU_STARVATION = "cpu-starvation"
    MEMORY_PRESSURE = "memory-pressure"
    QUEUE_SATURATION = "queue-saturation"
    GPU_FAILURE = "gpu-failure"
    LLM_TIMEOUT = "llm-timeout"
    MALFORMED_MODEL_RESPONSE = "malformed-model-response"
    NEWS_CONTRADICTION = "news-contradiction"
    GATEWAY_DISCONNECT = "gateway-disconnect"
    REJECT_BURST = "reject-burst"
    DROP_COPY_DISAGREEMENT = "drop-copy-disagreement"
    JOURNAL_TRUNCATION = "journal-truncation"
    CONFIGURATION_CORRUPTION = "configuration-corruption"
    SPLIT_BRAIN = "split-brain"
    TRADING_HALT = "trading-halt"
    AUCTION_REOPENING = "auction-reopening"


@dataclass(frozen=True, slots=True)
class Transition:
    """Expected component-local state transition."""

    domain: str
    from_state: str
    to_state: str

    def as_dict(self) -> dict[str, str]:
        """Return the stable wire representation."""
        return {
            "domain": self.domain,
            "from": self.from_state,
            "to": self.to_state,
        }


@dataclass(frozen=True, slots=True)
class Injection:
    """One bounded signal mutation performed at a logical time."""

    signal: str
    value: Scalar
    duration_ns: int

    def as_dict(self) -> dict[str, Scalar]:
        """Return the stable wire representation."""
        return {
            "duration_ns": self.duration_ns,
            "signal": self.signal,
            "value": self.value,
        }


@dataclass(frozen=True, slots=True)
class Scenario:
    """Validated expected behavior for one fault."""

    scenario_id: str
    fault: FaultKind
    component: str
    profiles: tuple[str, ...]
    production_hook: str
    injection: Injection
    expected_detection: str
    expected_transition: Transition
    expected_automated_response: tuple[str, ...]
    maximum_detection_latency_ns: int
    recovery_criteria: tuple[str, ...]
    required_audit_events: tuple[str, ...]
    blocks_new_orders: bool


@dataclass(frozen=True, slots=True)
class Combination:
    """Nightly simultaneous-fault contract."""

    combination_id: str
    scenario_ids: tuple[str, ...]
    expected_safety_state: str


@dataclass(frozen=True, slots=True)
class ScenarioCatalog:
    """Immutable validated catalog plus its canonical content identity."""

    catalog_id: str
    catalog_schema_version: int
    scenarios: tuple[Scenario, ...]
    combinations: tuple[Combination, ...]
    sha256: str


@dataclass(frozen=True, slots=True)
class Behavior:
    """Independent detector/recovery behavior for one injected signal."""

    signal: str
    active_value: Scalar
    detection: str
    transition: Transition
    responses: tuple[str, ...]
    base_detection_latency_ns: int
    detection_jitter_ns: int
    recovery: tuple[str, ...]
    audit_events: tuple[str, ...]
    blocks_new_orders: bool


def _behavior(
    signal: str,
    active_value: Scalar,
    detection: str,
    domain: str,
    from_state: str,
    to_state: str,
    responses: tuple[str, ...],
    latency_ns: int,
    jitter_ns: int,
    recovery: tuple[str, ...],
    audit_events: tuple[str, ...],
    *,
    blocks_new_orders: bool,
) -> Behavior:
    return Behavior(
        signal,
        active_value,
        detection,
        Transition(domain, from_state, to_state),
        responses,
        latency_ns,
        jitter_ns,
        recovery,
        ("CHAOS_FAULT_INJECTED", *audit_events),
        blocks_new_orders,
    )


_BEHAVIORS: Final[Mapping[FaultKind, Behavior]] = {
    FaultKind.PACKET_LOSS: _behavior(
        "packet_delivery_ratio_ppm",
        900_000,
        "SEQUENCE_GAP",
        "FEED_STATE",
        "HEALTHY",
        "GAP_DETECTED",
        ("BLOCK_NEW_ORDERS", "REQUEST_RETRANSMISSION"),
        50_000,
        10_000,
        ("CONTIGUOUS_SEQUENCE_RESTORED", "BOOK_REBUILT", "FEED_STABILIZED"),
        ("SEQUENCE_GAP_DETECTED", "FEED_RECOVERY_STARTED", "FEED_HEALTHY_RESTORED"),
        blocks_new_orders=True,
    ),
    FaultKind.PACKET_DUPLICATION: _behavior(
        "duplicate_packet_count",
        1,
        "DUPLICATE_SEQUENCE",
        "FEED_STATE",
        "HEALTHY",
        "HEALTHY",
        ("DISCARD_DUPLICATE", "INCREMENT_DUPLICATE_METRIC"),
        15_000,
        5_000,
        ("DUPLICATE_ACCOUNTED", "SEQUENCE_REMAINS_MONOTONIC"),
        ("DUPLICATE_PACKET_DETECTED", "DUPLICATE_PACKET_DISCARDED"),
        blocks_new_orders=False,
    ),
    FaultKind.PACKET_REORDERING: _behavior(
        "packet_sequence_delta",
        -1,
        "OUT_OF_ORDER_SEQUENCE",
        "FEED_STATE",
        "HEALTHY",
        "GAP_DETECTED",
        ("QUARANTINE_PACKET", "BLOCK_NEW_ORDERS", "REQUEST_RETRANSMISSION"),
        35_000,
        10_000,
        ("ORDERED_SEQUENCE_RESTORED", "BOOK_REBUILT", "FEED_STABILIZED"),
        ("OUT_OF_ORDER_DETECTED", "FEED_RECOVERY_STARTED", "FEED_HEALTHY_RESTORED"),
        blocks_new_orders=True,
    ),
    FaultKind.FEED_GAP: _behavior(
        "explicit_gap_width",
        4,
        "EXPLICIT_FEED_GAP",
        "FEED_STATE",
        "HEALTHY",
        "RECOVERING",
        ("BLOCK_NEW_ORDERS", "REQUEST_GAP_REPLAY"),
        20_000,
        5_000,
        ("GAP_REPLAY_COMPLETE", "SEQUENCE_CONTIGUOUS", "BOOK_REBUILT"),
        ("FEED_GAP_DECLARED", "GAP_REPLAY_REQUESTED", "FEED_RECOVERY_COMPLETED"),
        blocks_new_orders=True,
    ),
    FaultKind.FEED_AB_DISAGREEMENT: _behavior(
        "feed_a_b_payload_match",
        False,
        "REDUNDANT_FEED_CONFLICT",
        "FEED_STATE",
        "HEALTHY",
        "INVALID",
        ("BLOCK_NEW_ORDERS", "QUARANTINE_CHANNEL", "REQUEST_SNAPSHOT_REBUILD"),
        65_000,
        10_000,
        ("FEEDS_AGREE", "SNAPSHOT_VERIFIED", "STABILIZATION_COMPLETE"),
        ("FEED_CONFLICT_DETECTED", "CHANNEL_QUARANTINED", "FEED_VALIDATED"),
        blocks_new_orders=True,
    ),
    FaultKind.STALE_FEED: _behavior(
        "feed_age_ns",
        12_000_000,
        "FEED_AGE_EXCEEDED",
        "FEED_STATE",
        "HEALTHY",
        "STALE",
        ("BLOCK_NEW_ORDERS", "RESUBSCRIBE_FEED", "REQUIRE_FRESH_SNAPSHOT"),
        5_000_000,
        500_000,
        ("FEED_AGE_WITHIN_LIMIT", "FRESH_SNAPSHOT_VERIFIED", "STABILIZATION_COMPLETE"),
        ("STALE_FEED_DETECTED", "FEED_RESUBSCRIBE_STARTED", "FEED_HEALTHY_RESTORED"),
        blocks_new_orders=True,
    ),
    FaultKind.CLOCK_DRIFT: _behavior(
        "ptp_drift_ppb",
        75_000,
        "PTP_DRIFT_THRESHOLD_EXCEEDED",
        "CLOCK_QUALITY",
        "HEALTHY",
        "UNSAFE",
        ("BLOCK_NEW_ORDERS", "INVALIDATE_TIMING_STATE", "RESYNCHRONIZE_CLOCK"),
        700_000,
        100_000,
        ("OFFSET_WITHIN_LIMIT", "DRIFT_WITHIN_LIMIT", "CLOCK_STABILIZATION_COMPLETE"),
        ("CLOCK_DRIFT_DETECTED", "CLOCK_STATE_UNSAFE", "CLOCK_HEALTHY_RESTORED"),
        blocks_new_orders=True,
    ),
    FaultKind.CLOCK_JUMP: _behavior(
        "wall_clock_step_ns",
        -500_000_000,
        "BACKWARD_CLOCK_JUMP",
        "CLOCK_QUALITY",
        "HEALTHY",
        "UNSAFE",
        ("BLOCK_NEW_ORDERS", "INVALIDATE_WALL_TIME_ORDERING", "RESYNCHRONIZE_CLOCK"),
        8_000,
        2_000,
        (
            "MONOTONIC_SOURCE_VALID",
            "PTP_OFFSET_WITHIN_LIMIT",
            "CLOCK_STABILIZATION_COMPLETE",
        ),
        ("CLOCK_JUMP_DETECTED", "CLOCK_STATE_UNSAFE", "CLOCK_HEALTHY_RESTORED"),
        blocks_new_orders=True,
    ),
    FaultKind.CPU_STARVATION: _behavior(
        "scheduler_budget_ppm",
        50_000,
        "HEARTBEAT_DEADLINE_MISSED",
        "EDGE_HEALTH",
        "HEALTHY",
        "DATA_DEGRADED",
        ("BLOCK_NEW_ORDERS", "SHED_OPTIONAL_WORK", "FENCE_STALE_PROCESS"),
        4_000_000,
        500_000,
        ("HEARTBEAT_CURRENT", "SCHEDULER_BUDGET_RESTORED", "SERVICE_RECONCILED"),
        ("HEARTBEAT_MISSED", "OPTIONAL_WORK_SHED", "EDGE_HEALTH_RESTORED"),
        blocks_new_orders=True,
    ),
    FaultKind.MEMORY_PRESSURE: _behavior(
        "memory_reserve_ppm",
        10_000,
        "MEMORY_RESERVE_THRESHOLD_EXCEEDED",
        "RESOURCE_HEALTH",
        "HEALTHY",
        "DEGRADED",
        ("BLOCK_NEW_ORDERS", "REJECT_NEW_ALLOCATIONS", "SHED_OPTIONAL_WORK"),
        600_000,
        100_000,
        ("MEMORY_RESERVE_RESTORED", "BOUNDED_POOLS_VALIDATED", "SERVICE_STABILIZED"),
        ("MEMORY_PRESSURE_DETECTED", "NEW_WORK_REJECTED", "MEMORY_HEALTH_RESTORED"),
        blocks_new_orders=True,
    ),
    FaultKind.QUEUE_SATURATION: _behavior(
        "queue_occupancy_ppm",
        1_000_000,
        "QUEUE_CAPACITY_EXHAUSTED",
        "EVENT_BUS_HEALTH",
        "HEALTHY",
        "OVERLOADED",
        ("BLOCK_NEW_ORDERS", "DROP_OPTIONAL_EVENTS", "PRESERVE_MANDATORY_AUDIT"),
        40_000,
        10_000,
        ("QUEUE_BELOW_LOW_WATERMARK", "CONSUMER_LAG_CURRENT", "STATE_REBUILT"),
        ("QUEUE_SATURATION_DETECTED", "BACKPRESSURE_APPLIED", "QUEUE_HEALTH_RESTORED"),
        blocks_new_orders=True,
    ),
    FaultKind.GPU_FAILURE: _behavior(
        "gpu_available",
        False,
        "GPU_INFERENCE_FAILURE",
        "MODEL_HEALTH",
        "HEALTHY",
        "DEGRADED",
        (
            "INVALIDATE_GPU_FORECASTS",
            "SWITCH_TO_CPU_FALLBACK",
            "ENFORCE_ORIGINAL_DEADLINE",
        ),
        2_000_000,
        250_000,
        ("GPU_SELF_TEST_PASSED", "MODEL_SIGNATURE_VERIFIED", "SHADOW_WARMUP_COMPLETE"),
        ("GPU_FAILURE_DETECTED", "CPU_FALLBACK_SELECTED", "GPU_RECOVERY_VALIDATED"),
        blocks_new_orders=False,
    ),
    FaultKind.LLM_TIMEOUT: _behavior(
        "llm_response_within_deadline",
        False,
        "DEEP_STAGE_DEADLINE_MISSED",
        "INTELLIGENCE_HEALTH",
        "HEALTHY",
        "DEGRADED",
        ("DISCARD_LATE_RESPONSE", "RETAIN_FAST_ALERT", "MARK_DEEP_STAGE_UNAVAILABLE"),
        5_000_000_000,
        0,
        ("DEEP_STAGE_HEALTHY", "DEADLINE_TEST_PASSED", "NO_PENDING_LATE_RESPONSE"),
        ("LLM_TIMEOUT_DETECTED", "FAST_ALERT_RETAINED", "DEEP_STAGE_RECOVERED"),
        blocks_new_orders=False,
    ),
    FaultKind.MALFORMED_MODEL_RESPONSE: _behavior(
        "model_response_schema_valid",
        False,
        "FORECAST_SCHEMA_VALIDATION_FAILED",
        "MODEL_HEALTH",
        "HEALTHY",
        "DEGRADED",
        (
            "REJECT_FORECAST",
            "ABSTAIN_AFFECTED_ENSEMBLE",
            "INCREMENT_VALIDATION_FAILURE",
        ),
        90_000,
        10_000,
        ("MODEL_CONTRACT_VALID", "PROVENANCE_VALID", "SHADOW_VALIDATION_COMPLETE"),
        ("MODEL_RESPONSE_REJECTED", "ENSEMBLE_ABSTAINED", "MODEL_RECOVERY_VALIDATED"),
        blocks_new_orders=False,
    ),
    FaultKind.NEWS_CONTRADICTION: _behavior(
        "contradictory_fact_count",
        2,
        "CONTRADICTORY_FACT_PROVENANCE",
        "INTELLIGENCE_STATE",
        "NORMAL",
        "CONTRADICTED",
        ("RETAIN_BOTH_ALERTS", "LOWER_CONFIDENCE", "REQUIRE_ADJUDICATION"),
        8_000_000,
        1_000_000,
        (
            "AUTHORITATIVE_SOURCE_IDENTIFIED",
            "CORRECTION_LINKED",
            "UNCERTAINTY_REPUBLISHED",
        ),
        (
            "NEWS_CONTRADICTION_DETECTED",
            "ADJUDICATION_REQUESTED",
            "CONTRADICTION_RESOLVED",
        ),
        blocks_new_orders=False,
    ),
    FaultKind.GATEWAY_DISCONNECT: _behavior(
        "gateway_connected",
        False,
        "SESSION_HEARTBEAT_LOST",
        "GATEWAY_STATE",
        "LOGGED_ON",
        "DISCONNECTED",
        ("BLOCK_NEW_ORDERS", "FENCE_SESSION", "START_ORDER_RECONCILIATION"),
        700_000,
        100_000,
        ("SESSION_REESTABLISHED", "OPEN_ORDERS_RECONCILED", "FENCING_TOKEN_CURRENT"),
        ("GATEWAY_DISCONNECT_DETECTED", "SESSION_FENCED", "GATEWAY_RECONCILED"),
        blocks_new_orders=True,
    ),
    FaultKind.REJECT_BURST: _behavior(
        "venue_reject_rate_ppm",
        800_000,
        "REJECT_RATE_LIMIT_EXCEEDED",
        "GATEWAY_HEALTH",
        "HEALTHY",
        "DEGRADED",
        ("PAUSE_VENUE_ROUTING", "TRIP_VENUE_KILL_SWITCH", "PRESERVE_REJECTS_FOR_AUDIT"),
        3_000_000,
        500_000,
        (
            "REJECT_RATE_WITHIN_LIMIT",
            "VENUE_SESSION_REVALIDATED",
            "OPERATOR_CLEARANCE_RECORDED",
        ),
        (
            "REJECT_BURST_DETECTED",
            "VENUE_KILL_SWITCH_ENGAGED",
            "VENUE_ROUTING_RESTORED",
        ),
        blocks_new_orders=True,
    ),
    FaultKind.DROP_COPY_DISAGREEMENT: _behavior(
        "drop_copy_position_match",
        False,
        "FILL_RECONCILIATION_MISMATCH",
        "PORTFOLIO_HEALTH",
        "RECONCILED",
        "INCONSISTENT",
        (
            "BLOCK_NEW_ORDERS",
            "QUARANTINE_POSITION_SNAPSHOT",
            "START_FILL_RECONCILIATION",
        ),
        1_500_000,
        250_000,
        ("FILLS_MATCH", "POSITIONS_REBUILT", "RISK_SNAPSHOT_REPUBLISHED"),
        ("DROP_COPY_MISMATCH_DETECTED", "POSITION_QUARANTINED", "PORTFOLIO_RECONCILED"),
        blocks_new_orders=True,
    ),
    FaultKind.JOURNAL_TRUNCATION: _behavior(
        "journal_tail_complete",
        False,
        "FRAME_TRUNCATION_DETECTED",
        "JOURNAL_HEALTH",
        "HEALTHY",
        "UNSAFE",
        ("BLOCK_NEW_ORDERS", "PRESERVE_ORIGINAL_JOURNAL", "START_COPY_ONLY_RECOVERY"),
        80_000,
        20_000,
        ("VALID_PREFIX_VERIFIED", "REPAIR_COPY_CREATED", "REPLAY_HASH_VERIFIED"),
        (
            "JOURNAL_TRUNCATION_DETECTED",
            "ORIGINAL_JOURNAL_PRESERVED",
            "RECOVERY_COPY_VERIFIED",
        ),
        blocks_new_orders=True,
    ),
    FaultKind.CONFIGURATION_CORRUPTION: _behavior(
        "active_configuration_integrity_valid",
        False,
        "SIGNATURE_OR_HASH_INVALID",
        "CONFIGURATION_HEALTH",
        "ACTIVE",
        "INVALID",
        (
            "BLOCK_NEW_ORDERS",
            "REJECT_CORRUPTED_CONFIGURATION",
            "REQUIRE_SIGNED_ROLLBACK",
        ),
        60_000,
        10_000,
        ("SIGNATURE_VERIFIED", "VERSION_MONOTONIC", "TWO_PERSON_APPROVAL_VERIFIED"),
        (
            "CONFIGURATION_CORRUPTION_DETECTED",
            "CONFIGURATION_REJECTED",
            "SIGNED_CONFIGURATION_RESTORED",
        ),
        blocks_new_orders=True,
    ),
    FaultKind.SPLIT_BRAIN: _behavior(
        "active_leader_count",
        2,
        "FENCING_TOKEN_CONFLICT",
        "LEADERSHIP_STATE",
        "ACTIVE_LEADER",
        "FENCED",
        ("BLOCK_NEW_ORDERS", "FENCE_BOTH_CANDIDATES", "REQUIRE_HIGHER_WITNESS_TOKEN"),
        8_000,
        2_000,
        ("SINGLE_OWNER_PROVEN", "HIGHER_TOKEN_ISSUED", "FULL_RECONCILIATION_COMPLETE"),
        ("SPLIT_BRAIN_DETECTED", "ORDER_EMITTERS_FENCED", "LEADERSHIP_RECONCILED"),
        blocks_new_orders=True,
    ),
    FaultKind.TRADING_HALT: _behavior(
        "official_trading_status",
        "HALTED",
        "OFFICIAL_HALT_RECEIVED",
        "MARKET_STATE",
        "NORMAL",
        "HALTED",
        ("BLOCK_NEW_ORDERS", "DISABLE_ROUTING", "APPLY_HALT_ORDER_POLICY"),
        15_000,
        5_000,
        ("OFFICIAL_REOPEN_RECEIVED", "RECOVERY_STATE_ENTERED", "FRESH_BOOK_VERIFIED"),
        ("TRADING_HALT_DETECTED", "MARKET_STATE_HALTED", "HALT_RECOVERY_VALIDATED"),
        blocks_new_orders=True,
    ),
    FaultKind.AUCTION_REOPENING: _behavior(
        "official_trading_status",
        "REOPENING_AUCTION",
        "OFFICIAL_REOPENING_AUCTION",
        "MARKET_STATE",
        "HALTED",
        "REOPENING",
        (
            "BLOCK_CONTINUOUS_ORDERS",
            "ENABLE_APPROVED_AUCTION_POLICY_ONLY",
            "REQUIRE_FRESH_BOOK",
        ),
        20_000,
        5_000,
        (
            "AUCTION_COMPLETE",
            "CONTINUOUS_BOOK_VALID",
            "RECOVERY_STABILIZATION_COMPLETE",
        ),
        (
            "AUCTION_REOPENING_DETECTED",
            "MARKET_STATE_REOPENING",
            "CONTINUOUS_TRADING_VALIDATED",
        ),
        blocks_new_orders=True,
    ),
}


def _canonical_bytes(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")


def _sha256(value: object) -> str:
    return hashlib.sha256(_canonical_bytes(value)).hexdigest()


def _require_mapping(value: object, field: str) -> Mapping[str, Any]:
    if not isinstance(value, dict):
        raise ValueError(f"{field} must be an object")
    return value


def _require_string(value: object, field: str) -> str:
    if not isinstance(value, str) or not value or len(value) > MAX_TEXT_BYTES:
        raise ValueError(f"{field} must be a non-empty bounded string")
    return value


def _require_string_tuple(value: object, field: str) -> tuple[str, ...]:
    if not isinstance(value, list) or not value or len(value) > MAX_STRING_ITEMS:
        raise ValueError(f"{field} must be a non-empty bounded array")
    return tuple(_require_string(item, field) for item in value)


def _parse_scenario(value: object) -> Scenario:
    item = _require_mapping(value, "scenario")
    injection_value = _require_mapping(item.get("injection"), "injection")
    scalar = injection_value.get("value")
    if not isinstance(scalar, (bool, int, str)):
        raise ValueError("injection.value must be a scalar")
    duration_ns = injection_value.get("duration_ns")
    latency_ns = item.get("maximum_detection_latency_ns")
    blocks_new_orders = item.get("blocks_new_orders")
    if (
        not isinstance(duration_ns, int)
        or isinstance(duration_ns, bool)
        or duration_ns <= 0
    ):
        raise ValueError("injection.duration_ns must be positive")
    if (
        not isinstance(latency_ns, int)
        or isinstance(latency_ns, bool)
        or latency_ns <= 0
    ):
        raise ValueError("maximum_detection_latency_ns must be positive")
    if not isinstance(blocks_new_orders, bool):
        raise ValueError("blocks_new_orders must be boolean")
    transition_value = _require_mapping(
        item.get("expected_state_transition"), "expected_state_transition"
    )
    try:
        fault = FaultKind(_require_string(item.get("fault"), "fault"))
    except ValueError as error:
        raise ValueError("fault is unsupported") from error
    return Scenario(
        scenario_id=_require_string(item.get("id"), "id"),
        fault=fault,
        component=_require_string(item.get("component"), "component"),
        profiles=_require_string_tuple(item.get("profiles"), "profiles"),
        production_hook=_require_string(item.get("production_hook"), "production_hook"),
        injection=Injection(
            signal=_require_string(injection_value.get("signal"), "injection.signal"),
            value=scalar,
            duration_ns=duration_ns,
        ),
        expected_detection=_require_string(
            item.get("expected_detection"), "expected_detection"
        ),
        expected_transition=Transition(
            domain=_require_string(transition_value.get("domain"), "transition.domain"),
            from_state=_require_string(transition_value.get("from"), "transition.from"),
            to_state=_require_string(transition_value.get("to"), "transition.to"),
        ),
        expected_automated_response=_require_string_tuple(
            item.get("expected_automated_response"), "expected_automated_response"
        ),
        maximum_detection_latency_ns=latency_ns,
        recovery_criteria=_require_string_tuple(
            item.get("recovery_criteria"), "recovery_criteria"
        ),
        required_audit_events=_require_string_tuple(
            item.get("required_audit_events"), "required_audit_events"
        ),
        blocks_new_orders=blocks_new_orders,
    )


def load_catalog(path: Path = DEFAULT_CATALOG) -> ScenarioCatalog:
    """Load and strictly validate a bounded scenario catalog."""
    raw = path.read_bytes()
    if not raw or len(raw) > MAX_CATALOG_BYTES:
        raise ValueError("catalog size is invalid")
    value = _require_mapping(json.loads(raw), "catalog")
    scenario_values = value.get("scenarios")
    combination_values = value.get("combinations")
    if (
        not isinstance(scenario_values, list)
        or not scenario_values
        or len(scenario_values) > MAX_SCENARIOS
        or not isinstance(combination_values, list)
        or len(combination_values) > MAX_SCENARIOS
    ):
        raise ValueError("catalog scenario or combination count is invalid")
    scenarios = tuple(_parse_scenario(item) for item in scenario_values)
    scenario_ids = {scenario.scenario_id for scenario in scenarios}
    faults = {scenario.fault for scenario in scenarios}
    if len(scenario_ids) != len(scenarios) or len(faults) != len(scenarios):
        raise ValueError("scenario IDs and faults must be unique")
    if faults != set(FaultKind):
        raise ValueError("catalog must define every supported fault exactly once")
    combinations: list[Combination] = []
    for raw_combination in combination_values:
        item = _require_mapping(raw_combination, "combination")
        scenario_id_values = _require_string_tuple(
            item.get("scenario_ids"), "combination.scenario_ids"
        )
        if (
            len(scenario_id_values) < MIN_COMBINATION_SIZE
            or not set(scenario_id_values) <= scenario_ids
        ):
            raise ValueError("combination references are invalid")
        combinations.append(
            Combination(
                combination_id=_require_string(item.get("id"), "combination.id"),
                scenario_ids=scenario_id_values,
                expected_safety_state=_require_string(
                    item.get("expected_safety_state"),
                    "combination.expected_safety_state",
                ),
            )
        )
    combination_ids = {item.combination_id for item in combinations}
    if len(combination_ids) != len(combinations):
        raise ValueError("combination IDs must be unique")
    schema_version = value.get("catalog_schema_version")
    if schema_version != 1:
        raise ValueError("catalog schema version is unsupported")
    return ScenarioCatalog(
        catalog_id=_require_string(value.get("catalog_id"), "catalog_id"),
        catalog_schema_version=schema_version,
        scenarios=scenarios,
        combinations=tuple(combinations),
        sha256=hashlib.sha256(raw).hexdigest(),
    )


class SyntheticEdgeHarness:
    """Bounded signal plane; it never invokes a gateway or consumes host resources."""

    def __init__(self) -> None:
        """Create an isolated, initially healthy signal plane."""
        self._signals: dict[str, Scalar] = {}

    def inject(self, injection: Injection) -> None:
        """Mutate one named signal in an isolated simulation state."""
        self._signals[injection.signal] = injection.value

    def active(self, behavior: Behavior) -> bool:
        """Return whether the detector's independent activation condition holds."""
        return self._signals.get(behavior.signal) == behavior.active_value

    def recover(self, behavior: Behavior) -> tuple[str, ...]:
        """Clear one fault and return independently verified recovery evidence."""
        was_active = self.active(behavior)
        self._signals.pop(behavior.signal, None)
        return behavior.recovery if was_active and not self.active(behavior) else ()


def _logical_latency(
    behavior: Behavior, seed: int, scenario_id: str, iteration: int
) -> int:
    material = f"{seed}:{scenario_id}:{iteration}".encode("ascii")
    jitter = int.from_bytes(hashlib.sha256(material).digest()[:8], "big")
    if behavior.detection_jitter_ns > 0:
        jitter %= behavior.detection_jitter_ns + 1
    else:
        jitter = 0
    return behavior.base_detection_latency_ns + jitter


def _run_attempt(scenario: Scenario, seed: int, iteration: int) -> dict[str, object]:
    harness = SyntheticEdgeHarness()
    harness.inject(scenario.injection)
    behavior = _BEHAVIORS[scenario.fault]
    detected = harness.active(behavior)
    latency_ns = _logical_latency(behavior, seed, scenario.scenario_id, iteration)
    detection = behavior.detection if detected else "FAULT_NOT_DETECTED"
    transition = (
        behavior.transition if detected else Transition("NONE", "UNKNOWN", "UNKNOWN")
    )
    responses = behavior.responses if detected else ()
    audit_events = behavior.audit_events if detected else ("CHAOS_FAULT_INJECTED",)
    recovery = harness.recover(behavior) if detected else ()
    checks = {
        "audit_events": audit_events == scenario.required_audit_events,
        "automated_response": responses == scenario.expected_automated_response,
        "blocks_new_orders": behavior.blocks_new_orders == scenario.blocks_new_orders,
        "detection": detection == scenario.expected_detection,
        "detection_latency": latency_ns <= scenario.maximum_detection_latency_ns,
        "recovery": recovery == scenario.recovery_criteria,
        "state_transition": transition == scenario.expected_transition,
    }
    evidence = {
        "audit_events": list(audit_events),
        "automated_response": list(responses),
        "checks": checks,
        "detection": detection,
        "detection_latency_ns": latency_ns,
        "fault": scenario.fault.value,
        "injection": scenario.injection.as_dict(),
        "iteration": iteration,
        "recovery_criteria": list(recovery),
        "seed": seed,
        "state_transition": transition.as_dict(),
    }
    return {
        "audit_events": list(audit_events),
        "automated_response": list(responses),
        "blocks_new_orders": behavior.blocks_new_orders,
        "checks": checks,
        "detection": detection,
        "detection_latency_ns": latency_ns,
        "evidence_sha256": _sha256(evidence),
        "passed": all(checks.values()),
        "recovery_criteria": list(recovery),
        "state_transition": transition.as_dict(),
    }


def _aggregate_scenario(
    scenario: Scenario, seed: int, iterations: int
) -> dict[str, object]:
    attempts = [
        _run_attempt(scenario, seed, iteration) for iteration in range(iterations)
    ]
    failures = sum(not bool(attempt["passed"]) for attempt in attempts)
    return {
        "attempts": iterations,
        "blocks_new_orders": scenario.blocks_new_orders,
        "component": scenario.component,
        "evidence_chain_sha256": _sha256(
            [attempt["evidence_sha256"] for attempt in attempts]
        ),
        "expected": {
            "audit_events": list(scenario.required_audit_events),
            "automated_response": list(scenario.expected_automated_response),
            "detection": scenario.expected_detection,
            "maximum_detection_latency_ns": scenario.maximum_detection_latency_ns,
            "recovery_criteria": list(scenario.recovery_criteria),
            "state_transition": scenario.expected_transition.as_dict(),
        },
        "failures": failures,
        "fault": scenario.fault.value,
        "maximum_observed_detection_latency_ns": max(
            cast("int", attempt["detection_latency_ns"]) for attempt in attempts
        ),
        "observed": {
            "audit_events": attempts[-1]["audit_events"],
            "automated_response": attempts[-1]["automated_response"],
            "detection": attempts[-1]["detection"],
            "recovery_criteria": attempts[-1]["recovery_criteria"],
            "state_transition": attempts[-1]["state_transition"],
        },
        "passed": failures == 0,
        "production_hook": scenario.production_hook,
        "scenario_id": scenario.scenario_id,
    }


def _run_combination(
    combination: Combination,
    scenarios_by_id: Mapping[str, Scenario],
    seed: int,
    iterations: int,
) -> dict[str, object]:
    evidence_hashes: list[str] = []
    maximum_latency = 0
    failures = 0
    for iteration in range(iterations):
        harness = SyntheticEdgeHarness()
        scenarios = [scenarios_by_id[item] for item in combination.scenario_ids]
        for scenario in scenarios:
            harness.inject(scenario.injection)
        detections: list[str] = []
        audit_events = ["CHAOS_COMBINATION_STARTED"]
        attempt_passed = True
        for scenario in scenarios:
            behavior = _BEHAVIORS[scenario.fault]
            detected = harness.active(behavior)
            latency = _logical_latency(
                behavior, seed, combination.combination_id, iteration
            )
            maximum_latency = max(maximum_latency, latency)
            detections.append(behavior.detection if detected else "FAULT_NOT_DETECTED")
            audit_events.extend(behavior.audit_events)
            attempt_passed = (
                attempt_passed
                and detected
                and latency <= scenario.maximum_detection_latency_ns
            )
        recoveries = []
        for scenario in scenarios:
            behavior = _BEHAVIORS[scenario.fault]
            recovery = harness.recover(behavior)
            recoveries.append(list(recovery))
            attempt_passed = attempt_passed and recovery == behavior.recovery
        safety_state = (
            "ORDERS_BLOCKED"
            if any(scenario.blocks_new_orders for scenario in scenarios)
            else "ANALYTICS_DEGRADED"
        )
        audit_events.append("CHAOS_COMBINATION_RECOVERED")
        attempt_passed = (
            attempt_passed and safety_state == combination.expected_safety_state
        )
        failures += not attempt_passed
        evidence_hashes.append(
            _sha256(
                {
                    "audit_events": audit_events,
                    "combination_id": combination.combination_id,
                    "detections": detections,
                    "iteration": iteration,
                    "recoveries": recoveries,
                    "safety_state": safety_state,
                    "seed": seed,
                }
            )
        )
    return {
        "attempts": iterations,
        "combination_id": combination.combination_id,
        "evidence_chain_sha256": _sha256(evidence_hashes),
        "expected_safety_state": combination.expected_safety_state,
        "failures": failures,
        "maximum_observed_detection_latency_ns": maximum_latency,
        "passed": failures == 0,
        "scenario_ids": list(combination.scenario_ids),
    }


def run_catalog(
    catalog: ScenarioCatalog,
    *,
    profile: str,
    seed: int,
    iterations: int,
    selected_scenarios: Sequence[str] = (),
) -> dict[str, object]:
    """Execute one deterministic profile and return a machine-readable report."""
    if profile not in {"fast", "nightly"}:
        raise ValueError("profile must be fast or nightly")
    if seed <= 0:
        raise ValueError("seed must be positive")
    if iterations <= 0 or iterations > MAX_ITERATIONS:
        raise ValueError("iterations are outside the bounded range")
    scenarios_by_id = {scenario.scenario_id: scenario for scenario in catalog.scenarios}
    requested = set(selected_scenarios)
    if requested - scenarios_by_id.keys():
        raise ValueError("selected scenario is not in the catalog")
    selected = [
        scenario
        for scenario in catalog.scenarios
        if profile in scenario.profiles
        and (not requested or scenario.scenario_id in requested)
    ]
    if not selected:
        raise ValueError("profile selects no scenarios")
    scenario_results = [
        _aggregate_scenario(scenario, seed, iterations) for scenario in selected
    ]
    combination_results = (
        [
            _run_combination(item, scenarios_by_id, seed, iterations)
            for item in catalog.combinations
        ]
        if profile == "nightly" and not requested
        else []
    )
    failed = sum(not bool(item["passed"]) for item in scenario_results)
    failed += sum(not bool(item["passed"]) for item in combination_results)
    report: dict[str, object] = {
        "catalog_id": catalog.catalog_id,
        "catalog_sha256": catalog.sha256,
        "combinations": combination_results,
        "iterations_per_scenario": iterations,
        "mode": SIMULATION_MODE,
        "profile": profile,
        "report_schema_version": REPORT_SCHEMA_VERSION,
        "scenarios": scenario_results,
        "seed": seed,
        "summary": {
            "failed": failed,
            "passed": failed == 0,
            "scenario_attempts": len(scenario_results) * iterations,
            "scenario_count": len(scenario_results),
            "simultaneous_combination_attempts": len(combination_results) * iterations,
            "simultaneous_combination_count": len(combination_results),
        },
    }
    report["result_sha256"] = _sha256(report)
    return report


def write_report(path: Path, report: Mapping[str, object]) -> None:
    """Atomically publish one canonical JSON result without following temp files."""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    payload = json.dumps(report, indent=2, sort_keys=True) + "\n"
    try:
        temporary.write_text(payload, encoding="utf-8")
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run deterministic Aegis-MX simulation-only chaos scenarios."
    )
    parser.add_argument("--catalog", type=Path, default=DEFAULT_CATALOG)
    parser.add_argument("--profile", choices=("fast", "nightly"), default="fast")
    parser.add_argument("--iterations", type=int)
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument("--scenario", action="append", default=[])
    parser.add_argument("--output", type=Path)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    """Run the CLI and return zero only when every assertion passes."""
    arguments = _parser().parse_args(argv)
    iterations = arguments.iterations
    if iterations is None:
        iterations = 1 if arguments.profile == "fast" else 1_000
    output = arguments.output
    if output is None:
        output = REPOSITORY_ROOT / "build/reports/chaos" / f"{arguments.profile}.json"
    try:
        catalog = load_catalog(arguments.catalog)
        report = run_catalog(
            catalog,
            profile=arguments.profile,
            seed=arguments.seed,
            iterations=iterations,
            selected_scenarios=arguments.scenario,
        )
        write_report(output, report)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"CHAOS ERROR: {error}")
        return 2
    summary = report["summary"]
    print(json.dumps(summary, sort_keys=True))
    return 0 if isinstance(summary, dict) and summary.get("passed") is True else 1


if __name__ == "__main__":
    raise SystemExit(main())
