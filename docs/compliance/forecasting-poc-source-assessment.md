# Forecasting POC source assessment

| Field | Value |
| --- | --- |
| Status | Current-source assessment; not a data license or approval |
| Assessment date | 2026-09-09 |
| Universe | 79 observed unique symbols from `ticker.txt` |
| Universe source SHA-256 | `c4bfd957725741d76f847405294258f4a8d31d16dff58b0a7c033bafbb446f79` |
| POC use | Offline/PAPER research, persistent storage, model training, and derived forecasts |
| Decision | No remote source selected or enabled |

## Scope and method

This assessment applies the [bounded forecasting POC contract](../architecture/forecasting-poc-contract.md),
the [licensed integration boundary](../architecture/licensed-integration-boundaries.md),
and the [licensed-integration checklist](licensed-integration-checklist.md).
It evaluates public, first-party documentation available on the assessment date.
It does not replace the subscriber's executed agreement, order form, exchange
agreement, series-specific copyright notice, or legal review.

The observed ticker count and digest are audit evidence, not constants. The
original assessment made no provider request. A later authorized five-session
Alpaca IEX pilot attempted every symbol and resolved 78; it retained `KRKNF` as
explicitly unsupported OTC. That current probe still does not prove complete
historical symbology or coverage.

Rights terms in this document have these meanings:

- **documented**: the cited first-party text addresses the proposed right;
- **prohibited**: the public terms disallow it absent a different written license;
- **unresolved**: the public materials do not grant enough authority for this POC;
- **not applicable**: the concept does not apply to that dataset; and
- **eligible for scoped review**: public terms support further approval, but the
  checked-in example policy remains disabled until an accountable approval
  record exists outside Git.

Free access, a working credential, a public URL, and a provider's use of the word
“open” are not interchangeable. In particular, access to bars does not by itself
grant persistent corpus storage, non-display processing, model training, or the
right to retain and distribute derived artifacts.

## Decision summary

| Source | Exact candidate dataset | Access evidence | Persistent storage | Model training | Derived outputs | POC disposition |
| --- | --- | --- | --- | --- | --- | --- |
| Massive/Polygon | Stocks Custom Bars, 1-minute aggregates | Documented by plan/API | Prohibited or unresolved under individual terms | Prohibited without license | Prohibited without license | **BLOCKED** pending written non-display/training license |
| Alpaca | Historical Stock Bars with `feed=iex`, `timeframe=1Min` | Documented for Basic users | Conditionally approved for owner-only academic POC | Conditionally approved for the same internal POC | No redistribution; retained artifacts are internal only | **CONDITIONALLY AUTHORIZED** through 2026-10-11 |
| SEC EDGAR | Submissions JSON, Company Facts XBRL, selected filing documents | Public, no API key | Eligible for scoped review | Unresolved for raw documents | Unresolved for raw documents | **DISABLED; REVIEW REQUIRED** per content class |
| GDELT | GDELT 2.0 Events/Mentions and GKG metadata | Open raw datasets | Documented for GDELT datasets | Documented by unrestricted-use language | Documented with attribution | **DISABLED; ELIGIBLE FOR SCOPED REVIEW**; publisher pages excluded |
| FRED/ALFRED | Selected series observations and vintages | API-key access documented | Series-specific | Series-specific and unresolved | Series-specific and unresolved | **BLOCKED** until a reviewed series allowlist exists |
| Nasdaq Trader | `nasdaqlisted.txt` and `otherlisted.txt` symbol directories | Public web/FTP mechanism documented | Prohibited absent written consent | Express written permission required | Express written permission required | **BLOCKED** pending written permission or replacement |

The decision states are operationally independent. A later approval for GDELT
does not authorize SEC content, ALFRED series, Nasdaq reference data, or either
market-data provider.

## Universe and coverage risks

The 79 entries are syntactically uppercase symbols, but a ticker is not a stable
instrument identity. The proposed pipeline must resolve each value to a stable
`InstrumentId`, issuer, security type, primary listing, and effective interval.
The following risks remain untested because this phase made no provider calls:

