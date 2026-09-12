# SEC EDGAR ingestion runbook

## Preconditions

1. Use `/scratch/djy8hg/env/aegis_mx_contracts`.
2. Verify the private approval is mode `0600`, self-hashed, and unexpired.
3. Set a descriptive contact-bearing User-Agent. It is identification metadata,
   not a credential.
4. Confirm `/opt/rci/bin/hdquota -s`, the 50 GB reserve, global 100 GB cap, and
   SEC 8 GB cap.
5. Confirm the authoritative `ticker.txt` hash and that the intent is internal
   academic research.

```bash
export AEGIS_SEC_USER_AGENT='Aegis-MX academic-research/0.1 contact@example.edu'
SEC_APPROVAL=/scratch/djy8hg/aegis_mx_poc_data/manifests/approvals/sec-edgar-academic-approval-v1.json
/scratch/djy8hg/env/aegis_mx_contracts/bin/aegis-sec-edgar coverage \
  --approval "$SEC_APPROVAL"
```

The command above is a dry run. Network access requires `--execute`:

```bash
/scratch/djy8hg/env/aegis_mx_contracts/bin/aegis-sec-edgar coverage \
  --approval "$SEC_APPROVAL" \
  --data-root /scratch/djy8hg/aegis_mx_poc_data \
  --filing-start-date 2024-09-11 \
  --filing-end-date 2026-09-10 \
  --quota-limit-bytes 10995116277760 \
  --quota-used-bytes CURRENT_EXACT_BYTES \
  --quota-source '/opt/rci/bin/hdquota -s' \
  --quota-observed-at-utc CURRENT_UTC_TIMESTAMP \
  --quota-authoritative \
  --execute
```

The exact quota fields are mandatory for execution. Filesystem-wide free space
is still checked independently for the 50 GB reserve, but it is not accepted as
proof of the user's allocation or current quota use.
The filing dates are also mandatory. Shards outside that closed interval are
not requested, and the interval is recorded in report schema v1.1.

Never place API keys, cookies, EDGAR filer tokens, or signed URLs in the
User-Agent. This read API needs none.

## Failure and recovery

- `UNAUTHORIZED`: verify content class, approval hash/permissions, and expiry.
- `RATE_LIMITED` or `RETRY_EXHAUSTED`: stop; do not increase rate or retry
  bounds. Resume later using immutable existing objects.
- `RESPONSE_TOO_LARGE`, `DECOMPRESSION_LIMIT`, or `PARSER_LIMIT`: retain the
  failure reason; do not weaken limits. Review that object separately.
- `TIMESTAMP_DISORDER`: quarantine the observation and inspect source/local
  clock evidence.
- `STORAGE_LIMIT`: run `aegis-data usage` and `cleanup-plan`; never delete
  evidence automatically.
- prompt injection: treat retained text as evidence only. The analysis view
  excludes it and neither view can reach order entry.

Every retry is bounded. Re-execution is content-addressed and does not replace
an existing object or manifest.
