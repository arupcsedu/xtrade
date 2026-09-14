# Forecasting POC source approval

## Current decision

| Field | Value |
| --- | --- |
| Decision ID | `AEGIS-POC-SOURCE-2026-09-11-03` |
| Accountable operator | `djy8hg` |
| Use classification | Academic, non-commercial |
| Distribution | Owner-only internal use; no raw-data redistribution |
| Approved dataset | Alpaca Market Data API v2 Historical Stock Bars, Basic IEX, `1Min`, `adjustment=raw` |
| Universe | Exact `/scratch/djy8hg/xtrade/ticker.txt` snapshot |
| Date bound | Five-session pilot, then at most two years ending on the latest complete session |
| Application state | **CONDITIONALLY AUTHORIZED** |
| Live trading capability | False |
| Reassessment deadline | `2026-10-11T23:59:59Z` |

The application restriction for `alpaca_iex_historical_bars` changed from
deny-all to a narrow, expiring academic authorization after the operator
instructed the project to rely on Alpaca's personal/non-commercial terms for
this owner-only POC. This is an application policy decision, not a legal
opinion or a representation that Alpaca issued a bespoke ML license.

The immutable approval and active policy are private administrative artifacts,
not repository files:

- approval record:
  `/scratch/djy8hg/aegis_mx_poc_data/manifests/approvals/alpaca-iex-academic-approval-v1.json`
- approval document SHA-256:
  `3ae12181b645a401d7374fe96bc2b41410491878ddd327fb3fd52d55a4a381a8`
- source policy:
  `/scratch/djy8hg/aegis_mx_poc_data/manifests/approvals/alpaca-iex-academic-policy-v1.json`
- source policy file SHA-256:
  `74bbd21d307e7ffff33ab4d51b9979e9a078195bc07222d3efc746980ae6f7bb`

Both files are owner-readable only. Neither contains credentials. The
checked-in [example policy](../../infra/data_poc/source-policy.example.json)
remains deny-by-default.

## Binding restrictions

- Only `GET` requests to exact Alpaca paper-account and market-data hosts are
  allowed; redirects and other hosts are rejected.
- Only IEX one-minute OHLCV bars are acquired. Quotes, trades, SIP, options,
  crypto, extended hours, and daily bars are outside the approval.
- Raw and canonical data remain under the owner-only external data root.
- Raw market data must not be published, redistributed, sold, or used
  commercially.
- Missing history is recorded, never synthetically filled.
- The 80 GB target, 100 GB hard root limit, 20 GB temporary limit, and 50 GB
  free reserve remain mandatory.
- A changed account class, plan, provider terms, purpose, user set, data root,
  universe, or dataset immediately suspends new downloads pending review.
- New downloads stop at approval expiry. Retained-data treatment is reviewed
  then; the application never deletes evidence automatically.
- Model training is limited to this internal academic POC and cannot imply
  economic value, production readiness, or permission for commercial use.

### Corporate-action reference extension

On 2026-09-12 the operator explicitly authorized a GET-only standard Alpaca
corporate-action query for the same internal academic, owner-only POC. The
extension is content-bound to the existing approval and policy, expires no
later than its parent, and permits only
`GET https://data.alpaca.markets/v1/corporate-actions`. It exists solely to
invalidate unsafe historical labels. It does not authorize redistribution or
trading. The private authorization record is stored under the data root and is
not committed because it contains operator identity metadata.

## Pilot evidence

The five-session pilot ran for `2026-09-03` through `2026-09-10` and completed
with 99 provider requests, zero retries, 109,420 canonical records, and
109,845 raw records. All 79 requested symbols were reported. Seventy-eight
resolved as supported US exchange-listed equities; `KRKNF` was retained as an
explicit unsupported OTC symbol, producing the only five missing
symbol/session pairs. Regular-session canonicalization rejected 425
out-of-session source bars and published 390 partitions without synthetic
filling.

Verification checked 803 manifests and 803 objects, found no sampled duplicate
rows, and produced deterministic verification SHA-256
`c250dbe98835655c4c79bebf6bf10e4ea504f34483fccc90cb33b0f39a047dc3`.
The private machine report and human report are under
`/scratch/djy8hg/aegis_mx_poc_data/reports/alpaca-iex-minute/pilot/`.

## Backfill evidence

The bounded backfill completed for `2024-09-11` through `2026-09-10`: 501
regular sessions, 79 requested symbols, and 1,010 bounded tasks. It retained
1,438 source objects and published 9,462,709 canonical records in 36,520
symbol/session partitions. Source payloads occupy 947,534,490 bytes and
canonical payloads occupy 6,662,713,009 bytes. The complete POC root uses
7,782,178,266 logical bytes (7.247718 GiB), below both the 80 GB target and
100 GB hard limit.

The report records 3,059 explicit missing symbol/session pairs. These include
all 501 sessions for unsupported OTC symbol `KRKNF`, pre-listing or sparse
coverage for newer symbols, and all 79 symbols on `2025-03-10`, when this
source response contained no in-scope bars. No missing observation was filled.
Canonicalization rejected 55,249 out-of-session bars. An interrupted first
attempt caused 8,782 identical bars from one durable final page to be seen
again on resume; deterministic deduplication removed them, and the recovery
contract now records an explicit `download_complete` flag with a regression
test preventing that refetch in future runs.

