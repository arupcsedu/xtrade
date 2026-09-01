# Market-state controller testing

Run the required isolated environment and repository gates:

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make format-check
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make lint
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make test
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make test-sanitizers
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make benchmark
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make docs-check
```

The deterministic property-test seed is `20260828`.

Coverage includes:

- all 121 from/to pairs in the documented transition table;
- fixed-seed randomized inputs proving every published transition is permitted;
- explicit prohibition of `HALTED -> NORMAL` and
  `DATA_DEGRADED -> NORMAL`;
- official-halt priority over unsafe clock, invalid feed/book, breaking news,
  volatility, and kill inputs;
- exact unsafe-clock, invalid-data, kill, recovery, event, and predictive-state
  ordering;
- pre-release scheduled activation, active release, overdue scheduled state,
  event dwell, reopening, and continuous recovery stabilization;
- malformed enum/value shape, sequence regression, monotonic regression,
  journal rejection, terminal shutdown, and invalid configuration;
- journal-before-publish hashes and atomic reader consistency; and
- sustained concurrent atomic readers under ThreadSanitizer where the host
  sanitizer runtime is available.

Benchmarks cover healthy unchanged evaluation, journaled event-state transition
plus atomic publication, and atomic snapshot read. Allocation counters must
remain zero in steady state. Reports are diagnostic host baselines, not accepted
production latency objectives.
