# ADR 0057: Bounded ALFRED vintages and date-only availability

- Status: accepted
- Date: 2026-09-11
- Scope: Prompt 55 forecasting POC

## Context

The macro specialist needs point-in-time observations, including revisions,
without allowing final revised values to leak into historical decisions. FRED
hosts series from many owners, so an API key and API availability do not grant
blanket storage or training rights. The FRED release calendar and ALFRED real-
time periods are date-precision facts and do not prove an official intraday
publication or local receipt time.

## Decision

Use a deny-by-default, seven-series allowlist limited to federal statistical
series whose original owners document public-domain reuse. Validate provider
metadata and release identity against the checked-in contract on every run.
Exclude ISM PMI unless separate written permission is approved.

Retrieve raw, unaggregated values with FRED API `units=lin` and ALFRED
`output_type=1`. Retain every returned real-time interval as an immutable
initial/revision record. Parse values as integer microunits; do not use binary
floating point or silently impute missing values.

Treat the ALFRED `realtime_start` as `vintage_date`, not an intraday timestamp.
For deterministic as-known queries, make the record eligible only at 00:00 UTC
on the next date. Treat the corresponding converted `realtime_end` boundary as
exclusive. Store release time as null with `DATE_ONLY` or `NOT_AVAILABLE`
precision. Preserve actual local HTTP receipt and processing times separately.

Use a fixed-host, no-redirect HTTPS transport, an external key, an owner-only
self-hashed execution approval, an authoritative quota gate, bounded request
and response budgets, immutable manifests, and a strict 999,000,000-byte
source cap. The CLI remains network-free without explicit `--execute`.

## Consequences

- Revisions can be replayed exactly and cannot replace release-time values.
- Same-day intraday strategies must abstain from these date-only records. This
  is deliberately more conservative than inventing an availability time.
- Missing values suppress prior revisions while current; they are not filled.
- New series, changed units, changed metadata, or copyrighted notes require a
  policy review and fail closed until accepted.
- The finite federal allowlist is useful for the POC but is not a complete macro
  calendar or an authoritative timestamped release feed.
- Remote ingestion still requires an external FRED key and a current approval;
  implementation does not imply automatic download authorization.

## Alternatives rejected

- **Use latest FRED values for every historical date.** This causes revision
  leakage.
- **Use the release or vintage date as an intraday timestamp.** The source does
  not support that precision.
- **Allow all FRED series.** Original-owner rights differ and some series are
  expressly copyrighted.
- **Use FRED transformations in canonical data.** This obscures native units
  and transformation provenance.
- **Store decimal values as floats.** This weakens exact hashing and numeric
  reproducibility.

## Evidence

- [FRED observations API](https://fred.stlouisfed.org/docs/api/fred/series_observations.html)
- [ALFRED real-time periods](https://fred.stlouisfed.org/docs/api/fred/realtime_period.html)
- [FRED release dates API](https://fred.stlouisfed.org/docs/api/fred/release_dates.html)
- [FRED API errors and limits](https://fred.stlouisfed.org/docs/api/fred/errors.html)
- [FRED API terms](https://fred.stlouisfed.org/docs/api/terms_of_use.html)
- [BLS link and copyright notice](https://www.bls.gov/bls/linksite.htm)
- [BEA copyright FAQ](https://www.bea.gov/help/faq/147)
- [Census data stewardship policy](https://www2.census.gov/foia/ds_policies/ds027.pdf)
- [Federal Reserve Board disclaimer](https://www.federalreserve.gov/disclaimer.htm)
- [ISM terms of use](https://www.ismworld.org/footer/terms-of-use/)
