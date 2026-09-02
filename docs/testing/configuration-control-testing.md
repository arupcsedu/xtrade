# Configuration control testing

The implementation uses only the Go standard library. Tests require no secret,
network, database, exchange, broker, or live-trading capability. Deterministic
cryptographic fixtures derive an Ed25519 test key from seed
`aegis-mx-control-20260902`; that key is synthetic and must never be deployed.

Run focused validation:

```bash
source tools/toolchain.sh
(cd control && go test -count=1 ./config_service)
(cd control && go test -race -count=1 ./config_service)
(cd control && go vet ./config_service ./cmd/config-service)
(cd control && go test -run '^$' -bench BenchmarkEdgeEvaluate \
  -benchmem -count=3 ./config_service)
```

The test suite covers:

- canonical hashing, signature tamper rejection, strict JSON, and live-mode
  rejection;
- bootstrap and active RBAC, unauthenticated/expired/wrong-epoch/wrong-role
  requests, replay IDs, and same-person approval attempts;
- staged activation, parent/revision compare-and-swap, restart recovery,
  two-person rollback, emergency kill authorization, and kill replay;
- absent, valid, stale, clock-regressed, and killed edge state, exact model and
  session eligibility, persistent local kills, and invalid signatures;
- audit corruption preventing startup; and
- concurrent duplicate request submission under the Go race detector.

`BenchmarkEdgeEvaluate` is the Go adapter/cache baseline, not the C++ pre-trade
risk latency claim. It reports time and allocations for an already verified
immutable snapshot. Signature verification, disk sync, and JSON parsing occur
only during control updates and must not be inferred from that measurement.

Full repository gates remain:

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make format
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make lint
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make test
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make benchmark
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make docs-check
```

Architecture and limitations are documented in the
[configuration control-plane design](../architecture/configuration-control-plane.md)
and [ADR 0033](../adr/0033-signed-two-person-configuration-control.md).
