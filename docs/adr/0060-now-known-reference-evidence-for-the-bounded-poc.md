# ADR 0060: Now-known reference evidence for the bounded POC

- Status: Accepted for offline POC use
- Date: 2026-09-12
- Scope: Alpaca IEX historical minutes and research labels only
- Live-trading capability: None

## Context

The approved Alpaca Basic backfill contains 9.5 million raw one-minute bars,
but the Prompt 52 asset snapshot is a current observation. Backdating its
knowledge timestamp would be false. Alpaca historical bars also omit original
publication and local receipt timestamps. The raw-price corpus contains splits
and other structural actions that must not appear as ordinary return labels.

## Decision

Keep the two time dimensions separate:

- symbol and instrument applicability may extend over the retained bar window,
  but the mapping is marked `NOW_KNOWN_CURRENT_UNIVERSE_MAPPING` and is not
  visible before its actual acquisition time;
- Alpaca acquisition receipt remains the source availability timestamp, while
  original historical publication time remains null;
- event/news/macro features use the exchange minute endpoint as their replay
  knowledge cutoff, never the later bulk-download processing time;
- one USD nanodollar is the exact offline research price quantum. It is
  explicitly marked as not being a venue minimum tick and cannot be used by an
  execution adapter;
- a bounded `GET /v1/corporate-actions` snapshot records the exact response and
  invalidates labels that cross splits or structural actions;
- the backfill's request-universe hash continues to bind the exact ticker file
  before provider resolution. The feature dataset records a separate resolved
  universe hash derived from the immutable current asset responses; every
  canonical partition must still carry the original request-universe hash;
- absent historical halt and universe-membership evidence remains a degraded
  quality flag and a limitation on economic claims.

The selected ticker list is a fixed current-universe cohort. Results from this
POC cannot be presented as survivorship-bias-free broad-market evidence.

## Consequences

The retained bars can be converted to typed integer Parquet and used for
infrastructure/model-pipeline validation without inventing timestamps or
corporate actions. Training must retain degraded-reference flags,
action-crossing labels are invalid, and evaluation reports must disclose
fixed-cohort selection.

This decision does not authorize live trading, provider redistribution, or use
of the research quantum as an exchange tick size.

## Evidence

- [Canonical minute storage](../architecture/canonical-minute-storage.md)
- [Point-in-time contract](../architecture/point-in-time-data-contract.md)
- [Source approval](../compliance/forecasting-poc-source-approval.md)
- [Alpaca historical bars](https://docs.alpaca.markets/us/reference/stockbars)
- [Alpaca corporate actions](https://docs.alpaca.markets/us/reference/corporateactions-1)
