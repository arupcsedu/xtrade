# SEC EDGAR POC ingestion

## Boundary

`aegis-sec-edgar` is an offline/asynchronous research command. Its only network
operation is GET against exact `www.sec.gov` or `data.sec.gov` paths. It cannot
submit EDGAR filings, cannot obtain EDGAR Next filer tokens, and cannot publish
an order, intent, risk decision, OMS command, or gateway message.

The selected official objects are:

- `/files/company_tickers.json`, used only as a current association;
- `/submissions/CIK##########.json` and validated SEC-named historical shards;
- `/api/xbrl/companyfacts/CIK##########.json`;
- the declared primary document under one validated accession directory.

The all-filer `submissions.zip` and `companyfacts.zip` archives are rejected for
this bounded universe. Arbitrary exhibits, PDFs, scripts, ZIPs, media, and
external links are not fetched.

## Data flow and limits

The authoritative `ticker.txt` snapshot is resolved against the current SEC
file. Resolved CIKs are fetched with a configured identifying User-Agent,
bounded concurrency, an eight-request-per-second default, explicit 429/5xx
retry limits, response byte limits, and bounded gzip/deflate expansion.

JSON parsing enforces a 64 MiB decoded limit, depth 32, two million nodes,
20,000 selected filings per issuer, and one million selected facts per issuer.
Only standard entity-wide taxonomies are retained. Fact numbers use integer
coefficient and base-10 scale.

Execution requires an explicit filing start and end date. Historical shard
metadata is validated for issuer identity, count, and ordered date range; only
shards intersecting that window are fetched. Both current and historical rows,
and Company Facts observations, are filtered on filing date before entering
coverage counts. Duplicate accessions across partitions reject the issuer.

Primary HTML documents have a 1 MiB byte limit, one million-character limit,
250,000-node limit, depth 128, and two-second parser deadline. Script, style,
iframe, object, embed, form, SVG, MathML, and template content is removed with
all attributes. Suspicious instruction text is retained for exact evidence but
removed from the analysis view. At most 32 selected Item excerpts of at most
4,096 characters are emitted. Production document ingestion uses the existing
spawned process boundary with cleared environment, network denial, CPU, memory,
file-size, file-descriptor, and wall-time limits. Inline sanitization exists only
for deterministic test/replay fixtures.

Storage uses SHA-256 content paths, immutable self-hashed manifests, atomic hard
link publication, global data-root admission, authoritative quota evidence, and
an 8,000,000,000-byte SEC cap. Unknown quota or insufficient reserve fails
closed. A coverage operation reserves the remaining SEC ceiling once under the
global single-writer fence, while individual objects remain independently
hashed and immutable. This avoids rescanning the existing minute repository per
object without relaxing the root limit, temporary-space bound, or 50 GB
reserve. No data is deleted automatically.

## Point-in-time semantics

- `acceptance_time_utc_ns`: exact SEC `acceptanceDateTime`.
- `publication_time_utc_ns`: nullable; absent in Submissions JSON and never
  inferred.
- `receipt_time_utc_ns`: wall-clock UTC when this process received the object.
- `processing_time_utc_ns`: wall-clock UTC after bounded parsing.
- filing and report dates remain separate calendar dates.
- amendments are immutable and do not overwrite originals.

Current CIK associations are not historical symbology. Unknown or ambiguous
tickers remain in the coverage report with reason codes.

## Intentionally unresolved source boundaries

- The current SEC ticker association cannot prove historical symbol ownership.
  Completing that field requires separately authorized historical symbology.
- Submissions JSON does not expose a separate publication timestamp; acceptance
  time is not substituted for it.
- Submissions JSON does not identify an amendment's parent accession. The
  implementation accepts only separately verified explicit links and never
  promotes a date/form heuristic to source fact.
- Primary-document retrieval is implemented and separately authorized, but a
  raw document corpus is not downloaded or used for training under the current
  approval. Required documents remain opt-in, size-bounded, sandboxed evidence.
