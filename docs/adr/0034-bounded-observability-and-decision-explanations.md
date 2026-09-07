# ADR 0034: Bounded observability and decision explanations

- Status: Accepted
- Date: 2026-09-02
- Owners: edge-core, observability, risk, execution, operations, compliance

## Context

Metrics, logs, and traces are required for operating and auditing Aegis-MX, but
ordinary exporter libraries allocate, lock, perform DNS/network I/O, and can
introduce unbounded label cardinality. None of those behaviors is permitted in
the market-data, decision, risk, OMS, or gateway hot path. An exporter outage
must not delay an order safety check. At the same time, a final decision must be
explainable using exact model/version/provenance, ensemble, risk, and routing
facts rather than reconstructed from unrelated time-series samples.

## Decision

1. Hot and near-hot producers copy only trivially copyable, fixed-layout points
   or audit records into bounded preallocated MPSC queues. Enqueue has a bounded
   retry count, rejects the newest item on saturation, never waits, and exposes
   a monotonic drop counter. Trading behavior never depends on telemetry
   acceptance.
2. A single off-path consumer owns aggregation. Prometheus exposition, JSON
   rendering, OTLP encoding, filesystem output, and network transport are
   prohibited on producer threads.
3. The metric vocabulary, units, latency stages, histogram bounds, and metric
   slots are compiled enums. The only label emitted by the C++ exporter is a
   bounded `slot` in `[1,16]`; instrument, model, order, account, correlation,
   configuration, and free-text values are forbidden as metric labels. A
   versioned deployment inventory resolves slots outside the time series.
4. Latency uses thirteen fixed stages and nanosecond buckets. Producers supply
   already-measured monotonic durations. The observability library never reads
   a clock. p50, p95, p99, p99.9, and maximum are deterministic fixed-bucket
   summaries; raw histogram buckets remain available for PromQL.
5. Structured logs use fixed component, severity, and event-code enums plus
   three fixed numeric values. They contain a `GlobalEventId` correlation ID,
   session ID, configuration version, explicit monotonic and wall-UTC times,
   and a stable hash. They cannot contain arbitrary document text, provider
   payloads, secrets, or licensed data.
6. OpenTelemetry span publication is explicitly off-path. The C++ layer emits a
   fixed span record and builds an OTLP/HTTP JSON body only on the consumer. A
   separate collector owns sampling, batching, retry, and network export.
7. `DecisionExplanationRecord` is a versioned, stable-hashed, bounded record. It
   binds correlation and explanation IDs to market/event state, every supplied
   model identity/version/feature snapshot/output/eligibility/weight, combined
   uncertainty and costs, abstention, deterministic risk result, and selected
   venue/routing result. A disposition makes partially completed, rejected, and
   routed decisions distinguishable without inventing missing stages.
8. Decision explanations use their dedicated bounded queue because loss and
   sizing differ from operational logs. The consumer must journal the exact
   validated record before sending optional analytics copies. Saturation is an
   operational fault and alert, never permission to bypass the journal or risk.

## Consequences

- Producer cost and memory are bounded and independent of Prometheus, Grafana,
  or an OpenTelemetry backend.
- Metrics deliberately sacrifice arbitrary dimensional drill-down. Exact
  per-decision investigation uses the correlated audit record and journal.
- Exported quantiles are bucket bounds, not interpolated estimates. Capacity
  decisions should use histogram buckets and host-specific benchmark evidence.
- A telemetry queue outage may reduce operational visibility but cannot make a
  component ready or authorize an order. Mandatory order/risk audit persistence
  remains governed by the journal's independent fail-closed policy.
- OTLP network transport, durable log shipping, production slot inventory, and
  environment-specific threshold tuning remain deployment integrations.

## Rejected alternatives

- Direct Prometheus client updates and synchronous structured logging were
  rejected because hidden locks, allocation, and output backpressure enter the
  hot path.
- IDs as Prometheus labels were rejected because their unbounded cardinality
  can exhaust monitoring memory precisely during an incident.
- Probabilistic producer-side trace sampling was rejected because it adds
  nondeterministic state to critical loops and still leaves exporter calls easy
  to misuse.
- Reconstructing explanations by joining metrics was rejected because scrape
  timing and aggregation cannot prove exact model, configuration, or risk
  provenance.