Two post-run verifications using seed `20260911` are byte-identical. Each
checked 38,783 manifests and 38,783 objects, hashed 7,724,928,068 bytes,
sampled 64 deterministic partitions, found zero duplicate canonical rows, and
produced reinspection SHA-256
`b6b0191c400221d765e972dd1782f774f177dfaaf4a9d2ba60ab0f76f456e973`.
The immutable run report SHA-256 is
`b166b13e5d372670c87370c15a6b00e258c31a9acc2d57248b276065b3022730`.
Evidence is under
`/scratch/djy8hg/aegis_mx_poc_data/reports/alpaca-iex-minute/backfill/`.

## SEC EDGAR decision

Prompt 53 created the separate, expiring approval
`AEGIS-SEC-POC-2026-09-11-01` for owner-only academic use. It permits the
official current company/ticker association, issuer-specific submissions JSON,
issuer-specific Company Facts JSON, and only the primary documents of forms
8-K, 10-Q, 10-K, 6-K, and 20-F. The private record is:

`/scratch/djy8hg/aegis_mx_poc_data/manifests/approvals/sec-edgar-academic-approval-v1.json`

Its SHA-256 is
`08ad9db282eb3d0725521ba3c63b6018f11b26011f533a42445f0fca79523e96`.
The approval expires on `2026-10-11T23:59:59Z`. It prohibits a complete EDGAR
mirror, arbitrary exhibits or attachments, linked external content, raw filing
document training, redistribution, and every connection from filing content to
order entry. Structured metadata and XBRL may support this internal research
POC. Filing text may produce bounded, sanitized evidence excerpts but is not a
model-training corpus.

The runtime requires an identifying `AEGIS_SEC_USER_AGENT`, stays below the
SEC's published fair-access maximum, and fails closed after approval expiry.
The checked-in example source policy remains disabled by default.

## GDELT decision

Prompt 54 approves only GDELT-owned GKG metadata from the partitioned BigQuery
table for the bounded `2024-09-11` through `2026-09-10` academic POC window.
The decision follows GDELT's published unrestricted dataset-use terms and
mandatory attribution; it does not grant or infer rights to linked publisher
article bodies. Stored data is owner-only, capped at 5,000,000,000 decimal
bytes, and cannot connect to risk, OMS, routing, or gateways.

The private, owner-only approval record is:

`/scratch/djy8hg/aegis_mx_poc_data/manifests/approvals/gdelt-academic-approval-v1.json`

It contains no credentials. The approval's canonical self-hash is
`1d9cbd8aca4debfd3af5abbbdb69a07ec4f8024d502947a66eb7fd8ef7962e24` and
the complete file SHA-256 is
`a46c9e4161b40e4f3d298ea414bf0d4008c5cdbb160840c9a0b4e60a9a977e5a`.
The checked-in example source policy remains disabled by default. Runtime
access is independently blocked until a Google
Cloud billing project, BigQuery access, and a short-lived OAuth credential are
provided outside Git.

See the [architecture](../architecture/gdelt-poc-ingestion.md),
[ADR](../adr/0056-bounded-gdelt-gkg-query-and-advisory-events.md), and
[runbook](../operations/gdelt-poc-ingestion.md).

## FRED/ALFRED selected federal series

Application approval is limited to `CPIAUCSL`, `PPIACO`, `PAYEMS`, `GDP`,
`RSAFS`, `DFEDTARU`, and `DFEDTARL`. Their original owners are BLS, BEA, the
U.S. Census Bureau, and the Federal Reserve Board, whose cited policies permit
reuse of the selected public-domain federal data. Persistent storage, internal
academic research/model training, and internal derived artifacts are approved
for this finite allowlist. Attribution and immutable source provenance are
required. Redistribution is outside this approval.

`NAPM`/ISM PMI is explicitly denied pending separate written permission. No
other FRED-hosted series inherits approval by association with FRED.

The adapter, schemas, tests, and documentation may be implemented. Network
retrieval remains disabled by default and requires all of the following at run
time:

- a registered FRED API key supplied externally;
- a `0600`, non-symlink approval record conforming to
  `schemas/alfred-approval-v1.schema.json`;
- exact sorted series and date-window authorization;
- an unexpired approval and a cap no greater than 999,000,000 bytes;
- authoritative current quota evidence and the global repository admission
  gates;
- explicit `--execute`.

The source provides release and vintage dates, not authoritative intraday
availability timestamps. Canonical records therefore retain null intraday
release time and become queryable only at the conservative next-day UTC
boundary. They must not drive same-day intraday macro surprise decisions.

See the [architecture](../architecture/alfred-macro-vintage-ingestion.md),
[ADR](../adr/0057-bounded-alfred-vintage-and-date-only-availability.md), and
[runbook](../operations/alfred-macro-vintage-ingestion.md).

## Other sources

Nasdaq Trader and Massive/Polygon retain their independent dispositions in the
[source assessment](forecasting-poc-source-assessment.md).
Paper-account order authorization remains separate from every data-source
authorization.

Follow the
[authorization runbook](../operations/data-source-authorization-runbook.md)
and [Alpaca ingestion runbook](../operations/alpaca-iex-minute-ingestion.md).
