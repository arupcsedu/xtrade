# Chaos testing

## Fast profile

Use the required isolated Python environment:

~~~bash
export AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts
make chaos-fast
~~~

The fast profile runs all 23 single-fault scenarios once with seed 20260828
and writes build/reports/chaos/fast.json. It is part of make fast and the
pull-request workflow.

Run one scenario while developing:

~~~bash
/scratch/djy8hg/env/aegis_mx_contracts/bin/python tools/chaos_runner.py \
  --profile fast \
  --scenario split-brain \
  --iterations 10 \
  --seed 20260828 \
  --output build/reports/chaos/split-brain.json
~~~

## Nightly profile

~~~bash
AEGIS_CHAOS_NIGHTLY_ITERATIONS=1000 make chaos-nightly
~~~

Nightly repeats every individual scenario and five simultaneous combinations.
The scheduled exhaustive workflow retains
build/reports/chaos/nightly.json. The make full command also runs the nightly
profile; the iteration count may be lowered for local diagnosis but must not be
lowered silently in CI.

## Pass criteria

A scenario passes only when every attempt exactly matches:

- expected detection code;
- component-local source and destination state;
- ordered automated-response codes;
- order-blocking posture;
- maximum logical detection latency;
- ordered recovery criteria; and
- ordered required audit events.

A report passes only when every selected scenario and combination passes.
Unknown scenarios, unsupported profiles, zero or excessive iterations,
duplicate catalog entries, missing faults, malformed JSON, and invalid
combination references fail before injection. The report and each attempt chain
are SHA-256 bound.

The JSON contract is
[chaos-result-report-v1.schema.json](../../schemas/chaos-result-report-v1.schema.json).
The test suite checks catalog completeness, deterministic hashes, independent
expectation/detector mismatch, nightly combinations, bounds, corruption, atomic
report publication, and CLI exit status:

~~~bash
/scratch/djy8hg/env/aegis_mx_contracts/bin/pytest \
  -q python/tests/test_chaos_runner.py --no-cov
~~~

## Scope of evidence

Logical latency proves configured detector scheduling against deterministic
event time. It is not a host-performance benchmark. Resource-pressure scenarios
exercise quota and failure behavior without consuming unbounded resources.
Component-native suites, sanitizers, sustained concurrency tests, and isolated
staging drills provide the corresponding implementation evidence.