- recent listings, symbol changes, delistings, and reused tickers may not have a
  complete two-year mapping;
- ADRs and foreign issuers may have corporate actions and calendars that differ
  from domestic common stock;
- OTC or foreign-suffix identifiers, including the locally requested `KRKNF`,
  may be outside an “all US exchanges” or Nasdaq Trader directory product;
- an IEX-only bar may be absent even when a security traded elsewhere, and its
  volume is not consolidated-market volume;
- no-bar intervals must remain missing observations, not synthetic zero-volume
  or carried-forward prices; and
- SEC and GDELT identify issuers/entities, not a canonical historical equity
  instrument, so ticker-string matching is insufficient.

Until an authorized reference source resolves every symbol point in time, the
pipeline must retain `UNRESOLVED` results and abstain rather than silently remove
or substitute symbols.

## Massive/Polygon minute aggregates

| Topic | Assessment |
| --- | --- |
| Exact dataset | Stocks REST **Custom Bars (OHLC)** endpoint, `GET /v2/aggs/ticker/{stocksTicker}/range/1/minute/{from}/{to}`. The separate Stocks flat-file dataset is `us_stocks_sip/minute_aggs_v1`; it is not included in the Basic plan. |
| History and market coverage | The current Stocks Basic page advertises all US stocks, 100% market coverage, minute aggregates, and two years of history. That matches the POC time bound in description only; exact symbol/period completeness remains unproven. |
| Mechanism | Authenticated HTTPS REST with pagination. The endpoint accepts date or millisecond boundaries, queries at most 50,000 base aggregates per request, and omits periods with no eligible trade. Starter and higher individual plans add S3 flat files. |
| Adjustment behavior | REST `adjusted` defaults to `true` and adjusts for splits; `false` returns unadjusted data. Stock flat files are unadjusted. The public documentation does not claim dividend adjustment for this endpoint. |
| Published limits | Stocks Basic advertises 5 API calls per minute. Higher individual tiers advertise unlimited API calls, but a production client would still require bounded concurrency, retry, and provider-error handling. |
| Timestamp semantics | Result `t` is the Unix-millisecond timestamp at the start of the aggregate window. The endpoint covers pre-market, regular, and after-hours, so POC ingestion must filter by its versioned regular-session calendar. Local receive and processing times must be captured separately. |
| Credentials | API key required and held only in an approved external secret mechanism. No key or signed URL may enter a manifest, command line, log, or repository file. |
| Storage rights | **Prohibited/unresolved.** The public individual market-data terms grant personal, non-business, non-commercial use and restrict copying/distribution. They describe market data as display-only unless a later agreement says otherwise. That is not authority for the POC repository. |
| Model-training rights | **Prohibited without another license.** The terms prohibit non-display use and creation of derivative works unless licensed. Training is treated as non-display use; a model or forecast trained from the data is treated conservatively as a derived work. |
| Derived-data rights | **Prohibited without another license.** The public restriction expressly reaches analytics, research, and works based on or derived from market data. |
| Retention/deletion | **Unresolved.** The public individual terms do not provide a POC retention schedule or a post-termination disposition that can safely be inferred for raw data, features, datasets, models, and forecasts. An executed order form must specify each class. |
| User classes | Basic through Advanced on the cited pricing page are individual-use/non-professional products. The terms define personal non-business use and direct professionals to contact Massive. Business/commercial use is governed by a product/order form and third-party agreements. No separate public academic exemption was found. |
| Unsupported risks | OTC coverage, newly listed securities, corporate-action backfills, missing eligible-trade minutes, ticker mappings, and plan-specific historical entitlements require a preflight probe only after authorization. |

Primary evidence:

