# Model framework testing

The model framework tests use deterministic fixture IDs and timestamps. No test
uses wall time, network access, credentials, order types, gateways, or live
trading. The repository-wide deterministic seed remains `20260828`; these
specific fixtures are exhaustive and do not sample randomness.

Coverage includes:

- complete forecast validation and rejection of expiry, malformed probability
  sums, absent feature provenance, stale features, NaN, and Inf;
- deadline sampling before and after inference, immediate versioned disabling,
  stale control updates, explicit fallback attribution, and model failure;
- asynchronous non-blocking polling and discard after forecast expiry;
- cache validation, expiration, corruption detection, and model invalidation;
- exact baseline outputs, warm-up behavior, fixed integer regression, and
  seasonal reset;
- isotonic configuration, Platt finite-input checks, and quantile coverage;
- C++/Python equality of `model_forecast_v1_2.amae` plus C++ semantic validation
  of the decoded zero-copy view.

Run focused checks from the repository root:

```bash
source tools/toolchain.sh
build/dev/cpp/models/aegis_model_tests --gtest_color=no
/scratch/djy8hg/env/aegis_mx_contracts/bin/python -m pytest python/tests
```

Run the normal repository gates with the required external environment:

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make format-check
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make lint
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make test
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make schemas-check
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make test-sanitizers
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make benchmark
```

The benchmark smoke binary reports model input validation and local
last-value-runner latency. Numbers are host observations, not acceptance limits;
production capacity and tail-latency qualification require isolated target
hardware, pinned CPUs, NUMA placement, and representative model workloads.
