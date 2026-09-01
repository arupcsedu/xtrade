"""Deterministic smoke benchmark for the near-real-time intelligence pipeline."""

from __future__ import annotations

import argparse
import json
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Final

from aegis_mx_intelligence import (
    AuthenticationEvidence,
    BoundedInMemoryPublisher,
    ChannelId,
    ConfigurationVersion,
    DeterministicDeepAdjudicator,
    EntityDefinition,
    EntityRegistry,
    Identifier128,
    IngestStatus,
    InstrumentId,
    IntelligencePipeline,
    IntelligencePipelineConfig,
    MockProvider,
    ModelId,
    ModelVersion,
    RuleEngine,
    SessionId,
    SourceAuthentication,
    SourceDocument,
    sanitize_document,
    sha256_bytes,
)

if TYPE_CHECKING:
    from collections.abc import Callable

SEED: Final = 20_260_828
WARMUP_ITERATIONS: Final = 8
MAX_ITERATIONS: Final = 100_000


@dataclass(frozen=True, slots=True)
class TimingSummary:
    """Integer latency distribution for one bounded operation."""

    name: str
    iterations: int
    p50_ns: int
    p95_ns: int
    p99_ns: int
    throughput_per_second: int


class StepClock:
    """Deterministic clock used to avoid ambient-clock reads in the pipeline."""

    def __init__(self) -> None:
        """Start from a fixed nonzero monotonic timestamp."""
        self._value = 1_000_000

    def __call__(self) -> int:
        """Return the current timestamp and advance by one nanosecond."""
        value = self._value
        self._value += 1
        return value


def _identifier(seed: int) -> Identifier128:
    return Identifier128(seed, seed + 1)


def _authentication() -> AuthenticationEvidence:
    return AuthenticationEvidence(
        SourceAuthentication.MOCK_VERIFIED,
        "benchmark-mock-authentication",
        sha256_bytes("benchmark-authentication"),
    )


def _source(index: int) -> SourceDocument:
    event_time = 1_800_000_000_000_000_000 + index
    return SourceDocument(
        provider_id="benchmark-mock",
        document_id=f"document-{index}",
        source_uri=f"https://benchmark.example.invalid/document-{index}",
        content_type="text/plain",
        payload=(
            f"ACME Corporation said the company reported earnings revenue "
            f"${index + 1} million."
        ).encode(),
        provider_event_time_utc_ns=event_time,
        received_wall_clock_utc_ns=event_time + 1_000,
        authentication=_authentication(),
    )


def _registry() -> EntityRegistry:
    return EntityRegistry(
        (
            EntityDefinition(
                "ACME Corporation",
                ("ACME Corporation",),
                ("ACME",),
                InstrumentId(_identifier(10)),
            ),
        )
    )


def _percentile(ordered: list[int], percentile: int) -> int:
    index = ((len(ordered) - 1) * percentile) // 100
    return ordered[index]


def _measure(
    name: str,
    iterations: int,
    operation: Callable[[], object],
) -> TimingSummary:
    for _ in range(WARMUP_ITERATIONS):
        operation()
    durations = []
    started = time.perf_counter_ns()
    for _ in range(iterations):
        sample_started = time.perf_counter_ns()
        operation()
        durations.append(time.perf_counter_ns() - sample_started)
    elapsed = max(1, time.perf_counter_ns() - started)
    ordered = sorted(durations)
    return TimingSummary(
        name=name,
        iterations=iterations,
        p50_ns=_percentile(ordered, 50),
        p95_ns=_percentile(ordered, 95),
        p99_ns=_percentile(ordered, 99),
        throughput_per_second=(iterations * 1_000_000_000) // elapsed,
    )


def _fast_pipeline_operation(iterations: int) -> Callable[[], object]:
    capacity = iterations + WARMUP_ITERATIONS
    publisher = BoundedInMemoryPublisher(capacity)
    config = IntelligencePipelineConfig(
        session_id=SessionId(_identifier(20)),
        source_channel_id=ChannelId(_identifier(30)),
        configuration_version=ConfigurationVersion(_identifier(40)),
        fast_model_id=ModelId(_identifier(50)),
        fast_model_version=ModelVersion(_identifier(60)),
        deep_model_id=ModelId(_identifier(70)),
        deep_model_version=ModelVersion(_identifier(80)),
        publication_capacity=capacity,
        deduplication_capacity=capacity,
        deep_queue_capacity=capacity,
    )
    pipeline = IntelligencePipeline(
        config,
        MockProvider((), capacity=capacity),
        _registry(),
        publisher,
        DeterministicDeepAdjudicator(),
        StepClock(),
    )
    sources = tuple(_source(index) for index in range(capacity))
    next_index = 0

    def ingest_next() -> object:
        nonlocal next_index
        status = pipeline.ingest(sources[next_index])
        next_index += 1
        if status is not IngestStatus.PUBLISHED:
            msg = f"benchmark fast pipeline failed closed with {status.name}"
            raise RuntimeError(msg)
        return status

    return ingest_next


def run(iterations: int) -> dict[str, object]:
    """Run bounded deterministic sanitization and publication categories."""
    if not 0 < iterations <= MAX_ITERATIONS:
        msg = f"iterations must be within [1, {MAX_ITERATIONS}]"
        raise ValueError(msg)

    source = _source(0)
    rules = RuleEngine()

    def sanitize_classify() -> object:
        document = sanitize_document(source)
        return rules.classify(document, 1_000_000)

    summaries = (
        _measure("sanitize_and_classify", iterations, sanitize_classify),
        _measure(
            "fast_pipeline_publish",
            iterations,
            _fast_pipeline_operation(iterations),
        ),
    )
    return {
        "benchmark_scope": "infrastructure_only",
        "economic_value_claim": False,
        "seed": SEED,
        "summaries": [asdict(summary) for summary in summaries],
    }


def main() -> int:
    """Write stable-shape JSON; host timings are observations, not thresholds."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--iterations", type=int, default=100)
    parser.add_argument("--output", required=True, type=Path)
    arguments = parser.parse_args()
    result = run(arguments.iterations)
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":  # pragma: no cover - exercised as a benchmark command.
    raise SystemExit(main())
