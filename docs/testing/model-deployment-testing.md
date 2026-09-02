# Model deployment testing

## Focused commands

Use the mandated isolated Python environment even though the deployment slice
itself is standard-library Go:

```bash
export AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts
source tools/toolchain.sh
(cd control && gofmt -l model_registry cmd/model-registry)
(cd control && go vet ./...)
(cd control && go test ./model_registry)
(cd control && go test -race ./model_registry)
(cd control && go test -run '^$' \
  -bench 'BenchmarkDeploymentObservation$' -benchmem -count=1 ./model_registry)
```

Repository `make lint`, `make test`, `make test-sanitizers`, `make benchmark`,
`make dependency-scan`, and `make package` cover the surrounding system. Tests
use seed `20260831`, deterministic in-memory Ed25519 test keys, injected UTC
clocks, synthetic artifacts/observations, temporary directories, and no network,
credential, broker, gateway, or live-trading capability.

## Coverage

The focused suite verifies:

- deterministic config canonicalization, hash/signature tamper detection, exact
  feature compatibility, and all ten required thresholds;
- identical snapshot ID/hash/as-of pairing and rejection of mismatched,
  malformed, stale, late, duplicated, or regressed evidence;
- non-executable checksummed shadow explanations and durable full paired inputs;
- exact integer confidence arithmetic, minimum samples, every required regime,
  and no readiness from aggregate/partial evidence;
- explicit second-person canary approval and retained external authorization;
- a separate automatic rollback test for latency, deadline misses, calibration,
  feature drift, OOD, trade rate, disagreement, implementation shortfall, P&L
  attribution anomaly, and risk-limit pressure;
- symbol, strategy, capital-overflow/excess, order-rate, risk-freshness,
  sequence, and monotonic-time canary scope rejection;
- invalid canary observation and audit failure rollback;
- fail-closed global disable when compatible rollback cannot verify its target;
- append-only audit checksum/chain tamper detection;
- deterministic active-canary restart recovery of observations, statistics,
  sequences, rate state, triggers, counters, and state;
- bounded Prometheus labels and regime/metric series; and
- concurrent access under the Go race detector.

`BenchmarkDeploymentObservation` measures paired validation, audit-record
validation, exact statistics, interval evaluation, and trigger checks using an
in-memory sink. It is a control-plane capacity benchmark, not an inference,
pre-trade-risk, gateway, or end-to-end latency claim. File fsync performance must
be qualified separately on intended control storage.

## Known limits

This slice does not implement organization RBAC, HSM/KMS, multi-host leader
fencing, a network API, an HTTP metrics server, sequential-test correction,
autocorrelation-aware confidence intervals, external dashboard provisioning,
or production experiment approval. The checked-in Grafana JSON and Prometheus
writer are integration artifacts. No synthetic result establishes economic
value or authorizes production/live use.
