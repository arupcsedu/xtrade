# Prompt 52 reference-data test evidence

| Field | Value |
| --- | --- |
| Date | 2026-09-11 |
| Python environment | `/scratch/djy8hg/env/aegis_mx_contracts` |
| Deterministic fixture seed | No random input; canonical source order |
| Network access | None |
| Trading capability | False |

## Coverage

The focused suite covers ticker reuse, symbol changes without automatic
substitution, split and reverse-split arithmetic, dividends, delisting, recent
IPOs, ADR classification, OTC restriction, weekends, holidays, early closes,
missing calendars, future-known actions, conflicting mappings/calendars,
revision lineage, int64 overflow, inexact arithmetic, malformed/hash-mismatched
artifacts, atomic-write failure, and offline CLI behavior.

The generated owner-only report covers all 79 symbols from `ticker.txt`:

- 78 current-only resolved exchange-listed observations;
- one stable but unsupported OTC observation (`KRKNF`);
- zero historically complete mappings;
- 501 sessions across 730 calendar days;
- five observed early closes;
- 21 weekday closures retained as unclassified;
- no authorized action or halt history; and
- `safe_for_historical_training=false`.

Artifact evidence is stored under
`/scratch/djy8hg/aegis_mx_poc_data/reports/reference-data/prompt-52/` with
owner-only permissions. The snapshot file SHA-256 is
`42cb3128cfd74d299a54dff3b6af567521725bd39acc30fa508612ea524d245d`;
the resolution-report file SHA-256 is
`d5a5d1776992445ccd7d815cbb3110394b24a2deb8afe94bebd24228447b271d`.

## Benchmark

The 100-sample local Python benchmark is infrastructure evidence, not a hot-path
claim. It recorded:

| Category | Operations | p50 batch | p95 batch | p99 batch | Throughput |
| --- | ---: | ---: | ---: | ---: | ---: |
| Universe resolution | 7,900 | 379,961 ns | 424,814 ns | 940,429 ns | 200,888 operations/s |
| Mapping lookup | 7,900 | 258,703 ns | 281,610 ns | 290,099 ns | 300,558 operations/s |
| Calendar lookup | 73,000 | 2,426,150 ns | 2,511,538 ns | 2,539,010 ns | 301,656 operations/s |
| Split adjustment | 7,900 | 228,924 ns | 250,774 ns | 271,381 ns | 339,597 operations/s |

Platform: Linux 4.18.0 x86-64, glibc 2.28. Maximum observed process RSS was
184,536 KiB. Raw samples are retained in
`reference-benchmark-v1-final.json`, file SHA-256
`024a6170c6fe34704b45f9ca6254f71c9f1bbb61211a3ce3be48826e66f5ce0d`.
