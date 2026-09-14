# Prompt 57 real-data label coverage

- Observation date: 2026-09-13
- Ticker source SHA-256: `c4bfd957725741d76f847405294258f4a8d31d16dff58b0a7c033bafbb446f79`
- Universe entries: 79
- Required horizons per entry: 12
- Coverage rows: 948
- Resolved symbols: 78
- Explicitly abstaining symbols: 1 (`KRKNF`, unsupported OTC)
- Accepted canonical partitions: 4,000
- Canonical minute records: 9,462,709
- Feature/label samples: 7,684,385
- Session summaries: 36,520
- Feature and summary objects: 306
- Dataset object bytes: 2,541,840,847 decimal bytes
- Dataset ID: `7859608e394a94ac570f221ff382c48a1d4a1c335cc9a29ef434ca35bec411ca`
- Dataset manifest SHA-256: `b8e2239410a5a2a8051d019ff11b9523187a74b83250201a2a60b3ab34ee9215`
- Leakage status: `PASS` over 7,684,385 samples
- Status: `READY_FOR_TRAINING_ADMISSION_ASSESSMENT`

The real Prompt 57 build completed from accepted Prompt 56 Parquet; no
synthetic market history was substituted. Every resolved symbol has more than
the reviewed minimum of 100 valid labels at every horizon. `KRKNF` remains in
all twelve rows with zero samples and explicit unsupported-venue provenance.
This table is label coverage, not the stricter selected-feature/label
intersection; that model-ready evidence is published by the
[training-readiness gate](../architecture/training-readiness.md).

| Horizon | Valid | Missing | Invalid | Minimum valid for one resolved symbol |
| --- | ---: | ---: | ---: | ---: |
| 5m | 6,237,797 | 1,446,542 | 46 | 311 |
| 10m | 6,214,246 | 1,470,050 | 89 | 250 |
| 15m | 6,197,394 | 1,486,858 | 133 | 239 |
| 30m | 6,165,850 | 1,518,280 | 255 | 199 |
| 60m | 6,120,825 | 1,563,106 | 454 | 179 |
| 2h | 6,054,608 | 1,628,967 | 810 | 144 |
| 5h | 6,018,607 | 1,663,800 | 1,978 | 128 |
| 1d | 7,576,529 | 104,630 | 3,226 | 1,271 |
| 1w | 7,472,142 | 197,082 | 15,161 | 1,112 |
| 2w | 7,351,315 | 305,189 | 27,881 | 1,060 |
| 1mo | 7,084,693 | 546,690 | 53,002 | 971 |
| 2mo | 6,571,667 | 1,031,218 | 81,500 | 736 |
| Total | 79,065,673 | 12,962,412 | 184,535 | — |

Missing targets remain missing: warm-up, absent future target minutes/session
ends, and the complete provider gap on 2025-03-10 are never filled. Labels
crossing a changed corporate-action version remain invalid. These outcomes are
quality evidence, not model errors.

The fixed current cohort, incomplete historical halt evidence, unobserved
historical publication time, IEX-only coverage, and research price-quantum
limitations remain explicit. They permit infrastructure validation under
[ADR 0061](../adr/0061-training-admission-with-degraded-source-evidence.md),
but not an economic-value, survivorship-bias-free, consolidated-market, or
production-readiness claim.

The exact 948-row machine-readable evidence is
[`forecasting-poc-label-coverage.json`](forecasting-poc-label-coverage.json).
