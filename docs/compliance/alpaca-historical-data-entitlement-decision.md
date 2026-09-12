# Alpaca historical-data entitlement decision

## Decision

As of 2026-09-11, Aegis-MX conditionally permits Alpaca Basic IEX historical
one-minute bars for an owner-only, internal academic, non-commercial POC. The
operator explicitly accepted the public personal/non-commercial restriction
and instructed the application to proceed within that scope. Raw-data
redistribution, external publication, resale, commercial use, and broader
account access remain prohibited.

This is a conservative application authorization, not legal advice and not a
claim that Alpaca issued a bespoke academic or ML-training license. If the
operator cannot continue to make the personal/non-commercial representation,
or if the account or governing terms change, new acquisition fails closed.

## Exact scope

- Plan: Alpaca Basic
- Dataset: Market Data API v2 Historical Stock Bars
- Query semantics: `feed=iex`, `timeframe=1Min`, `adjustment=raw`
- Universe: exact current `ticker.txt` snapshot
- Sessions: regular session only
- Window: five-session pilot followed by no more than the rolling two-year
  window ending at the latest complete market session
- Storage: owner-only `/scratch/djy8hg/aegis_mx_poc_data`
- Uses: internal research, backtesting, POC training/validation, and retained
  internal derived artifacts
- Distribution: none
- Expiry/review: `2026-10-11T23:59:59Z`

## Evidence and interpretation

Official Alpaca documentation describes historical-data retrieval, Basic IEX
coverage, history since 2016, and a 200 historical-requests-per-minute limit.
The public terms support personal/non-commercial use and restrict reproduction,
distribution, sale, and commercial exploitation. They do not expressly answer
every modern ML-artifact question. The application therefore applies the
narrowest practical scope: single owner, internal non-commercial work, no raw
data sharing, short authorization lifetime, immutable provenance, and
immediate reassessment on change.

Primary evidence:

- [Market Data API plans](https://docs.alpaca.markets/us/docs/about-market-data-api)
- [Historical stock data](https://docs.alpaca.markets/us/v1.1/docs/historical-stock-data-1)
- [Historical bars reference](https://docs.alpaca.markets/us/reference/stockbars)
- [Market Data FAQ](https://docs.alpaca.markets/us/docs/market-data-faq)
- [Alpaca Terms and Conditions](https://files.alpaca.markets/disclosures/library/TermsAndConditions.pdf)
- [Alpaca Customer Agreement](https://files.alpaca.markets/disclosures/library/AcctAppMarginAndCustAgmt.pdf)

Retrieved Terms and Conditions SHA-256:
`2dc774d4aeeafbe4c7f0565e7842d932bc8bc10488af805fce43b8734e7b9859`.
Retrieved Customer Agreement SHA-256:
`d086d69f063bba1e9693fffbb3185c4469b24b47bda79f587053bd771580fcf6`.

## Enforcement

The committed source-policy example remains fully disabled. A private,
owner-only policy activates only `alpaca_iex_historical_bars` and is bound to
the approval, universe, data root, dataset, purpose, expiry, and rights matrix
by SHA-256. The adapter rejects mismatches before bar retrieval, loads secrets
only from a `0600` regular file, never logs secret values, prohibits redirects,
rate-limits below the Basic ceiling, and exposes no order operation.

The approval can be revoked without code changes. Revocation or expiry blocks
new requests; it does not silently delete immutable data. A cleanup plan and a
fresh rights review determine retained-artifact treatment.
