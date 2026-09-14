# Bounded ALFRED macro-vintage ingestion

Status: implemented for offline research; remote execution disabled unless all
runtime gates are supplied

## Scope and ownership

The adapter retrieves a seven-series allowlist from the FRED API and uses
ALFRED real-time intervals to construct immutable revision chains. It belongs
to the asynchronous research/data plane. It has no dependency on risk, OMS,
routing, gateways, or live trading.

| Event | Series | Original owner | Native units | Frequency | Seasonal treatment |
| --- | --- | --- | --- | --- | --- |
| CPI | `CPIAUCSL` | U.S. Bureau of Labor Statistics | Index 1982-1984=100 | Monthly | Seasonally Adjusted |
| PPI | `PPIACO` | U.S. Bureau of Labor Statistics | Index 1982=100 | Monthly | Not Seasonally Adjusted |
| Employment | `PAYEMS` | U.S. Bureau of Labor Statistics | Thousands of Persons | Monthly | Seasonally Adjusted |
| GDP | `GDP` | U.S. Bureau of Economic Analysis | Billions of Dollars | Quarterly | Seasonally Adjusted Annual Rate |
| Retail sales | `RSAFS` | U.S. Census Bureau | Millions of Dollars | Monthly | Seasonally Adjusted |
| FOMC target upper | `DFEDTARU` | Federal Reserve Board | Percent | Daily, 7-Day | Not Seasonally Adjusted |
| FOMC target lower | `DFEDTARL` | Federal Reserve Board | Percent | Daily, 7-Day | Not Seasonally Adjusted |

`NAPM`/ISM Manufacturing PMI is explicitly denied. ISM's terms restrict
copying, archival, and derivative time-series use; inclusion requires separate
written permission and a new policy revision.

## Data flow and bounds

```text
reviewed series policy + owner-only approval + external API key + quota
  -> fixed-host HTTPS transport
  -> current metadata and release identity validation
  -> date-only release calendar
  -> output_type=1 observations in raw units
  -> complete immutable revision chains
  -> content-addressed raw/canonical objects and manifests
  -> deterministic as-known-at research queries
```

The client is restricted to four documented endpoints on
`api.stlouisfed.org`, refuses redirects, accepts bounded JSON only, and sends
at most 100 requests per minute. A run has explicit date windows, at most 1,000
requests, 100 pages per endpoint, 10,000 rows per page, 100,000 rows per series,
8 MB per response, three attempts, and a 60-second request timeout. The ALFRED
subtree must remain strictly below 1 GB; the configured maximum is 999,000,000
decimal bytes. Global 800 GB data-root, 20 GB temporary-space, authoritative
quota, and 50 GB reserve gates still apply.

Raw responses, canonical snapshots, reports, ALFRED manifests, and
`tmp/alfred-*` partials all count toward the source cap. Publication uses the
data repository's interprocess writer fence and atomic staged-object protocol.
The original object and its manifest are content addressed and never replaced.

## Point-in-time semantics

The following fields remain distinct:

| Field | Meaning |
| --- | --- |
| `observation_date` | Economic period represented by a value |
| `vintage_date` | ALFRED `realtime_start`, when that value version's real-time period begins, at date precision |
| `known_at_utc_ns` | Conservative query boundary: 00:00 UTC on the day after `vintage_date` |
| `known_through_utc_ns` | Exclusive end derived from ALFRED's closed `realtime_end`; null only for `9999-12-31` |
| `release_date` | Matching FRED release-calendar date, when present |
| `release_time_utc_ns` | Always null because this adapter has no authoritative intraday timestamp |
| `local_receipt_time_utc_ns` | Actual local wall-clock time at HTTP receipt |
| `processing_time_utc_ns` | Actual local wall-clock time at canonicalization |
| metadata `last_updated_utc_ns` | Provider series-metadata update time; not substituted for release or receipt time |

FRED release dates do not necessarily establish when a value became available
through FRED. Therefore a date-only source fact is never promoted to an
intraday timestamp. This conservative rule prevents a same-day backtest from
using a value whose true availability time is unknown. An authoritative
official release-time source can be added later as a separate, versioned fact.

`as_known_at(t)` is inclusive. `latest_available_before(t)` is strict.
Both return the current immutable revision for each observation at the
specified knowledge boundary. Missing values remain explicit and suppress the
earlier value; they are never forward-filled. `revisions_after(t)` returns the
unaltered future revision records for audit, and dataset construction rejects
any record with `known_at_utc_ns` after its cutoff.

## Numeric and transformation contract

Requests force `units=lin`, no frequency aggregation, and `output_type=1`.
Provider decimal strings are parsed without binary floating point and stored as
signed integer microunits with scale 1,000,000. `.` is `MISSING`, not zero.
The canonical snapshot records exact native units, frequency, and seasonal
adjustment; any percentage change, log, normalization, surprise, or resampling
is a separately versioned derived dataset.

At run time, provider title, units, frequency, seasonal adjustment, release ID,
and release name must match the reviewed contract. Unexpected copyright text,
new fields, unknown series, malformed values, pagination drift, overlapping
real-time intervals, and duplicate conflicts fail the whole series closed.

## Contracts and evidence

- [ADR 0057](../adr/0057-bounded-alfred-vintage-and-date-only-availability.md)
- [point-in-time data contract](point-in-time-data-contract.md)
- [source approval](../compliance/forecasting-poc-source-approval.md)
- [series policy](../../infra/data_poc/alfred-series-policy.example.json)
- [operations runbook](../operations/alfred-macro-vintage-ingestion.md)
- [approval schema](../../schemas/alfred-approval-v1.schema.json)
- [snapshot schema](../../schemas/alfred-series-snapshot-v1.schema.json)
- [manifest schema](../../schemas/alfred-artifact-manifest-v1.schema.json)
- [report schema](../../schemas/alfred-run-report-v1.schema.json)

The local, socket-free performance evidence is generated with
`tools/benchmark_alfred.py`. It reports raw monotonic samples, percentile
latencies, record throughput, peak traced memory, fixed seed `20260911`, and
host metadata for index construction, point-in-time queries, and canonical
snapshot serialization plus hashing.

## Known limitations

- No remote run was performed in this phase; a registered FRED API key and an
  unexpired owner-only execution approval remain external requirements.
- Release and vintage availability are date-precision facts. These records are
  unsuitable for intraday macro-surprise trading until an authoritative
  timestamped release feed is independently approved and joined point in time.
- FRED metadata is revalidated on every run, but the reviewed allowlist remains
  the controlling rights and units contract.
- This is infrastructure validation for offline research, not evidence of
  forecasting quality, profitability, or production readiness.
