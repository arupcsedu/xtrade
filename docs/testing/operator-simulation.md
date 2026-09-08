# PAPER operator simulation

## Purpose

The drill exercises operational kill and recovery mechanics without OMS,
gateway, network, credential, or live-activation access. It calls the real
deterministic C++ pre-trade risk engine with fixed seed `20260908`.

For each symbol, strategy, venue, and firm scope it:

1. applies an engage command;
2. evaluates a fresh PAPER intent and requires `KILL_SWITCH_ENGAGED`;
3. attempts a clear without operator authorization and requires rejection;
4. applies the same clear sequence with operator authorization; and
5. evaluates a fresh intent and requires a normally approved decision.

The drill then drains all nine decisions from the bounded risk journal in exact
sequence and writes a 21-record SHA-256-linked NDJSON extract. Recovery here
means authorized kill clearing plus a fresh risk evaluation under the unchanged
healthy synthetic fixture. It does not assert that external open orders, fills,
positions, or a venue were reconciled; the operational runbooks still require
that evidence before a real reset.

## Command and outputs

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts \
  make operator-simulation
```

Outputs:

- `build/reports/operations/operator-simulation.json`
- `build/reports/operations/operator-audit.ndjson`
- `build/reports/operations/live-mode-disabled.json`
- `build/reports/operations/live-mode-disabled.md`

The report conforms to the
[operator simulation schema](../../schemas/operator-simulation-report-v1.schema.json),
and each NDJSON line conforms to the
[operator audit-record schema](../../schemas/operator-drill-audit-record-v1.schema.json).
The non-live aggregate conforms to the
[live-mode-disabled schema](../../schemas/live-mode-disabled-evidence-v1.schema.json).

## Automated assertions

- C++ tests run the fixed-seed drill twice and compare decision and audit hashes.
- Python tests recompute every audit link and file digest, require risk journal
  sequences 1–9, and verify the simulator imports neither OMS nor execution.
- Tampering with the audit extract makes the live-disabled aggregate fail.
- The aggregate requires the CMake default and configured option off, build
  metadata false, six non-live edge profiles, systemd's disabled-live guard,
  control-plane LIVE rejection, and visible open production blockers.

The result is evidence of non-live safety and operator-drill behavior only. A
pass deliberately retains `production_ready=false` and
`activation_status=PROHIBITED`.
