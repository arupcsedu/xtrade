# Alpaca PAPER certification testing

## Deterministic gates

The unit and integration suite injects an in-process transport. It proves:

- live and arbitrary origins are rejected;
- unsafe, symlinked, malformed, duplicate-key, and over-permissive credential
  or authorization files fail closed;
- no unit test opens a network connection;
- preflight issues only authenticated `GET` operations;
- submission uses one US-equity share, `day`, `limit`, USD 0.01,
  `extended_hours=false`, and `buy_to_open`;
- one ambiguous `POST` is resolved by client ID and is never submitted again;
- cancellation addresses only the returned UUID;
- closed markets and mismatched universes fail before mutation; and
- a pass requires canceled status, zero fill, and unchanged existing state;
- the C++ path traverses router, deterministic risk, OMS, and the final paper
  gateway gate and emits stable evidence;
- Python independently validates the evidence hash and exact order fields; and
- malformed or altered system-path evidence blocks before `POST`.

Run:

```bash
export AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts
"$AEGIS_PYTHON_ENV/bin/pytest" -q \
  python/tests/test_alpaca_paper_certification.py --no-cov
source tools/toolchain.sh
cmake --preset release
cmake --build --preset release --target aegis_alpaca_paper_path \
  aegis_operator_simulation_tests --parallel
ctest --test-dir build/release --output-on-failure \
  -R aegis_operator_simulation_tests
make lint
make test
make security-test
make docs-check
```

The authenticated preflight and regular-session order/cancel are external
operator tests, not unit tests. Their reports conform to
[`alpaca-paper-certification-report-v1.schema.json`](../../schemas/alpaca-paper-certification-report-v1.schema.json).

## Non-claims

This evidence is not a latency benchmark, realistic fill test, model validation,
profitability result, broker-response/OMS round-trip certification, licensed
historical-data authorization, or live-trading qualification.
