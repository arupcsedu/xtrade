# ADR 0053: Bounded Alpaca IEX minute acquisition

- Status: Accepted
- Date: 2026-09-11
- Scope: Forecasting POC only

## Context

The forecasting POC needs real one-minute US-equity observations without
expanding into consolidated quotes, ticks, depth, options, daily-bar copies, or
live execution. Alpaca Basic provides historical IEX bars and the operator has
accepted a narrow owner-only academic/non-commercial application policy.

The checked-in provider-neutral framework already supplies immutable manifests,
quota admission, atomic publication, checkpoints, and deterministic dataset
identities. A provider-specific implementation must preserve those controls
and must not inherit any paper-order authority.

## Decision

Implement a GET-only adapter for exactly:

```text
host: data.alpaca.markets
path: /v2/stocks/bars
feed: iex
timeframe: 1Min
adjustment: raw
sessions: Alpaca calendar regular-session interval
```

The adapter uses the exact paper origin only for read-only clock, calendar, and
asset-resolution calls. It rejects redirects and other hosts, limits responses
to 16 MiB, runs below the documented Basic rate ceiling, bounds retries, and
stores raw response bytes before canonicalization. Prices become integer USD
nanounits; quantities remain integer shares. Missing observations are explicit.

The five-session pilot is a mandatory gate. A backfill requires an immutable
pilot acceptance bound to the same source policy, approval, and universe hash.
The backfill window starts on the first market date on or after the calendar day
following the same date two years earlier and ends at the latest complete
market session. One process owns a content-derived run epoch.

The checkpoint records source manifest IDs, the continuation token, page
index, and an explicit `download_complete` flag. This distinguishes an initial
task from an interrupted task whose final page is already durable, preventing
an unnecessary final-page refetch during recovery. Legacy checkpoints infer
that state only when at least one page exists and no continuation token remains.

## Consequences

- IEX bars are not SIP or consolidated-market observations and must be labeled
  accordingly.
- `KRKNF`, an OTC instrument, remains in universe reports but is unsupported by
  this approved source scope.
- Raw and canonical data live outside Git and are limited by both project and
  cluster quota admission.
- The authorization expires and can be revoked without a code deployment.
- No adapter method can submit, cancel, replace, or otherwise create an order.

## Alternatives rejected

- Synthetic filling was rejected because it hides real coverage gaps.
- Separate daily bars were rejected because longer-session features can be
  derived from minute data.
- SIP and broad provider selection were rejected because they exceed the
  approved dataset and POC scope.
- A single unconstrained multi-year request was rejected because bounded tasks
  and checkpoints provide clearer recovery and audit evidence.
