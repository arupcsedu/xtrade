# Bounded GDELT metadata ingestion

| Field | Value |
| --- | --- |
| Schema | `gdelt-metadata-record` `1.0.0` |
| Window | At most two years; planned POC window `2024-09-11` through `2026-09-10` |
| Universe | Exact `ticker.txt` snapshot |
| Source | GDELT 2.0 GKG partitioned BigQuery table |
| Storage cap | 5,000,000,000 decimal bytes |
| Execution class | Asynchronous/offline Python |
| Trading capability | None; advisory records only |
| Decision | [ADR 0056](../adr/0056-bounded-gdelt-gkg-query-and-advisory-events.md) |

## Boundaries

`aegis_mx_research.gdelt` joins the immutable Prompt 52 instrument snapshot to
current SEC issuer names from Prompt 53. It retains 78 resolved issuer names and
the explicit unresolved ticker `KRKNF`; neither count is hardcoded. The join
checks both universe hashes and both source-document self-hashes.

The adapter reads only the following GKG columns:

- `GKGRECORDID`;
- `DATE`, labeled as GDELT provider observation time;
- `SourceCollectionIdentifier` and `SourceCommonName`;
- `DocumentIdentifier`, retained as an inert normalized source URL with user
  information, query parameters, and fragments removed;
- `V2Themes`, `V2Organizations`, and source-language metadata.

It does not request article bodies, quotes, publisher HTML, images, or linked
resources. The URL is evidence, never an instruction to fetch. The fixed query
selects web-source metadata, prunes `_PARTITIONTIME`, and tests exact
server-normalized organization names against named array parameters. No alias
is interpolated into SQL.

## Entity resolution

Names use bounded Unicode NFKC normalization, case folding, punctuation and
whitespace normalization, and a deterministic legal-suffix variant. A suffix
variant equal to the ticker is removed, which prevents false matches for names
such as `IREN Ltd`. Shared aliases are excluded from the query allowlist and
produce an `AMBIGUOUS` record if encountered. Unknown and too-short names are
`REJECTED`; bare ticker strings do not resolve.

Resolved records bind one or more stable `InstrumentId` values. The evidence is
`SEC_CURRENT_ASSOCIATION_ONLY`; it must not be used as proof that the name was
historically valid on the GDELT observation date.

## Temporal and integrity contract

| Field | Meaning |
| --- | --- |
| `provider_observation_time_utc_ns` | UTC value parsed from GKG `DATE` |
| `publication_time_utc_ns` | Null in v1; not inferred |
| `receipt_time_utc_ns` | Time Aegis received the BigQuery response |
| `processing_time_utc_ns` | Time the row was locally validated |

Receipt may not precede provider observation and processing may not precede
receipt. GDELT v1 never invents provider delivery latency. Every retained record
has a content-derived SHA-256 excluding the provider ID, allowing both provider
ID deduplication and independent content-key deduplication. Different content
for the same provider ID is an invalid conflict. Different content at the same
URL is retained with contradiction lineage.

Each monthly/alias-batch partition is canonical JSON Lines plus an immutable,
self-hashed manifest. The source-specific 5 GB counter includes its committed
objects, manifests, reports, and every `tmp/gdelt-*` partial so an interrupted
publication cannot evade the cap. The run report combines ticker coverage,
rejections,
storage accounting, data quality, attribution, and network provenance. Run IDs
bind accepted content hashes, ordered query-plan IDs, rejections, and universe.

## Fast advisory classifier

The infrastructure classifier is deterministic and theme-only. Its ordered
rules cover correction, rumor, halt, bankruptcy, cybersecurity, M&A, recall,
regulatory, executive, guidance, earnings, financing, litigation, and supply
chain labels. It makes no economic claim, does not read article text, and cannot
create an order, intent, forecast, or risk decision.

## Source evidence

- [GDELT terms of use](https://gdeltproject.org/about.html#termsofuse)
- [GDELT GKG 2.0 codebook](https://data.gdeltproject.org/documentation/GDELT-Global_Knowledge_Graph_Codebook-V2.pdf)
- [GDELT partitioned BigQuery tables](https://blog.gdeltproject.org/announcing-partitioned-gdelt-bigquery-tables/)
- [Google BigQuery public datasets](https://docs.cloud.google.com/bigquery/public-data)
- [BigQuery jobs.query API](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/query)