- [Massive Stocks pricing and plan coverage](https://massive.com/pricing?product=stocks)
- [Massive Stocks Custom Bars documentation](https://massive.com/docs/rest/stocks/aggregates/custom-bars?assetClass=stocks&license=personal&name=stocks_basic)
- [Massive Stocks flat-file overview](https://massive.com/docs/flat-files/stocks/overview?assetClass=stocks&license=personal&name=stocks_basic)
- [Massive Market Data Terms, updated 2025-08-28](https://massive.com/legal/market-data-terms-of-service)
- [Massive business terms](https://massive.com/legal/businesses-terms-of-service)

**Required evidence to unblock:** an executed Massive and applicable upstream
exchange/data-owner agreement naming the exact minute-aggregate product and
authorizing persistent local storage, non-display research, model training,
internal derived features/models/forecasts, the chosen user classification,
the 79-symbol/two-year scope, and retention/deletion obligations.

## Alpaca IEX historical bars

| Topic | Assessment |
| --- | --- |
| Exact dataset | Market Data API v2 Historical Stock Bars, `GET /v2/stocks/bars`, with `feed=iex`, `timeframe=1Min`, explicit `start`, `end`, `adjustment`, and `asof`. The POC must never rely on the subscription-dependent default feed. |
| History and market coverage | Alpaca's current Basic plan documents US stocks and ETFs, historical data since 2016, IEX real-time coverage, a latest-15-minute restriction, and 200 historical calls per minute. IEX is one exchange; Alpaca describes it as approximately 2.5% of US market volume. It is not a consolidated-market substitute. |
| Mechanism | Authenticated HTTPS REST at `data.alpaca.markets`, multi-symbol pagination via `next_page_token`, page limit 1–10,000 points. |
| Adjustment behavior | `adjustment=raw` is the default. Documented alternatives are `split`, `dividend`, `spin-off`, `all`, and supported combinations. `asof` controls historical symbol mapping and is not reliably available until the day after a rename. |
| Published limits | Basic: 200 historical API calls/minute. Algo Trader Plus: 10,000/minute. The policy concerns Basic IEX only; a SIP entitlement would be a separate source assessment. |
| Timestamp semantics | Minute bars are built by truncating the source trade timestamp to the minute; the bar timestamp is the left edge of `[minute, minute + 1)`. API timestamps are RFC 3339. Receipt and processing timestamps are local facts, not recoverable from historical bars. |
| Credentials | Trading API uses `APCA-API-KEY-ID` and `APCA-API-SECRET-KEY` headers. Broker API uses a client-credentials flow. Secrets must remain external; this policy grants neither account nor trading authority. |
| Storage rights | The application conditionally allows owner-only persistence for this internal academic/non-commercial POC. This is narrower than a transferable or commercial data license. |
| Model-training rights | The application conditionally allows POC training and validation under the operator's accepted personal/non-commercial scope; it does not authorize commercial deployment or an economic-value claim. |
| Derived-data rights | Internal-only features, labels, model parameters, forecasts, and reports are conditionally allowed. External distribution remains prohibited. |
| Retention/deletion | New acquisition authorization expires 2026-10-11. A fresh review decides continued retention; no automatic deletion is performed. |
| User classes | Trading API plans are described for individual traders/developers; Broker API plans are for business partners and use tailored terms. Public Alpaca terms describe personal/non-commercial use and incorporate additional market-data agreements. No separate academic grant was found. Professional/commercial use requires the exact account and contract review. |
| Unsupported risks | IEX may have no eligible trades for a requested minute; OTC needs a special broker subscription; inactive/halted symbols and ticker renames require explicit handling. An IEX-only volume or price must be labeled as such in every dataset and result. |

Primary evidence:

- [Alpaca Market Data API plans and authentication](https://docs.alpaca.markets/us/docs/about-market-data-api)
- [Alpaca historical stock data sources](https://docs.alpaca.markets/us/v1.1/docs/historical-stock-data-1)
- [Alpaca Historical Bars reference](https://docs.alpaca.markets/us/reference/stockbars)
- [Alpaca Market Data FAQ](https://docs.alpaca.markets/us/docs/market-data-faq)
- [Alpaca Terms and Conditions](https://files.alpaca.markets/disclosures/library/TermsAndConditions.pdf)
- [Alpaca current disclosures index](https://alpaca.markets/disclosures)

**Engineering disposition as of 2026-09-11:** conditionally authorized for the
single-owner academic/non-commercial scope described in the
[binding entitlement decision](alpaca-historical-data-entitlement-decision.md).
The official terms and customer agreement retrieved on that date had SHA-256 values
`2dc774d4aeeafbe4c7f0565e7842d932bc8bc10488af805fce43b8734e7b9859`
and `d086d69f063bba1e9693fffbb3185c4469b24b47bda79f587053bd771580fcf6`
respectively. The authorization is expiring and non-transferable; professional,
commercial, multi-user, redistributed, SIP, or broader uses still require a
new review and, where applicable, express provider/data-owner permission.

## SEC EDGAR

SEC is assessed independently from commercial market data and independently by
content class.

| Topic | Assessment |
| --- | --- |
| Exact datasets | `data.sec.gov/submissions/CIK##########.json`, its historical shards, `data.sec.gov/api/xbrl/companyfacts/CIK##########.json`, and selected EDGAR filing documents identified by accession number. Bulk `submissions.zip` and `companyfacts.zip` exist but are outside the bounded per-universe design unless separately admitted. |
| History and coverage | Submissions JSON contains at least the latest year or 1,000 filings and points to older shards. XBRL APIs cover standardized entity-wide facts from forms including 10-K, 10-Q, 8-K, 20-F, 40-F, and 6-K; XBRL reporting became required beginning in 2009. Availability varies by filer, form, taxonomy, and fact context. |
| Mechanism | Public HTTPS JSON and EDGAR archive documents. `data.sec.gov` requires no API key. The APIs update throughout the day; bulk ZIP files are rebuilt nightly. |
| Adjustment/revision behavior | There is no market-price adjustment. Amendments, post-acceptance corrections, XBRL contexts, units, filed facts, and accession lineage must remain distinct. A later fact must not overwrite what was known at an earlier feature cutoff. |
| Published limits | Automated EDGAR access is capped at 10 requests/second across machines and must identify the application/operator in its User-Agent under SEC fair-access guidance. The POC should operate materially below the cap with caching and backoff. |
| Timestamp semantics | Filing `acceptanceDateTime`, filing date, report-period end, and change date are distinct. The SEC says filings are often available on sec.gov within 1–3 minutes of the EDGAR system timestamp. Historical ingestion must not invent original receipt time; it records observed local retrieval separately. |
| Credentials | None for `data.sec.gov` public data. A descriptive User-Agent with contact information is required operational metadata, not a secret. EDGAR Next filer tokens are unrelated and prohibited from this read-only pipeline. |
| Storage rights | **Eligible for scoped review.** The SEC's website policy says information on sec.gov is public information and may be copied or further distributed with appropriate citation. The approval must still constrain sensitive personal information and non-SEC linked content. |
| Model-training/derived rights | **Unresolved by content class.** The public-information language supports analysis, but it does not expressly classify ML training or settle copyright in every issuer-authored exhibit, image, transcript, or linked document. Structured SEC metadata/XBRL may be approved separately from raw filing text. |
| Retention/deletion | No SEC deletion mandate was identified for the cited public information. Privacy, court-sealed/redacted changes, and source corrections still require lineage and a documented response. |
| User classes | The public read APIs do not distinguish personal, academic, professional, or commercial callers. Applicable law and the user's own regulated status still apply. |
| Unsupported risks | Ticker-to-CIK is time-varying; foreign issuers, multiple share classes, shell/blank-check entities, filing amendments, unit/context mismatch, and issuer-authored active content require strict resolution and sandboxing. |

Primary evidence:

- [SEC EDGAR data APIs](https://www.sec.gov/search-filings/edgar-application-programming-interfaces)
- [SEC fair-access rate control](https://www.sec.gov/filergroup/announcements-old/new-rate-control-limits)
- [SEC webmaster FAQ and timestamp guidance](https://www.sec.gov/about/webmaster-frequently-asked-questions)
- [SEC website privacy, security, and dissemination policy](https://www.sec.gov/about/privacy-information)

**Engineering disposition as of 2026-09-11:** the narrow Prompt 53 approval
permits current ticker associations, issuer-specific submissions, Company
Facts, and primary documents for 8-K, 10-Q, 10-K, 6-K, and 20-F within an 8 GB
cap. Raw document model training remains prohibited. Active content is removed,
instruction-like text is isolated, only bounded evidence excerpts are derived,
and arbitrary exhibits, attachments, external links, redistribution, and a
complete archive mirror remain prohibited. Re-review is required after
2026-10-11 or for any broader purpose or content class.

## GDELT

GDELT dataset rights do not grant rights to linked publisher pages.

| Topic | Assessment |
| --- | --- |
| Exact datasets | GDELT 2.0 Event and Mentions tables plus GDELT 2.0 Global Knowledge Graph metadata. Full article bodies and linked publisher content are excluded. Visual and special-collection GKG datasets are excluded. |
| History and coverage | GDELT 2.0 begins on 2015-02-19 and updates every 15 minutes. GDELT 1.0 reaches 1979 but is not selected. Coverage is global-news-derived and entity/theme oriented, not guaranteed for every ticker or issuer. |
| Mechanism | Compressed raw CSV update files and public indexes; BigQuery is an alternative subject to separate Google credentials, billing, quotas, and terms. The GDELT site reports that full raw data exceeded 2.5 TB for one year, so a broad mirror violates the POC storage contract. |
| Adjustment/revision behavior | No corporate-action adjustment. Event, mention, and GKG records have different semantics and must not be collapsed. Corrections or evolving stories are new source observations with lineage, not silent replacement. |
| Published limits | No contractual request-rate limit for raw files was found in the cited first-party pages. “No published limit” is not unlimited authority; a future adapter must use conservative polling, caching, backoff, and bounded partitions. BigQuery limits are separate. |
| Timestamp semantics | Event `DATEADDED` is UTC `YYYYMMDDHHMMSS` and is the database-addition time used for 15-minute resolution. Event date, mention time, and optional extracted article publication time are different. GDELT notes that discovery can lag publication and precise publication time is only available for a subset. Local receipt time remains separate. |
| Credentials | None for public raw-file access. Google credentials may be required for BigQuery and are not authorized by this source record. |
| Storage rights | **Documented for GDELT-released datasets.** GDELT calls its datasets available for unlimited and unrestricted academic, commercial, or governmental use without fee. |
| Model-training/derived rights | **Documented for GDELT datasets by broad unrestricted-use language.** This is not a license to scrape, store, quote, or train on the linked publisher article body. |
| Derived-data/redistribution | GDELT permits redistribution, rehosting, republication, and mirroring of its datasets, provided use or redistribution cites GDELT and links to its site. Derived reports should retain that attribution. |
| Retention/deletion | No deletion or maximum-retention requirement appears in the cited GDELT terms. The approval record must preserve attribution and document correction/removal handling. |
| User classes | The cited terms expressly cover academic, commercial, and governmental use; no different personal or professional tier is stated. |
| Unsupported risks | Company-name ambiguity, ticker-like words, subsidiaries, stale URLs, uneven language/source coverage, false correlations, delayed discovery, and missing publication timestamps make GDELT unsuitable as sole evidence for a tradable event. |

Primary evidence:

- [GDELT terms of use](https://gdeltproject.org/about.html#termsofuse)
- [GDELT datasets and access mechanisms](https://gdeltproject.org/data.html)
- [GDELT 2.0 introduction and update streams](https://blog.gdeltproject.org/gdelt-2-0-our-global-world-in-realtime/)
- [GDELT Event Database 2.0 codebook](https://data.gdeltproject.org/documentation/GDELT-Event_Codebook-V2.0.pdf)
- [GDELT publication-timestamp limitations](https://blog.gdeltproject.org/new-gkg-2-0-article-metadata-fields/)

**Required evidence to unblock:** an approved, storage-bounded query/partition
design limited to GDELT-owned Events/Mentions/GKG metadata, mandatory
attribution, explicit exclusion of publisher content, entity-resolution tests,
and an accountable authorization record. The public terms make this source
eligible for that review; they do not enable it automatically.

## FRED/ALFRED

FRED API access and rights in each underlying series are separate questions.

| Topic | Assessment |
| --- | --- |
| Exact dataset | FRED API v1 `fred/series/observations` with ALFRED `realtime_start`, `realtime_end`, `vintage_dates`, and `output_type`, plus `fred/series/vintagedates` and series metadata. The series allowlist is intentionally empty in this phase. |
| History and coverage | Series-specific. FRED/ALFRED covers economic series from many original sources; start date, frequency, vintages, units, seasonal adjustment, and update behavior must be captured per series. It is not an equity ticker source. |
| Mechanism | Authenticated HTTPS REST returning XML or JSON; observations can also return XLSX or compressed CSV. The POC should use bounded JSON and persist original source/series metadata. |
| Adjustment/revision behavior | FRED can transform units (`lin`, changes, percent changes, logs) and aggregate frequency. ALFRED real-time periods/vintage dates represent when values were released or revised. Raw source units and untransformed values must be canonical; transforms are versioned derived data. |
| Published limits | Up to 120 requests per minute before HTTP 429; repeated noncompliance can cause a temporary block. |
| Timestamp semantics | Observation `date`, `realtime_start`, `realtime_end`, series `last_updated`, release dates, and vintage dates are distinct. A vintage date records the date on which a series changed, not necessarily the official intraday publication timestamp or this system's receive time. It cannot alone timestamp an intraday macro surprise. |
| Credentials | A registered application API key is required. It must be injected externally and never written to policy, manifests, URLs, logs, or reports. |
| Storage rights | **Series-specific/unresolved.** The API terms warn that series may be owned by third parties and subject to copyright restrictions. |
| Model-training/derived rights | **Series-specific/unresolved.** For anything beyond personal use of a third-party-owned series, the terms direct the user to obtain permission from the data owner. Model training is not separately granted by API access. |
| Retention/deletion | **Series-specific/unresolved.** The API license can terminate, and underlying-owner conditions remain controlling. The POC needs an owner/series-specific retention and deletion record. |
| User classes | The API terms bind all users. They specifically distinguish personal use from other use for third-party-owned series and provide no blanket academic, professional, or commercial grant. |
| Unsupported risks | Copyrighted series, incompatible units, seasonal-adjustment changes, release revisions, unavailable vintage history, and date-only vintage semantics can cause leakage or false release timing. |

Primary evidence:

- [FRED API overview](https://fred.stlouisfed.org/docs/api/fred/overview.html)
- [FRED/ALFRED observations and vintage parameters](https://fred.stlouisfed.org/docs/api/fred/series_observations.html)
- [ALFRED series vintage dates](https://fred.stlouisfed.org/docs/api/fred/series_vintagedates.html)
- [FRED API errors and 120-request limit](https://fred.stlouisfed.org/docs/api/fred/errors.html)
- [FRED API terms and third-party series restrictions](https://fred.stlouisfed.org/docs/api/terms_of_use.html)

**Required evidence to unblock:** a finite series allowlist recording original
owner, copyright flag and notes, observation/vintage coverage, units,
publication-time source, storage, training, derivative, attribution,
retention/deletion, and the user's permitted classification for every series.

## Nasdaq Trader symbol directories

| Topic | Assessment |
| --- | --- |
| Exact dataset | Nasdaq Trader `nasdaqlisted.txt` and `otherlisted.txt`. Paid Nasdaq Daily List and market-data feeds are separate products and are not assessed or authorized here. |
| History and coverage | The public files describe current Nasdaq-listed and certain other-exchange-listed securities and are updated periodically throughout the day. The symbol lookup says its information is for the current trading day. This is not a point-in-time ten-year symbol/corporate-action history. |
| Mechanism | Pipe-delimited text over the documented Nasdaq Trader FTP directory and web-accessible files. No credential requirement is documented for those public directory URLs. |
| Adjustment/revision behavior | No price adjustments. The Nasdaq-listed file carries security/test/financial-status fields; the other-listed file carries ACT/CQS symbols and listing exchange. A current file replacement cannot erase the prior snapshot in a point-in-time repository. |
| Published limits | No request-rate guarantee was found for these files. A future authorized client must poll conservatively, honor service controls, and capture the file hash and creation row. |
| Timestamp semantics | The last row carries `File Creation Time` formatted `mmddyyyyhhmm`, generated by Nasdaq Trader. The cited directory definition does not state a timezone, so the value must remain source-local/unknown-zone until Nasdaq documents it. It is not an instrument effective time. |
| Credentials | None documented for public directory access. This does not imply storage or ML permission. |
| Storage rights | **Prohibited absent written consent.** Nasdaq's current website terms prohibit storing site content for subsequent use without prior written consent. |
| Model-training rights | **Express written permission required.** Current terms prohibit using content, data, databases, directories, or metadata to train or develop AI/ML or data-analysis software without express written permission. |
| Derived-data rights | **Express written permission required.** Current terms restrict derivative works and products/services based on content. Separate data-license terms and order forms may grant bounded rights, but none is present in this repository. |
| Retention/deletion | **Unresolved.** Without a written license there is no approved persistent retention. Any license must state snapshot retention and disposition on termination. |
| User classes | The website license is personal, limited, revocable, and non-commercial; the current ML restriction states no academic exemption. Professional/non-professional and commercial rights depend on a separate written data agreement/order form. |
| Unsupported risks | OTC securities, suffix conventions, multiple classes, intra-day adds/deletes, timezone ambiguity, and absence of historical symbol mappings prevent this directory from being the sole point-in-time reference master. |

Primary evidence:

- [Nasdaq Trader Symbol Directory definitions](https://www.nasdaqtrader.com/trader.aspx?id=symboldirdefs)
- [Nasdaq Trader current-day Symbol Lookup description](https://nasdaqtrader.com/trader.aspx?id=symbollookup)
- [Nasdaq website legal terms, updated 2026-05-11](https://www.nasdaq.com/legal)
- [Nasdaq data-license terms](https://data.nasdaq.com/terms)

**Required evidence to unblock:** Nasdaq's written permission or an executed
data-license order form that expressly covers automated retrieval, historical
snapshot storage, instrument resolution, ML/data-analysis use, derived
artifacts, the user's classification, and retention/deletion.

## Cross-source conclusions

1. **Alpaca IEX is the only conditionally authorized minute-bar source.** Its
   scope is expiring, owner-only, academic/non-commercial, and non-redistributed.
   Massive remains blocked by public non-display/derived-use restrictions.
2. **Nasdaq Trader is not authorized for automated ML/reference ingestion.** Its
   current terms require written permission, and its public files are not a
   historical reference master in any event.
3. **GDELT is the clearest public dataset license, but only for GDELT-released
   datasets.** Publisher text remains outside scope and all access stays disabled
   until bounded operational approval.
4. **SEC can be narrowed by content class.** Public structured metadata/XBRL is
   a stronger approval candidate than arbitrary filing attachments or linked
   materials.
5. **ALFRED must be authorized one series at a time.** An API key and inclusion
   in FRED do not settle the original owner's rights or publication-time meaning.
6. The minute-bar pilot/backfill may proceed within the private Alpaca approval.
   Point-in-time reference, event, and macro sources remain independently gated;
   their absence reduces later feature scope and prevents unsupported
   point-in-time completeness claims.

The binding operational decision and evidence request are in
[Forecasting POC source approval](forecasting-poc-source-approval.md). The
mechanical process is in the
[data-source authorization runbook](../operations/data-source-authorization-runbook.md).
