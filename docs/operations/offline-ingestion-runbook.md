# Offline ingestion runbook

This runbook operates synthetic or pre-positioned filesystem fixtures only. It
does not authorize a remote source and does not provide a provider-specific
adapter. Follow the [source authorization runbook](data-source-authorization-runbook.md)
before any future network implementation.

## Preconditions

1. Use `/scratch/djy8hg/env/aegis_mx_contracts`.
2. Confirm `ticker.txt` and retain its reported universe hash.
3. Use an absolute external data root, normally
   `/scratch/djy8hg/aegis_mx_poc_data`.
4. Obtain current authoritative user-quota evidence. Shared filesystem free
   space is not entitlement evidence.
5. Run `aegis-data usage` and retain the audit JSON.
6. Never put a password, token, API key, signed URL, or authorization header in
   arguments, filenames, sidecars, or issue reports.

The examples use decimal bytes and deliberately omit `--execute` first.

## Plan a synthetic fixture

```bash
/scratch/djy8hg/env/aegis_mx_contracts/bin/aegis-data \
  --data-root /scratch/djy8hg/aegis_mx_poc_data plan-fetch \
  --provider synthetic \
  --dataset synthetic-minute-bars \
  --start 2026-09-08 --end 2026-09-08 --ticker AAPL \
  --request-id operator-reviewed-synthetic-plan \
  --quota-limit-bytes 10995116277760 \
  --quota-used-bytes CURRENT_DECIMAL_BYTES \
  --quota-source AUTHORITATIVE_SOURCE \
  --quota-observed-at-utc CURRENT_UTC_TIMESTAMP \
  --quota-authoritative
```

Review the exact objects, total estimate, temporary peak, admission reasons,
universe hash, policies, and `dry_run=true`. An admitted plan is capacity
evidence, not permission to fetch a remote dataset.

## Execute an approved local fixture

Use the same filters and quota evidence with `fetch --execute`:

```bash
/scratch/djy8hg/env/aegis_mx_contracts/bin/aegis-data \
  --data-root /scratch/djy8hg/aegis_mx_poc_data fetch \
  --provider synthetic \
  --dataset synthetic-minute-bars \
  --start 2026-09-08 --end 2026-09-08 --ticker AAPL \
  --request-id operator-reviewed-synthetic-plan --execute \
  --quota-limit-bytes 10995116277760 \
  --quota-used-bytes CURRENT_DECIMAL_BYTES \
  --quota-source AUTHORITATIVE_SOURCE \
  --quota-observed-at-utc CURRENT_UTC_TIMESTAMP \
  --quota-authoritative
```

The filesystem provider additionally requires `--filesystem-root` containing
regular data files and closed `*.source.json` sidecars. Symlinks, credentialed
locators, changed files, sidecar escape paths, and metadata mismatches reject.

After success, retain the CLI audit, then run:

```bash
/scratch/djy8hg/env/aegis_mx_contracts/bin/aegis-data \
  --data-root /scratch/djy8hg/aegis_mx_poc_data verify
```

## Resume after a controlled interruption

Do not edit a `.partial` object or checkpoint. Inspect the checkpoint and
source-version evidence, re-establish quota and entitlement evidence, then use
the original filters and request ID:

```bash
/scratch/djy8hg/env/aegis_mx_contracts/bin/aegis-data \
  --data-root /scratch/djy8hg/aegis_mx_poc_data resume-fetch \
  --provider synthetic \
  --dataset synthetic-minute-bars \
  --start 2026-09-08 --end 2026-09-08 --ticker AAPL \
  --request-id operator-reviewed-synthetic-plan --execute \
  --quota-limit-bytes 10995116277760 \
  --quota-used-bytes CURRENT_DECIMAL_BYTES \
  --quota-source AUTHORITATIVE_SOURCE \
  --quota-observed-at-utc CURRENT_UTC_TIMESTAMP \
  --quota-authoritative
```

A mismatch returns a stable error and preserves evidence. Do not delete a
checkpoint to force progress. Follow the
[data recovery runbook](poc-data-recovery.md) for preservation and reviewed
replacement.

## Inspect provenance

```bash
/scratch/djy8hg/env/aegis_mx_contracts/bin/aegis-data \
  --data-root /scratch/djy8hg/aegis_mx_poc_data inspect-source \
  --manifest-id source-EXACT_64_HEX_DIGEST
```

The command authenticates the manifest before printing its payload. It never
prints raw object content.

## Stop, quarantine, and escalation

- Request cooperative shutdown and wait for current bounded calls. New batches
  stop and checkpoints remain durable.
- Treat any `CHECKPOINT_CORRUPT`, `OBJECT_CHANGED`, `HASH_MISMATCH`,
  `MANIFEST_CORRUPT`, `UNAUTHORIZED`, or quota error as a stop condition.
- Quarantine is evidence, not usable source data. It cannot feed normalization,
  training, or inference.
- Never repair the original bytes in place and never delete automatically to
  regain space.
- A suspected credential disclosure requires secret revocation and the
  [security incident runbook](../security/incident-response.md).
