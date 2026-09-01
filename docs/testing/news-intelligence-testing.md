# News intelligence testing

The deterministic test seed remains `20260828`. Synthetic fixtures contain no
licensed text, production endpoints, or credentials.

Run the Python and cross-language contract suite with:

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make test
```

Coverage includes source authentication, exact-byte hashing, active-content
removal, strict UTF-8, direct/nested/Base64 prompt injection, malformed replay
envelopes, registry-only ticker resolution, corporate expansion, all sixteen
event types, language/document detection, evidence-backed facts, deterministic
novelty, duplicate articles, revisions/corrections, contradictory sources,
delayed official confirmation, bounded publication/deep queues, deep provenance
and evidence rejection, deadline misses, lifecycle state, configuration hash,
metrics, audit-log redaction, graceful shutdown, and immutable fast/deep lineage.

Schema checks prove v1 through v1.4 additive conformance and byte-for-byte
Python/C++ validation of `event_intelligence_v1_4.amae`. Fuzzing continues to
exercise the shared contract envelope parser. The pipeline is asynchronous
Python, so it has no hot-path latency acceptance target; benchmarks measure
deterministic fast-stage and replay throughput only and make no economic-value
claim.

Run `make benchmark` to record the bounded sanitization/classification and
fast-stage publication p50, p95, p99, and throughput observations in
`build/reports/benchmarks/news-intelligence.json`. Host-dependent timings are
diagnostic observations, not acceptance thresholds.
