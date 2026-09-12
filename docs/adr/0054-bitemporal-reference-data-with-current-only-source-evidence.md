# ADR 0054: Bitemporal reference data with current-only source evidence

| Field | Value |
| --- | --- |
| Status | Accepted |
| Date | 2026-09-11 |
| Scope | Forecasting POC Prompt 52 |

## Context

The forecasting POC needs stable instrument identities, point-in-time symbol
mappings, exchange sessions, corporate actions, and halt history. The accepted
source policy authorizes Alpaca Basic IEX minute bars and the current asset and
calendar observations already retained by that acquisition. It does not
authorize persistent Nasdaq Trader ingestion. SEC current ticker mappings are
public current mappings, not an authoritative historical symbology or
corporate-action source.

Backdating a current asset mapping across the two-year market-data window would
create future-reference leakage. Treating missing action or halt records as
evidence that no action or halt occurred would create a second, less visible
form of leakage.

## Decision

Implement a separate, closed JSON v1 reference snapshot and resolution report
for offline research. Each revision carries a business-effective interval and
independent source observation, processing, revision, optional source
publication, and safe availability times.

Stable Alpaca instrument IDs use the existing domain-separated derivation:

```text
SHA-256("ALPACA-ASSET-V1:" || provider_asset_id)[0:16]
```

This preserves identity parity with the canonical minute records without
publishing provider asset identifiers. Current asset mappings become effective
at their actual retained observation time; they are not applied backward.
Unsupported venues may retain an internal stable identity, but the forecast
universe resolver returns an explicit unsupported result without an eligible
`InstrumentId`.

Calendar sessions retained by the backfill are represented with exact UTC open
and close timestamps. Weekends are structural closures. A missing weekday is
`UNCLASSIFIED_CLOSURE` unless an approved source identifies it as a holiday;
the implementation does not infer a holiday name. Early closes derive only
from the recorded close boundary. Halt coverage remains incomplete.

Splits use a reduced rational `new_shares / old_shares`. Price adjustment uses
the exact reciprocal with checked signed-int64 multiplication and an explicit
rounding policy. Cash dividends use integer currency nanos and stay separate
from split adjustment. An action is visible only when both known and effective
at the requested cutoffs.

The detailed 79-symbol report and provider-derived identifiers remain in the
owner-only external data root. The repository retains contracts, code,
aggregate evidence, hashes, and limitations—not licensed/raw provider payloads.

## Consequences

- Current mapping coverage is useful for forward POC operation but insufficient
  for point-in-time historical training.
- Prompt 56 and later historical datasets must abstain or acquire an authorized
  historical source before relying on mappings, splits, dividends, delistings,
  or halts.
- Nasdaq Trader and SEC adapters remain disabled; no provider semantics are
  fabricated.
- The contract adds no order, gateway, or live-trading capability.

## Compatibility and rollback

The new JSON contracts are side-by-side with FlatBuffers v1.9 and the Python
point-in-time v1 contract. No existing record meaning or wire layout changes.
Readers reject unknown schema versions and noncanonical or hash-mismatched
artifacts. Rollback stops new reference publication and retains all v1 evidence
immutably; it does not rewrite the source backfill or generated artifacts.
