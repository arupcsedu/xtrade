# Forecast universe contract

## Scope and authority

The bounded forecasting POC reads symbols only from
`/scratch/djy8hg/xtrade/ticker.txt`. `load_ticker_universe` intentionally has
no caller-supplied path parameter. Tests may parse supplied bytes through
`parse_ticker_universe`, but production POC snapshots always record the exact
authoritative path.

The parser accepts ASCII input and a final line without a newline. Empty lines
are ignored. Every non-empty line must contain one uppercase symbol of at most
16 bytes. The accepted grammar is an uppercase letter followed by uppercase
letters or digits and, optionally, one `.` or `-` qualified suffix. Whitespace,
provider/exchange qualifiers, paths, delimiters, non-ASCII text, duplicates,
and ambiguous spellings reject the complete snapshot. No normalization is
performed because normalization could silently change the requested universe.

## Identity and ordering

The immutable snapshot contains two views:

- `ordered_entries` preserves source-file order and a contiguous source
  ordinal for operator reporting;
- `canonical_symbols` sorts the same unique symbols lexicographically for
  deterministic processing.

The source-file SHA-256 hashes the exact bytes, including blank lines, line
endings, order, and final-newline state. The universe-snapshot SHA-256 hashes
canonical ASCII JSON containing the schema version, authoritative path, source
digest, and sorted entries. Each entry retains its source ordinal, resolution
code, and stable `InstrumentId` when resolved. Reordering the file therefore
changes both identities even if its symbol set is unchanged.

Instrument resolution is injected from an approved point-in-time reference
snapshot. A successful result requires a nonzero stable `InstrumentId`, and
two symbols may not resolve to the same identity in one universe. Unsupported,
recently listed, delisted, ambiguous, and missing-reference symbols remain in
the snapshot with explicit reason codes and no identifier. They are not silently
removed, aliased, or guessed.

## Serialization and failure behavior

Universe snapshots use bounded canonical JSON because they are offline control
artifacts, not hot-path records. Decoding rejects oversized or non-canonical
bytes, unknown fields or enum values, invalid identities, order violations, and
digest mismatches. The input bound is 1 MiB/10,000 symbols and the snapshot
bound is 8 MiB. Any parser, I/O, or resolver failure rejects the whole snapshot.

This contract performs no network access and grants no market-data or trading
authority. The source and resolution snapshots must be recorded with any
dataset, training run, or forecast evaluation that uses them.

See the [POC contract](forecasting-poc-contract.md), [horizon contract](forecast-horizons.md),
and [ADR 0046](../adr/0046-exchange-calendar-forecast-horizons.md).
