# ADR 0056: Bounded GDELT GKG query and advisory events

- Status: Accepted
- Date: 2026-09-11
- Scope: Prompt 54 forecasting POC

## Context

The POC needs two years of issuer-relevant open-news metadata without storing a
global GDELT mirror or assuming rights to publisher article bodies. GDELT raw
updates are broad 15-minute archives, while the partitioned BigQuery GKG table
supports server-side date and organization predicates. Google access has a
separate project, billing, authentication, and quota boundary.

GKG organization extraction is noisy and a company ticker is not an issuer
identity. GKG `DATE`, a publisher publication time, and Aegis receipt time also
cannot be treated as interchangeable.

## Decision

Use parameterized, partition-pruned reads of
`gdelt-bq.gdeltv2.gkg_partitioned`. Plans are split by calendar month and at
most 32 unambiguous normalized organization aliases. Each query has fixed row
and processed-byte bounds. More than the row bound fails the partition; it is
never silently truncated.

Resolve only exact normalized current SEC issuer names and deterministic legal
suffix variants to the stable `InstrumentId` from Prompt 52. A normalized
alias shared by issuers is excluded from remote queries and is ambiguous at
ingestion. Bare ticker strings are never aliases. The current SEC association
is explicitly not represented as historical issuer truth.

Persist only GDELT GKG identifiers, provider observation time, source domain,
normalized URL, organizations, themes, language metadata, hashes, local
receipt/processing times, entity decisions, and deterministic advisory labels.
Publisher full text is neither fetched nor stored. Precise publication time is
null unless a future additive, source-validated field proves it.

All classified events are `advisory_only=true` and
`live_trading_capable=false`. This Python subsystem has no dependency on risk,
OMS, routing, or gateways. GDELT artifacts, manifests, and reports share a hard
5,000,000,000-byte decimal cap enforced inside the broader POC repository.

## Consequences

- Historical retrieval requires a Google Cloud project, BigQuery API access,
  short-lived access credentials, billing/quota, and explicit owner approval.
- Missing publication time and ambiguous/unresolved identities degrade quality
  and may require abstention in later research.
- Exact matching reduces false positives at the cost of lower recall.
- Same-provider IDs, content-derived duplicates, and same-URL contradictions
  remain independently visible.
- GDELT attribution is embedded in manifests and reports. Publisher rights are
  never inferred from GDELT dataset rights.

## Rejected alternatives

- Mirroring raw two-year 15-minute archives violates the issuer-only and 5 GB
  constraints.
- The DOC API cannot provide a complete bounded two-year corpus.
- Ticker substring matching is vulnerable to ordinary words and ambiguous
  names.
- Fetching linked pages would cross a separate publisher-rights and security
  boundary.
