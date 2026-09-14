# ALFRED macro-vintage ingestion runbook

This runbook operates only the bounded offline research adapter. It cannot
submit orders and does not enable live trading.

## Preconditions

1. Use `/scratch/djy8hg/env/aegis_mx_contracts`.
2. Confirm the data root is `/scratch/djy8hg/aegis_mx_poc_data` or another
   explicitly approved external root. Never place downloaded objects in Git.
3. Obtain fresh authoritative quota evidence. Unknown quota or capacity blocks
   execution.
4. Review
   [the series policy](../../infra/data_poc/alfred-series-policy.example.json).
   The seven allowlisted series are eligible; `NAPM`/ISM PMI is not.
5. Create an approval record conforming to
   [the approval schema](../../schemas/alfred-approval-v1.schema.json), with an
   exact sorted series list, date window, storage cap no greater than
   999,000,000 bytes, expiration, and canonical SHA-256. Store it outside Git
   with owner-only mode `0600`.
6. Supply the registered FRED API key through the approved secret mechanism as
   `AEGIS_FRED_API_KEY`. Do not place it in a command line, file in this
   repository, job output, shell tracing, or a manifest.

## Network-free plan

Planning is the default even when the command name is `ingest`:

```bash
/scratch/djy8hg/env/aegis_mx_contracts/bin/aegis-alfred ingest \
  --vintage-start 2024-01-01 \
  --vintage-end 2024-12-31 \
  --observation-start 2014-01-01 \
  --observation-end 2024-12-31
```

Verify `network_access_performed` is false, the exact seven-series allowlist,
the blocked PMI entry, date windows, deterministic plan IDs, and the source
storage cap.

## Authorized execution

Only after all preconditions are met, add `--execute`, the owner-only approval
path, data root, and current quota evidence:

```bash
/scratch/djy8hg/env/aegis_mx_contracts/bin/aegis-alfred ingest --execute \
  --vintage-start YYYY-MM-DD \
  --vintage-end YYYY-MM-DD \
  --observation-start YYYY-MM-DD \
  --observation-end YYYY-MM-DD \
  --approval /approved/private/path/alfred-approval.json \
  --data-root /scratch/djy8hg/aegis_mx_poc_data \
  --quota-limit-bytes LIMIT \
  --quota-used-bytes USED \
  --quota-source SOURCE \
  --quota-observed-at-utc TIMESTAMP \
  --quota-authoritative
```

The command prints only report paths and the report hash. It must never print
the API key or a credential-bearing URL. Do not enable shell tracing while the
secret environment is present.

## Verification

1. Confirm the machine report validates against
   `schemas/alfred-run-report-v1.schema.json`.
2. Recompute SHA-256 for every stored object and compare it with its immutable
   manifest.
3. Confirm all manifests bind the same approval ID, series-universe hash,
   source revision, plan ID, event range, vintage range, receipt time, and
   processing time.
4. Confirm the storage report remains below 999,000,000 decimal bytes and that
   global data-root limits and the 50 GB reserve remain satisfied.
5. Confirm each requested series has initial records, revisions where supplied,
   and explicit missing values. Never fill a gap.
6. Repeat the run with the same source responses. Snapshot and run hashes must
   match; immutable objects must not be duplicated.
7. Exercise `as_known_at` before accepting a dataset. A future revision in a
   training cutoff is a failed run.

## Failure and recovery

- HTTP 423, 429, and 500 receive at most three total attempts. Honor bounded
  `Retry-After`; persistent throttling fails the run.
- Any other non-200 response fails immediately. No unbounded retry is allowed.
- Metadata, unit, release, copyright, timestamp, or pagination mismatches fail
  closed. Review source changes before updating policy.
- Staged `tmp/alfred-*` objects count toward the source cap. Inspect them and
  the audit trail after interruption; never delete automatically.
- A hash or immutable-path conflict is an incident. Preserve the original,
  stop publication, and follow the data-repository recovery procedure.
- If the API key appears in any output, stop, revoke/rotate it, preserve
  evidence without redistributing the secret, and follow the security incident
  runbook.

## Disablement

Remove the external API key and set `api_execution_authorized` false or allow
the approval to expire. The checked-in example policy is remote-disabled by
default. Disablement does not delete previously retained objects.
