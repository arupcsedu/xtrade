# Point-in-time reference-data operations

## Safe defaults

The command reads an existing, checksummed backfill report and writes owner-only
artifacts. It does not contact Alpaca, Nasdaq, SEC, or any other remote source.
It never modifies the minute dataset and cannot enable trading.

Use the required environment:

```bash
source /scratch/djy8hg/env/aegis_mx_contracts/bin/activate
```

Build the reference snapshot and complete-universe report:

```bash
aegis-reference build \
  --source-report /scratch/djy8hg/aegis_mx_poc_data/reports/alpaca-iex-minute/backfill/report.json \
  --output-directory /scratch/djy8hg/aegis_mx_poc_data/reports/reference-data/prompt-52
```

Verify either artifact:

```bash
aegis-reference verify \
  /scratch/djy8hg/aegis_mx_poc_data/reports/reference-data/prompt-52/reference-snapshot-v1.json
```

Run the bounded benchmark:

```bash
aegis-reference benchmark \
  --source-report /scratch/djy8hg/aegis_mx_poc_data/reports/alpaca-iex-minute/backfill/report.json \
  --iterations 100 \
  --output /scratch/djy8hg/aegis_mx_poc_data/reports/reference-data/prompt-52/reference-benchmark-v1.json
```

## Failure and recovery

Hash mismatch, changed `ticker.txt`, incomplete asset coverage, malformed
calendar data, unknown version, and noncanonical output reject before usable
publication. Publication writes a mode-0600 temporary file, synchronizes it,
atomically renames it, and synchronizes the directory. A failed write removes
only its newly created temporary file. Existing source evidence is never
rewritten or deleted.

The generated report must remain `PARTIAL_REFERENCE_COVERAGE` while historical
mappings, corporate actions, and halts are absent. Operators must not override
`safe_for_historical_training=false` merely to unblock a downstream phase.
