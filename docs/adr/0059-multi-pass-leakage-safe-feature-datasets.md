# ADR 0059: Multi-pass leakage-safe feature datasets

- Status: Accepted
- Date: 2026-09-12

## Context

The forecasting POC needs features and twelve forward-return labels from the
same canonical one-minute history. A one-pass implementation cannot know
cross-sectional market/sector values or TRAIN-only normalization parameters
without either retaining the complete universe in memory or revisiting the
immutable input. It must also distinguish exchange-event cutoffs from the time
at which news, macro, reference, and correction facts became knowable.

## Decision

Use repeatable bounded passes over hash-verified Prompt 56 partitions:

1. determine observed sessions and calculate minute-keyed market and
   point-in-time sector aggregates;
2. allocate chronological TRAIN, VALIDATION, and TEST sessions with exactly 42
   excluded sessions at both boundaries, then fit integer sufficient
   statistics from TRAIN rows only;
3. stream feature/label rows and minute-derived session summaries to
   per-instrument/split Zstandard Parquet objects while accumulating bounded
   coverage and leakage evidence.

The implementation may reread immutable partitions to keep memory bounded. It
may retain only one configured instrument history plus bounded aggregate,
normalization, coverage, and manifest state. It never derives a missing minute
by filling and never downloads a daily-bar dataset.

Each market or sector aggregate records the latest local processing time among
its contributing bars. A feature row uses that aggregate only when the entire
aggregate was available by the row's knowledge cutoff; it does not admit a
later-processed constituent merely because the exchange minute matches.

Every feature row carries an exchange-event cutoff and a separate knowledge
cutoff. Advisory news and macro revisions require `available_at <= knowledge
cutoff`. Every valid label resolves through the versioned exchange calendar,
starts strictly after the feature interval, and carries an explicit target
timestamp, return PPM, direction, future integer ticks, and the source-row
corporate-action version. A tick-grid or action-version change makes the label
invalid rather than silently adjusting with future knowledge.

The storage repository admits projected output before writing. Data objects are
content addressed; a self-hashed dataset manifest is atomically published last
and is the only acceptance marker. Orphan objects from an interrupted run are
not accepted as a dataset.

## Consequences

- Input I/O is intentionally traded for bounded memory and auditability.
- Cross-sectional aggregate state is bounded but can be material; exceeding
  the configured point limit fails closed.
- Null normalization is retained for features with no TRAIN observations.
- The POC cannot build an accepted real dataset until historical canonical
  minutes and point-in-time reference/action coverage are both accepted.
- These artifacts are offline research inputs and provide no order authority.
