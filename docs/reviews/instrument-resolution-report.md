# Instrument-resolution report index

The complete Prompt 52 report covers every symbol in the authoritative
`ticker.txt` snapshot while keeping provider-derived identifiers in the
owner-only data root:

```text
/scratch/djy8hg/aegis_mx_poc_data/reports/reference-data/prompt-52/instrument-resolution-report-v1.json
/scratch/djy8hg/aegis_mx_poc_data/reports/reference-data/prompt-52/instrument-resolution-report.md
```

The machine report contains 79 source-ordered entries and has file SHA-256
`d5a5d1776992445ccd7d815cbb3110394b24a2deb8afe94bebd24228447b271d`.
It records 78 current-only resolutions, one unsupported OTC venue, 79 stable
internal identities, and zero historically complete mappings. Every entry
explicitly marks listing/delisting dates, historical symbology, security
subtype, splits, dividends, and halt history unresolved.

The report status is `PARTIAL_REFERENCE_COVERAGE`, with
`safe_for_historical_training=false`, `economic_value_claimed=false`, and
`live_trading_capable=false`. Current mappings are not represented as
historical truth.

See the [reference-data architecture](../architecture/point-in-time-instrument-reference.md)
and [test evidence](../testing/point-in-time-reference-data-testing.md).
