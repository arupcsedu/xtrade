# GDELT POC ingestion runbook

This procedure never activates trading and never fetches publisher pages.

## Preconditions

1. Use `/scratch/djy8hg/env/aegis_mx_contracts`.
2. Confirm `ticker.txt`, the Prompt 52 reference snapshot, and Prompt 53 SEC
   coverage artifacts verify.
3. Confirm the owner-only GDELT approval is unexpired and matches the exact
   universe, window, table, 5 GB cap, attribution, and academic/internal scope.
4. Obtain a Google Cloud project with BigQuery API/billing enabled and
   `bigquery.jobs.create` plus read access to the public GDELT dataset.
5. Obtain a short-lived OAuth access token. Do not put it on the command line,
   in a file under Git, or in logs.
6. Refresh authoritative `hdquota -s` evidence. Unknown or stale evidence is
   unsafe.

## Network-free plan

```bash
export AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts
$AEGIS_PYTHON_ENV/bin/aegis-gdelt plan \
  --start-date 2024-09-11 \
  --end-date 2026-09-10
```

The current plan resolves 78 issuers, retains `KRKNF` as unresolved, and emits
125 bounded monthly/alias-batch plans. Re-run after any source artifact changes;
plan IDs must change rather than silently reusing old evidence.

## Authorized execution

```bash
export AEGIS_GCP_PROJECT_ID='<billing-project-id>'
export AEGIS_GCP_ACCESS_TOKEN='<short-lived-oauth-token>'
$AEGIS_PYTHON_ENV/bin/aegis-gdelt ingest \
  --start-date 2024-09-11 \
  --end-date 2026-09-10 \
  --approval /scratch/djy8hg/aegis_mx_poc_data/manifests/approvals/gdelt-academic-approval-v1.json \
  --data-root /scratch/djy8hg/aegis_mx_poc_data \
  --google-project "$AEGIS_GCP_PROJECT_ID" \
  --quota-limit-bytes '<current-limit>' \
  --quota-used-bytes '<current-used>' \
  --quota-source '/opt/rci/bin/hdquota -s' \
  --quota-observed-at-utc '<current-UTC-observation>' \
  --quota-authoritative \
  --execute
unset AEGIS_GCP_ACCESS_TOKEN
```

Execution fails on authentication errors, redirects, malformed responses,
pagination, incomplete jobs, row overflow, processed-byte overflow, storage
admission failure, untrusted metadata, or authorization mismatch. There are no
unbounded retries.

## Verification and recovery

- Run `aegis-data usage` and `aegis-data verify` with the same quota evidence.
- Verify every GDELT object and manifest hash and both report self-hashes.
- Compare ticker coverage, unresolved identities, rejection reasons, precise
  publication-time gaps, duplicate classes, and contradiction counts.
- Re-running identical plans is content-addressed and idempotent. Conflicting
  staged, manifest, or report bytes stop the run. Do not overwrite them.
- If the 5 GB cap is approached, stop and produce `aegis-data cleanup-plan`.
  Never delete automatically.

The access token, Google project access, and BigQuery billing/quota are the only
known external execution dependencies. GDELT dataset storage and derived-use
scope are separately recorded in the approval; publisher content remains out
of scope.
