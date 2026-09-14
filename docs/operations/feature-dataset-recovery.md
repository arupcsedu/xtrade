# Feature dataset build and recovery runbook

## Preconditions

1. Use `/scratch/djy8hg/env/aegis_mx_contracts`.
2. Verify the authoritative ticker snapshot and accepted Prompt 56 manifests.
3. Verify the reference and corporate-action evidence bound by the canonical
   promotion. For this POC only, ADR 0060 permits the explicitly degraded
   now-known current-universe cohort; incomplete historical mapping and halt
   history must remain quality reasons and prohibit economic-value claims.
4. Run `aegis-data usage` and require authoritative quota evidence, at least
   50 GB filesystem reserve, projected root usage below 100 GB, and temporary
   usage below 20 GB.
5. Confirm the build configuration is offline/PAPER-only.

Plan without writing:

```bash
source /scratch/djy8hg/env/aegis_mx_contracts/bin/activate
aegis-real-features
```

After every precondition passes, publish from the accepted promotion:

```bash
aegis-real-features --execute
```

This command performs no network access and has no order-entry capability.

## Acceptance check

A build is accepted only when
`datasets/feature-poc/manifests/<build-key>.json` exists, its
`manifest_sha256` and `dataset_id` verify, every listed object hash verifies,
and `leakage.status` is `PASS`. Parquet objects without that final manifest are
orphans, not a dataset.

## Interrupted build

1. Preserve logs and identify the deterministic build key.
2. Do not relabel or edit any published object.
3. Run `aegis-data verify` and `aegis-data cleanup-plan`; do not delete
   automatically.
4. Re-run with the same immutable inputs. Identical content-addressed objects
   are reused; conflicting bytes fail closed.
5. Validate the new manifest, coverage report, and leakage result before use.

## Rejection response

- `NORMALIZATION_LEAKAGE`, `LABEL_FEATURE_OVERLAP`,
  `FUTURE_EVENT_REVISION`, or `PURGE_EMBARGO_VIOLATION`: quarantine the build
  evidence and investigate; never override the rejection.
- Input hash/schema failure: quarantine the source partition and rerun Prompt
  56 verification.
- Missing target bars: retain explicit per-horizon coverage; do not fill.
- Storage admission failure: produce a cleanup plan or reduce the authorized
  scope. Never automatically remove source data.
- Reference semantics outside ADR 0060: keep the real build blocked. Under the
  bounded current-cohort POC, preserve `NOW_KNOWN_CURRENT_UNIVERSE_MAPPING` and
  `HALT_HISTORY_INCOMPLETE` as degraded quality rather than claiming complete
  point-in-time reference history. Synthetic fixture success never clears a
  real input defect.
