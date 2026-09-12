# Alpaca IEX minute ingestion

The Alpaca data path is an offline research boundary. It does not connect to
risk, OMS, routing, gateway send, or paper order submission.

```text
ticker.txt + private approval/policy + quota evidence
  -> clock/calendar and asset preflight (GET only)
  -> deterministic 8-symbol x 5-session tasks
  -> bounded Alpaca IEX historical-bar pages
  -> immutable raw response + source manifest
  -> regular-session validation and integer normalization
  -> immutable symbol/session partition + manifest
  -> content-derived dataset and run reports
  -> verification and deterministic sampled reinspection
```

## Canonical record

Each JSON Lines record is schema version `1.0.0` and contains:

- stable instrument identifier and source symbol;
- IEX feed and raw-adjustment provenance;
- bar start as exchange-event UTC nanoseconds;
- local receipt and processing UTC nanoseconds;
- open, high, low, close, and VWAP as integer USD nanounits;
- integer share volume and trade count;
- source-object SHA-256 and immutable record SHA-256.

Historical receipt time is the current acquisition receipt, not historical
market latency. The source bar start is the left edge of the minute. Missing
minutes are absent; they are not converted to zeros or forward-filled.

## Failure containment

Authorization, universe hash, data-root identity, credentials, calendar,
quota, pilot acceptance, and run-epoch checks all precede bar acquisition.
Every request has a timeout, maximum body size, fixed host, and bounded retry
budget. Malformed timestamps, fractional share volume, impossible OHLC values,
conflicting duplicate bars, out-of-session bars, corrupt checkpoints, and hash
mismatches reject or quarantine data explicitly.

Publication uses content-addressed immutable paths. A completed report makes
rerun idempotent; a report with a different plan cannot overwrite the existing
run. Interrupted tasks resume from a hash-verified checkpoint and page token.
An explicit final-page marker ensures interruption between download and
partition publication does not fetch the page again.

See [ADR 0053](../adr/0053-bounded-alpaca-iex-minute-acquisition.md), the
[source decision](../compliance/alpaca-historical-data-entitlement-decision.md),
and the [operations runbook](../operations/alpaca-iex-minute-ingestion.md).
