# Gateway testing

The deterministic seed is `20260828`; the paper-model and synthetic certification
seed is `20260831`. No test uses a network endpoint, credential, provider payload,
or proprietary protocol field.

Coverage includes:

- default-off compile capability and simulation/paper startup;
- explicit session, logon/logoff, sequence, heartbeat, recovery, and shutdown;
- independent negative final predicates for risk, mode, scope, clock, market,
  feed, book, halt, kill, fencing, journal, session, and rate capacity;
- duplicate and conflicting command identity;
- acknowledgement latency, deterministic rejects, partial fills, queue ahead,
  cancel races, replace responses, fees, slippage, impact, halts, and auctions;
- acknowledgement/reject/execution decoding into validated OMS inputs;
- recovery hashes binding all mutable order fields, canonical slot rebuilding,
  and explicit ambiguous recovery snapshots;
- cross-scope normalized-response rejection, non-valid market-data suppression,
  and configurable auction-participation suppression;
- OMS-to-gateway-to-OMS certification round trip;
- repeated fixed-seed synthetic-exchange runs with identical audit hashes;
- arbitrary normalized response and sequence fuzzing; and
- allocation and latency smoke benchmarks.

Run with the required isolated environment:

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make format
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make lint
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make test
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make test-fuzz
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make test-sanitizers
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make benchmark
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make docs-check
```

Focused execution uses `ctest --test-dir build/dev -R
aegis_execution_tests --output-on-failure`. Benchmarks cover final submission,
acknowledgement polling, and fixed-window throttling. Results are infrastructure
baselines, not venue certification, capacity approval, or economic claims.
