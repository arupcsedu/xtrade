# Portfolio-risk testing

The portfolio suite uses deterministic IDs and timestamps from
`portfolio_test_support.hpp`; it has no random seed or external dependency.
Coverage includes configuration hashing, fail-closed startup, order lifecycle,
pending exposure, exactly-once logical fills, primary/drop-copy reconciliation,
orphan reconstruction, conflicting sources, average cost, realized/unrealized
P&L, corrections, busts, integral splits, session carry, all six stress
scenarios, canonical v1.7 serialization, immutable-reader concurrency, local
pre-trade import, hash-chain integrity, and exact journal recovery.

Run the focused gate from the required environment:

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make format-check
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make lint
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make test
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make schemas-check
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make docs-check
```

Focused C++ and benchmark commands are:

```bash
ctest --test-dir build/dev --output-on-failure -R aegis_risk_tests
build/release/cpp/benchmarks/aegis_benchmark_smoke \
  --benchmark_filter='benchmark_portfolio_(fill_to_snapshot|mark_to_snapshot|snapshot_read|stress_six_scenarios)'
```

UBSan is required for arithmetic and replay. ASan and TSan binaries are built
and run by `make test-sanitizers`; a host unable to reserve sanitizer shadow
memory must report that environmental limitation rather than claim a pass.

