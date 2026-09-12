# ADR 0055: Bounded issuer-specific SEC EDGAR ingestion

- Status: Accepted
- Date: 2026-09-11

## Context

The POC needs filing metadata and structured facts for the ticker universe, but
SEC ticker associations are current rather than historical, submission records
do not expose a separate publication timestamp, filer documents are untrusted,
and the full SEC bulk archives are far broader than this 79-symbol POC.

## Decision

Use fixed-host, read-only HTTPS against the official current ticker file,
issuer-specific Submissions endpoints, issuer-specific Company Facts endpoints,
only SEC-declared historical submission shards whose date ranges overlap the
explicit POC filing window, and explicitly selected primary filing documents.
Default to eight requests per
second and at most two concurrent workers, below the published ten-request
maximum. Redirects, queries, credentials, bulk archive mirrors, arbitrary
attachments, and non-SEC hosts are rejected.

Keep filing acceptance time, source publication time, local receipt time, and
local processing time distinct. A missing publication time is `null` with
`NOT_EXPOSED_BY_SUBMISSIONS_API`; it is never replaced by acceptance or receipt
time. Current ticker-to-CIK results are marked `RESOLVED_CURRENT_ONLY` and may
not backfill historical issuer identity.

XBRL numbers are parsed as exact decimals and stored as integer coefficient and
base-10 scale. Amendments remain immutable records. Because the source metadata
does not identify an amendment parent, lineage stays unresolved unless a
separate explicit accession-to-accession assertion is supplied and validated.

Raw content is content-addressed behind global storage admission and a second
8 GB SEC-specific cap. Filing HTML is bounded, active elements and attributes
are discarded, prompt-like instructions remain inert and are excluded from the
analysis view, and only exact sanitized evidence excerpts are emitted. Filing
content has no dependency on risk, OMS, routing, gateways, or order entry.

Issuer fetches may run concurrently, but local publication is serialized. One
coverage operation reserves the remaining SEC-specific ceiling once while it
holds the data repository's interprocess single-writer fence. Each object and
manifest is still independently hashed and immutable. This prevents concurrent
workers from colliding at admission without repeating a complete data-root scan
for every SEC object.

## Consequences

Per-issuer requests are efficient for this universe; the nightly all-filer bulk
ZIPs are deliberately not used. Coverage report schema v1.1 records the exact
filing window and historical-shard count. Historical ticker resolution remains
incomplete until a separately authorized historical symbology source exists.
Raw filing documents are not an ML training corpus under this decision.

The source does not expose an authoritative publication timestamp or an
explicit amendment-parent accession in Submissions JSON. Those fields remain
null or unresolved unless separately sourced; inventing either would violate
the point-in-time contract.

## Primary evidence

- [SEC EDGAR data APIs](https://www.sec.gov/search-filings/edgar-application-programming-interfaces)
- [SEC developer resources and fair access](https://www.sec.gov/about/developer-resources)
- [SEC access, timestamp, and correction guidance](https://www.sec.gov/search-filings/edgar-search-assistance/accessing-edgar-data)
