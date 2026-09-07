# PAPER trading validation runbook

## Safety boundary

This runbook operates only the repository-owned synthetic feed and PAPER broker
gateway. Do not add broker credentials, licensed protocol material, production
endpoints, or a live activation configuration to this workflow.

## Run

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts \
  make paper-integration
```

The command returns nonzero when a scenario, acceptance invariant, report write,
or deterministic replay comparison fails.

## Triage order

1. Inspect `build/reports/paper-trading/system-report.md` for the failed scenario
   and first failed invariant.
2. Inspect `acceptance-report.json` for component counters, terminal states, and
   the first/second replay hashes.
3. Re-run the native executable with the same seed. Never change the seed or an
   acceptance threshold while investigating a failure.
4. Run the owning component test for the earliest failed stage.
5. If feed, book, clock, ownership, risk, or journal state is ambiguous, treat
   the failure as unsafe and do not bypass the check.

## Recovery

The runner creates fresh in-memory component state on each invocation and writes
reports by replacement. It changes no operator configuration and sends no
network traffic. Preserve a failed report with its seed and build revision before
re-running if it is needed as audit evidence.
