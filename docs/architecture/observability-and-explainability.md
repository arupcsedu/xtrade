# Observability and Decision Explainability

## Safety boundary

The C++ observability module has two zones:

```text
critical producer                         asynchronous consumer
-----------------                         ---------------------
fixed component snapshot                  single-writer aggregation
        |                                 Prometheus text rendering
bounded enqueue -- reject newest ------>  structured JSON rendering
        |                                 OTLP encoding and transport adapter
atomic drop count                         journal/analytics publication
```

The producer side is preallocated, trivially copyable, bounded, `noexcept`, and
does not read a clock. It performs no disk or network I/O and renders no text.
Queue saturation returns `false` immediately and increments a drop counter. A
caller may expose or alert on that result but must not retry in a critical loop,
block trading, or interpret observability success as risk authorization.

The consumer side may allocate to produce Prometheus and JSON text. It cannot
grant health, readiness, market validity, model eligibility, risk approval, or
gateway authority.

The binding producer/consumer and cardinality choices are recorded in
[ADR 0034](../adr/0034-bounded-observability-and-decision-explanations.md).

## Metric ownership and units

The adapter layer converts existing immutable component snapshots into one
canonical vocabulary. Cumulative component values use counter `set`, while
individual outcomes use counter `add`. Signed P&L/cost and state values are
gauges.

| Family | Canonical owner/input | Important units |
| --- | --- | --- |
| Market data | `FeedMetrics`, `DataQualityState`, book validity | events, nanoseconds, boolean 0/1 |
| Latency | caller-supplied monotonic duration | nanoseconds and fixed stage |
| Models | `RunResult`, `ModelForecast`, ensemble explanation | ppm, nanoseconds, bounded slot |
| Trading | risk, gateway, portfolio, cost-attribution snapshots | counts, quantity units, currency nanos |
| Infrastructure | clock/journal snapshots and OS/NIC collector sample | ppm, counts, events, nanoseconds |

The full metric names are declared in
[`telemetry.cpp`](../../cpp/observability/src/telemetry.cpp). Required latency
stages cover packet-normalization, decode, book, feature, inference, ensemble,
risk, send, round trip, journal, and end-to-end boundaries. Histogram bounds are
compile-time constants. Prometheus output includes buckets, count, sum, p50,
p95, p99, p99.9, and maximum.

Metrics do not carry raw model, instrument, venue, order, account,
configuration, or correlation identifiers. Models and other bounded fan-outs
use slot numbers 1–16. The deployment configuration must publish the versioned
slot-to-identity inventory separately. This keeps cardinality bounded and makes
an inventory change attributable to a configuration version.

## Logs and correlation

`StructuredLogRecord` carries fixed enum values, `GlobalEventId` correlation,
session and configuration identities, explicit process-monotonic and wall-UTC
times, three numeric fields, and a stable hash. There is deliberately no string
payload. Exact provider documents and externally supplied news text remain in
their protected evidence stores and never enter operational logs.

The originating normalized event ID becomes the correlation ID when available.
A derived workflow without one receives a deterministic `GlobalEventId` from
its owning journal sequence/epoch. The same identity follows feature, forecast,
ensemble, intent, risk, OMS, gateway, fill, and position records. It belongs in
logs, traces, and decision explanations—but never in Prometheus labels.

## Traces

`TracePublisher::publish_off_path` makes the placement rule visible at the call
site. Traces are appropriate for asynchronous model service, control-plane,
registry, replay, and export work. Strict packet/order loops use latency metric
points and journal correlation instead. `OtlpJsonExporter` creates a standards-
shaped OTLP/HTTP JSON request body but performs no transport. The reference
collector batches and exports only outside colocated critical processes.

## Decision explanation lifecycle

The explanation builder accepts an exact ensemble request/output and optional
validated risk and route decisions. It fails closed on invalid hashes or
session, instrument, and configuration mismatches. Its fixed record contains:

- authoritative market and event state plus source snapshot hash;
- every supplied model ID/version, feature snapshot ID, model health,
  eligibility, freshness, prediction distribution, calibration/OOD/data score,
  ensemble weight, and forecast hash;
- combined distribution, effective uncertainty, disagreement, transaction
  cost, uncertainty penalty, safety margin, robust edge, abstention and reason;
- risk decision, failed check/reason, limit snapshot identity/hash, context hash,
  and approved integer price/quantity when present;
- route status/reason, venue, action, time-in-force, integer price/quantity,
  expected value, venue counts, and decision hash when present.

The disposition is one of `ABSTAINED`, `AWAITING_RISK`, `RISK_REJECTED`,
`AWAITING_ROUTE`, or `ROUTED`. A record never makes a missing stage appear to
have occurred. It is stable-hashed and can be emitted as structured JSON off
path. The complete record has a canonical, padding-free little-endian encoding.
The order composition accepts that encoding into the mandatory fail-closed
journal queue before gateway submission; the asynchronous writer then persists
its checksummed segment. Best-effort telemetry remains a derived copy. PAPER
acceptance closes and scans the journal and decodes every explanation before it
reports audit completeness. Whole-system fresh-process reconstruction remains a
production blocker rather than an inferred consequence of this record.

## Deployment views

- [Edge overview dashboard](../../infra/observability/grafana/dashboards/aegis-edge-overview.json)
- [Prometheus alert rules](../../infra/observability/prometheus/alerts.yml)
- [Reference Prometheus configuration](../../infra/observability/prometheus/prometheus.yml)
- [Reference OpenTelemetry collector](../../infra/observability/otel/collector.yaml)
- [Alert runbook](../operations/observability-alert-runbook.md)

These are simulation/paper reference assets. They include no credentials,
remote endpoints, live gateway, or order authority.
