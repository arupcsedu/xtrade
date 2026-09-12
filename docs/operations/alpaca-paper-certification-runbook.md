# Alpaca PAPER connectivity certification runbook

## Preconditions

This runbook is only for the Alpaca paper hostname. Stop if any output names
`api.alpaca.markets`, options, crypto, short selling, extended hours, account
reset, bulk cancellation, a symbol outside `ticker.txt`, quantity above one, or
more than the approved submission count.

The credential file must contain exactly:

```text
AEGIS_ALPACA_PAPER_ENDPOINT=https://paper-api.alpaca.markets/v2
AEGIS_ALPACA_PAPER_KEY_ID=<external secret>
AEGIS_ALPACA_PAPER_SECRET_KEY=<external secret>
```

Keep it outside Git, owned by the current user, and mode `0600`. Never paste it
into a command, report, issue, job environment, or log.

## Prepare the bounded authorization

The following records the authorization already supplied for the 2026-09-11
regular session. It does not access Alpaca:

```bash
export AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts
authorization=/scratch/djy8hg/aegis_mx_poc_data/manifests/alpaca-paper-system-path-authorization-20260911.json
"$AEGIS_PYTHON_ENV/bin/python" tools/alpaca_paper_certification.py \
  prepare-authorization \
  --authorization-id operator-paper-system-path-20260911 \
  --expected-session-date 2026-09-11 \
  --issued-at-utc 2026-09-11T01:18:16Z \
  --expires-at-utc 2026-09-11T20:15:00Z \
  --output "$authorization"
```

Inspect only the non-secret authorization file and its mode before continuing.
Any subsequent tool, policy, or universe edit makes it invalid and blocks the
scheduled operation.

## Read-only preflight

```bash
report_root=/scratch/djy8hg/aegis_mx_poc_data/reports/alpaca-paper-20260911
mkdir -p "$report_root"
"$AEGIS_PYTHON_ENV/bin/python" tools/alpaca_paper_certification.py \
  preflight --symbol NVDA --report "$report_root/preflight.json"
```

`READY` confirms credentials, account state, selected-asset eligibility, and
clock availability. It can be run while the market is closed and performs no
mutation. It does not certify the full system order path.

## Build and inspect the deterministic system path

Use the non-live Release preset and run its local evidence check before any
network mutation:

```bash
source tools/toolchain.sh
cmake --preset release
cmake --build --preset release --target aegis_alpaca_paper_path --parallel
path_binary=build/release/cpp/integration/aegis-alpaca-paper-path
"$path_binary" --symbol NVDA --seed 20260911
```

Stop unless the JSON says `mode=PAPER`, `stage=COMPLETE`,
`live_trading_compiled=false`, `risk_approved=true`,
`oms_command_valid=true`, and `gateway_final_gate_accepted=true`. A source,
universe, or authorization change requires a newly generated authorization.

## Regular-session system-path certification

Run only while Alpaca's clock reports the authorized regular session open:

```bash
"$AEGIS_PYTHON_ENV/bin/python" tools/alpaca_paper_certification.py \
  certify-system-path --symbol NVDA \
  --authorization "$authorization" \
  --report "$report_root/certification.json" \
  --path-binary "$path_binary" \
  --seed 20260911 \
  --execute-paper
```

The command submits one share at USD 0.01, cancels only the returned UUID, and
requires a canceled zero-fill state. It never invokes cancel-all or closes a
position. The Slurm wrapper is
[`tools/slurm/alpaca-paper-certification.sbatch`](../../tools/slurm/alpaca-paper-certification.sbatch).

## Failure handling

- `MARKET_CLOSED`, `WRONG_SESSION`, `ACCOUNT_UNSAFE`, `ASSET_INELIGIBLE`, or
  `QUOTE_UNSAFE`: no order was submitted; correct the condition without
  weakening the gate.
- `ORDER_AMBIGUOUS`: do not rerun. Find the `aegis-paper-YYYYMMDD-*` order in the
  paper portal and cancel only that order if it is open.
- `CANCEL_UNCONFIRMED`: inspect that certification order in the portal. Do not
  cancel unrelated orders.
- `STATE_CHANGED`: compare portal activity and invoke
  [position reconciliation](portfolio-reconciliation-runbook.md). Do not reset
  the account and do not submit an automatic offsetting order.
- `SYSTEM_PATH_FAILED` or `SYSTEM_PATH_INVALID`: no order was submitted. Verify
  the owner-controlled binary, non-live build, hashes, and deterministic C++
  tests. Do not fall back to connectivity-only certification when system-path
  evidence is required.

Preserve the Slurm log, preflight report, certification report, authorization
record, Git commit, executable SHA-256, and session date. Reports contain no
credential or raw account values. A successful outbound report is not evidence
that broker responses traversed the OMS; that report field remains false.
