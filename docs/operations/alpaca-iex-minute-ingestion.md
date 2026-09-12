# Alpaca IEX minute-ingestion runbook

## Safety boundary

This runbook retrieves historical bars only. It has no order command and does
not enable live trading. Use the required environment
`/scratch/djy8hg/env/aegis_mx_contracts` and data root
`/scratch/djy8hg/aegis_mx_poc_data`.

Credentials remain in `/scratch/djy8hg/ALPACA/.keys`, owned by the operator and
mode `0600`. The accepted endpoint forms are the exact paper origin with an
optional `/v2` suffix. Never pass key values as command arguments.

## Preflight

```bash
export AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts
export PYTHONPATH=/scratch/djy8hg/xtrade/python/research:/scratch/djy8hg/xtrade
cd /scratch/djy8hg/xtrade

$AEGIS_PYTHON_ENV/bin/python -m aegis_mx_research.alpaca_historical \
  --data-root /scratch/djy8hg/aegis_mx_poc_data pilot
```

Without `--execute`, the command is a no-network dry run. Before execution,
confirm the private approval and policy have not expired, `ticker.txt` has not
changed, `/opt/rci/bin/hdquota -s` remains authoritative, and no run lease is
active.

## Pilot

Run the `pilot` command with `--policy`, `--approval`, `--secrets-file`, and
`--execute`. The command selects the latest five complete market sessions. It
must report every symbol, preserve missing pairs, and publish both `report.json`
and `report.md` beneath `reports/alpaca-iex-minute/pilot/`.

Verify and accept only after the run completes:

```bash
$AEGIS_PYTHON_ENV/bin/python -m aegis_mx_research.alpaca_historical \
  --data-root /scratch/djy8hg/aegis_mx_poc_data verify \
  --report /scratch/djy8hg/aegis_mx_poc_data/reports/alpaca-iex-minute/pilot/report.json \
  --accept-pilot
```

## Backfill

Submit from the repository root. The script launches one downloader despite
the parallel partition's two-node allocation:

```bash
mkdir -p /scratch/djy8hg/aegis_mx_poc_data/reports/alpaca-iex-minute/backfill
sbatch \
  --output=/scratch/djy8hg/aegis_mx_poc_data/reports/alpaca-iex-minute/backfill/slurm-%j.out \
  --error=/scratch/djy8hg/aegis_mx_poc_data/reports/alpaca-iex-minute/backfill/slurm-%j.err \
  tools/slurm/alpaca-iex-minute-backfill.sbatch
```

Monitor with `squeue` and `sacct`. Do not submit a second epoch to improve
speed. On interruption, resubmit the same plan: verified checkpoints make it
idempotent. Never remove the checkpoint, lease, original raw object, or report
to force progress.

The CLI converts `SIGINT` and `SIGTERM` into a structured `INTERRUPTED` result,
unwinds the writer lease, removes only its incomplete atomic-write temporary
file, and retains the last durable checkpoint. Scheduler `SIGKILL` still relies
on OS fence release and may leave an accounted partial file; inspect it and use
`aegis-data cleanup-plan` rather than deleting it automatically.

The 2026-09-11 run demonstrates this recovery boundary. An operator stopped an
initial job after discovering its one-hour walltime request was too short. The
older process was interrupted during `fsync`, released the OS writer fence, and
left one accounted 71,308-byte temporary. The two-hour replacement job resumed
the same run ID and completed in 1:38:04 with no provider retries. Because the
old checkpoint did not distinguish an initial task from a durable final page,
one page was fetched again and 8,782 identical bars were removed during
canonicalization. The implementation now records `download_complete`; its
regression test proves a final-page resume makes no network refetch. Preserve
the original scheduler logs and temporary until an operator explicitly reviews
cleanup. Final-attempt duration and request counters do not include the first
attempt; source-object counts and both scheduler records are the cross-attempt
evidence.

## Verification and incidents

Run `verify` against the backfill report twice with seed `20260911`, using
different `--verification-output` paths; both deterministic reinspection hashes
must match. Use new output paths so both content-hashed evidence files remain
available. Also run `aegis-data usage` and `aegis-data verify` for the full
repository.

```bash
$AEGIS_PYTHON_ENV/bin/python -m aegis_mx_research.alpaca_historical \
  --data-root /scratch/djy8hg/aegis_mx_poc_data verify \
  --report /scratch/djy8hg/aegis_mx_poc_data/reports/alpaca-iex-minute/backfill/report.json \
  --sample-seed 20260911 \
  --verification-output /scratch/djy8hg/aegis_mx_poc_data/reports/alpaca-iex-minute/backfill/verification-1.json
```

Stop new acquisition if authorization expires, terms/account scope changes,
credentials fail, a response is redirected or malformed, the provider changes
an object, reserve/quota is unsafe, or verification fails. Preserve the raw
object and audit record, quarantine affected new material, revoke the private
policy, and follow the data-source authorization runbook. Do not silently
rewrite or delete the original.
