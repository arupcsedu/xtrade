# Bounded POC data storage operations

This runbook operates only on a configured local forecasting POC data root. It
does not authorize a data source, perform a download, delete a file, or enable
trading. The architecture and failure semantics are defined in the
[data-repository design](../architecture/poc-data-repository.md).

## Prerequisites

- Activate or address `/scratch/djy8hg/env/aegis_mx_contracts` directly.
- Use an absolute data root outside `/scratch/djy8hg/xtrade`.
- Obtain authoritative quota evidence from the allocation authority. Shared
  filesystem free space alone is not quota evidence.
- Record quota values in decimal bytes and an explicit UTC observation time.
- Do not put credentials, signed URLs, or tokens in CLI arguments or reports.

The default root is `/scratch/djy8hg/aegis_mx_poc_data`; pass `--data-root` to
use an approved alternative. The first command initializes the empty fixed
layout and a policy marker. No data is downloaded.

## Inspect usage

```bash
/scratch/djy8hg/env/aegis_mx_contracts/bin/aegis-data \
  --data-root /scratch/djy8hg/aegis_mx_poc_data usage
```

Review `total.bytes_decimal`, `partial.bytes_decimal`,
`temporary.bytes_decimal`, and `filesystem.free.bytes_decimal`. Binary GiB is
informational only. Any integrity error blocks the operation.

## Estimate an operation

Substitute current, authoritative quota evidence:

```bash
/scratch/djy8hg/env/aegis_mx_contracts/bin/aegis-data \
  --data-root /scratch/djy8hg/aegis_mx_poc_data estimate \
  --operation-id approved-pilot-plan \
  --output-bytes 1000000000 \
  --temporary-bytes 200000000 \
  --retry-overhead-bytes 100000000 \
  --quota-limit-bytes 250000000000 \
  --quota-used-bytes 10000000000 \
  --quota-source allocation-authority \
  --quota-observed-at-utc 2026-09-09T16:00:00Z \
  --quota-authoritative
```

Exit status `0` means the estimate is admitted under the observed state; it is
not download authorization. Status `2` means a computed policy rejection.
Malformed or unavailable state returns status `1`. Omitting any quota field
produces `QUOTA_UNKNOWN` and fails closed.

## Verify integrity

```bash
/scratch/djy8hg/env/aegis_mx_contracts/bin/aegis-data \
  --data-root /scratch/djy8hg/aegis_mx_poc_data verify
```

Status `0` means all discovered manifests and referenced objects passed. Status
`1` means at least one error. Verification is read-only; follow the
[recovery runbook](poc-data-recovery.md) and retain original evidence.

## Plan cleanup

```bash
/scratch/djy8hg/env/aegis_mx_contracts/bin/aegis-data \
  --data-root /scratch/djy8hg/aegis_mx_poc_data cleanup-plan \
  --target-bytes 80000000000
```

The deterministic plan prioritizes temporary, partial, quarantine, and then
immutable retention-review candidates. Every candidate requires operator
approval and has `automatic=false`. The command does not delete or move it.

## Routine checks

Before each future ingestion or dataset build:

1. Confirm the root marker and policy hash are accepted.
2. Run `usage` and retain the JSON output.
3. Refresh authoritative quota evidence.
4. Estimate the complete peak, including partial files, temporary conversion,
   output duplication, retry overhead, and publication manifests.
5. Stop on any reason code; do not split an operation to evade limits.
6. After an approved external operation, run `verify` and retain its audit.

Never manually remove the policy marker or admission lock to bypass a failure.
Never delete an immutable object simply because it appears in a cleanup plan.
