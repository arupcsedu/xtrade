# Chaos testing runbook

## Preconditions

Run only from a disposable development, CI, paper, or simulation environment.
Confirm the selected process has no venue endpoint, broker credential, licensed
feed credential, live build capability, mounted production journal, production
configuration signing key, or route to an order gateway. The repository runner
itself is simulation-only, but operators must still verify the surrounding
environment before adding future native adapters.

Use seed 20260828 unless an investigation records another positive seed.
Preserve the scenario catalog hash, source revision, command, result JSON, and
test logs together.

## Run and observe

~~~bash
export AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts
make chaos-fast
make chaos-nightly
~~~

The command must print a JSON summary whose passed field is true, produce a
report whose mode is SIMULATION, and exit zero. Verify result_sha256 after
removing only that field and serializing the remaining object with sorted
compact JSON keys. Do not edit a failed report.

For every failed scenario, compare expected and observed, then inspect the named
production_hook and component-native test. A missing block action on a
safety-critical fault is a release blocker. A late or missing audit event is
also a failure even when the final state is safe.

## Failure handling

1. Stop the scenario run; do not retry until it passes without preserving the
   original report.
2. Preserve the report, catalog, source revision, seed, stdout/stderr, and
   affected component logs.
3. Keep order admission disabled for a failed safety-critical detector.
4. Determine whether the failure is injection, detection, state publication,
   automated response, latency, recovery, or audit evidence.
5. Add a deterministic regression test before changing the implementation.
6. Re-run the failed scenario, the complete fast profile, and any combination
   containing that scenario.

For journal corruption, never repair the original. For split brain, fence all
candidate emitters and require a higher witness token plus full reconciliation.
For halt/reopening failures, official status remains authoritative. For
malformed or late model output, discard it; do not extend its deadline.

## Recovery exit

Recovery is complete only when every catalog criterion is independently
observed, all required audit events exist, the evidence chain verifies, and the
full profile passes with the recorded seed. Do not treat process health, a
single fresh packet, gateway reconnect, or an operator assertion alone as
recovery.

## Prohibited use

Do not point chaos tooling at production, add real endpoints, run destructive
host-pressure commands, modify host time, corrupt an original journal, or use a
fault to bypass risk. A production-like staging adapter requires a separate ADR,
least-privilege identity, target allowlist, two-person approval, bounded blast
radius, abort mechanism, and evidence-preservation plan.
