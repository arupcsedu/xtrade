# SEC EDGAR ingestion testing

Prompt 53 uses a socket-free deterministic mock SEC server. Unit and integration
tests never contact SEC. They cover fixed-host policy, identifying User-Agent,
rate control, 429/5xx retries, shutdown, malformed and compressed responses,
decompression bombs, JSON depth/size limits, ticker ambiguity, selected forms,
historical-shard issuer/date validation, overlap-only retrieval, cross-partition
duplicate accessions, amendments, explicit lineage, exact decimal facts,
timestamp disorder, active-content removal, prompt injection, parser deadline,
storage admission, immutable conflicts, races, and report generation.
Concurrent issuer retrieval is also tested against a serialized publication
batch so two fetch workers cannot collide at the global single-writer admission
fence. The sustained regression publishes 32 distinct objects through four
workers while proving that the root admission fence is acquired exactly once.
The microbenchmark reports both per-object admission and pre-admitted batched
publication latency.

Run:

```bash
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make format
AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts make lint
PYTHONPATH=python/intelligence:python/research \
  /scratch/djy8hg/env/aegis_mx_contracts/bin/pytest python/tests/test_sec_edgar.py
/scratch/djy8hg/env/aegis_mx_contracts/bin/python tools/benchmark_sec_edgar.py
```

The final validation evidence and live public-source coverage counts are recorded
after execution. Live retrieval is an explicit operational check, not a unit
test and not evidence of predictive value.
