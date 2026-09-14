# Real minute promotion runbook

This runbook converts the already downloaded Alpaca IEX source corpus to the
accepted Prompt 56 Parquet contract. It never submits an order.

Use the required environment and owner-only data root:

```bash
export PYTHONPATH=python/research:python/contracts
PY=/scratch/djy8hg/env/aegis_mx_contracts/bin/python
ROOT=/scratch/djy8hg/aegis_mx_poc_data
```

Dry-run the two mutating stages first:

```bash
$PY -m aegis_mx_research.real_minute_pipeline --data-root "$ROOT" \
  fetch-actions --secrets-file /scratch/djy8hg/ALPACA/.keys
$PY -m aegis_mx_research.real_minute_pipeline --data-root "$ROOT" promote
```

After a narrow child authorization exists, execute the bounded metadata read:

```bash
$PY -m aegis_mx_research.real_minute_pipeline --data-root "$ROOT" \
  fetch-actions --execute --secrets-file /scratch/djy8hg/ALPACA/.keys
```

Promote and verify:

```bash
$PY -m aegis_mx_research.real_minute_pipeline --data-root "$ROOT" promote --execute
$PY -m aegis_mx_research.real_minute_pipeline --data-root "$ROOT" verify
```

The spool is restartable by source-manifest identity. `SIGINT` and `SIGTERM`
stop at a source checkpoint. Do not remove the spool or an orphaned staged
object automatically; inspect it against the recovery runbook and immutable
manifests first.

Acceptance requires matching source/input/published counts, verified object
hashes, Zstandard Parquet, explicit regular-session rejects, and retained
degraded-reference reason codes. A successful run remains offline research
data and does not make the system production-ready.
