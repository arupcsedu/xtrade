# SMX/1 synthetic exchange protocol

## Scope and license boundary

SMX/1 is a project-owned binary fixture protocol for deterministic testing. It
does not imitate, claim compatibility with, or derive semantics from a real
exchange, broker, or market-data provider. Its names, magic values, layouts, and
state rules exist only in this repository. It has no order-entry direction and
contains no account, credential, endpoint, entitlement, or live-trading field.

The normative architectural decision is
[ADR-0006](../adr/0006-license-clean-synthetic-exchange.md).

## Encoding rules

- All multibyte integers use big-endian/network byte order.
- Signed values use two's-complement representation.
- No C++ structure is copied to or from the wire; encoders and decoders operate
  field by field, so padding and host endianness are irrelevant.
- Unknown major versions, types, sides, actions, flags, nonzero reserved fields,
  invalid lengths, invalid identifiers, zero sequences, checksum mismatches,
  and trailing bytes fail closed.
- Every version-1 packet contains exactly one 108-byte message.
- FNV-1a-64 uses offset basis `14695981039346656037` and prime
  `1099511628211`. It is an error-detection and reproducibility hash, not a MAC
  or signature.

## Capture header

An `.smxcap` file begins with this fixed 80-byte header:

| Offset | Size | Field | Semantics |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | ASCII `SMXC` |
| 4 | 1 | major | `1` |
| 5 | 1 | minor | `0` |
| 6 | 2 | header bytes | `80` |
| 8 | 2 | scenario | SMX scenario code |
| 10 | 2 | flags | zero in v1 |
| 12 | 8 | seed | explicit generator seed |
| 20 | 8 | configuration hash | FNV-1a-64 over the normalized configuration |
| 28 | 8 | logical event count | ordered generator events |
| 36 | 8 | physical packet count | records after packet fault injection |
| 44 | 8 | start exchange time | signed Unix-epoch nanoseconds |
| 52 | 8 | stale threshold | nanoseconds |
| 60 | 8 | expected book hash | logical final-book FNV-1a-64 |
| 68 | 8 | header hash | FNV-1a-64 over bytes `[0, 68)` |
| 76 | 4 | reserved | zero |

The writer initially emits a zero book hash and rewrites the header only during
successful finalization. A verifier rejects a partial/unfinalized header.

## Capture record

Each capture record immediately follows the previous record:

| Offset | Size | Field | Semantics |
| ---: | ---: | --- | --- |
| 0 | 8 | capture time | signed Unix-epoch nanoseconds at synthetic receive |
| 8 | 2 | packet bytes | exactly `156` in SMX/1 |
| 10 | 2 | flags | zero in v1 |
| 12 | 156 | packet | one complete SMX packet |

EOF must occur exactly after the declared physical packet count. Truncation or
trailing bytes is invalid.

## Packet header

Each 156-byte packet has a 48-byte header followed by one message:

| Offset | Size | Field | Semantics |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | ASCII `SMXP` |
| 4 | 1 | major | `1` |
| 5 | 1 | minor | `0` |
| 6 | 2 | header bytes | `48` |
| 8 | 2 | packet bytes | `156` |
| 10 | 2 | message count | `1` |
| 12 | 4 | venue | nonzero synthetic venue number |
| 16 | 4 | channel | nonzero synthetic channel number |
| 20 | 8 | packet sequence | nonzero, scoped to venue/channel |
| 28 | 8 | send time | signed Unix-epoch nanoseconds |
| 36 | 8 | payload hash | FNV-1a-64 over the 108 message bytes |
| 44 | 4 | flags | defined mask only; zero is normal |

Packet flags are `0x1` burst timing, `0x2` first packet after a stale interval,
and `0x4` semantic scenario injection. Any other bit is invalid.

## Message layout

The fixed 108-byte message is encoded as follows:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 2 | message bytes (`108`) |
| 2 | 1 | message type |
| 3 | 1 | side |
| 4 | 1 | normalized book action |
| 5 | 1 | trading status |
| 6 | 2 | message flags |
| 8 | 4 | instrument number |
| 12 | 8 | channel sequence |
| 20 | 8 | global logical ordinal |
| 28 | 8 | exchange event time |
| 36 | 8 | NIC receive time |
| 44 | 8 | process monotonic time |
| 52 | 8 | synthetic native order identifier |
| 60 | 8 | price ticks |
| 68 | 8 | native/event quantity units |
| 76 | 8 | resulting level or secondary quantity units |
| 84 | 4 | resulting order count |
| 88 | 4 | auxiliary code |
| 92 | 8 | auxiliary value |
| 100 | 8 | event hash |

The event hash is FNV-1a-64 over bytes `[0, 100)`. The packet payload hash then
covers all 108 bytes, including the event hash.

### Codes

Message types are `1 ADD_ORDER`, `2 CANCEL_ORDER`, `3 MODIFY_ORDER`,
`4 PRICE_LEVEL`, `5 TRADE`, `6 QUOTE`, `7 AUCTION_IMBALANCE`, and
`8 TRADING_STATUS`.

Side is `0 NONE`, `1 BID/BUY`, or `2 ASK/SELL`. Book action is `0 NONE`,
`1 ADD`, `2 CHANGE`, `3 DELETE`, `4 CLEAR_SIDE`, or `5 CLEAR_BOOK`. Trading
status uses the canonical numeric codes only for `TRADING_STATUS`; it is zero
otherwise.

Message flags are `0x1 ORDER_LEVEL`, `0x2 PRICE_LEVEL`, `0x4 SCENARIO_FAULT`,
`0x8 HIDDEN_REPLENISHMENT`, and `0x10 SHOCK`. Unknown bits are invalid.

### Type-specific fields and canonical normalization

| Native type | Field interpretation | Canonical payload |
| --- | --- | --- |
| `ADD_ORDER` | order id and order quantity; resulting aggregate level quantity/count | `BookUpdate` |
| `CANCEL_ORDER` | cancelled order id/quantity; resulting aggregate level quantity/count | `BookUpdate CHANGE/DELETE` |
| `MODIFY_ORDER` | order id and replacement quantity; resulting aggregate level | `BookUpdate` |
| `PRICE_LEVEL` | complete resulting aggregate level quantity/count | `BookUpdate` |
| `TRADE` | price, trade quantity, aggressor side; auxiliary value is trade id | `TradeEvent` |
| `QUOTE` | price is bid; auxiliary value bit-pattern is ask; quantity fields are bid/ask quantity | `QuoteEvent` |
| `AUCTION_IMBALANCE` | price, imbalance/paired quantity and side | `AuctionImbalance` |
| `TRADING_STATUS` | status byte; auxiliary code is reason code | `TradingStatus` |

Every canonical event repeats the identifiers, sequence, and timestamp metadata
inside its selected payload as required by the v1 relationship validator. Order
identifiers never enter the canonical event. A canonical stream is a
concatenation of size-prefixed `AMAE` `AuditEnvelope` buffers whose payload is a
`MARKET_EVENT` `AMCR` record.

## Sequence and safe replay

Sequence is scoped to `(venue, channel)` and begins at one. The offline artifact
verifier applies only the next exact sequence, counts an exact duplicate, and
invalidates its book oracle after a gap or lower out-of-order sequence.

The online feed framework adds the recovery behavior defined by
[ADR-0007](../adr/0007-bounded-feed-recovery-and-arbitration.md). It holds later
packets in a bounded A/B arbitration window, publishes degraded health, requests
synthetic retransmission, and falls back to a validated bounded book snapshot.
No later event is normalized until continuity or snapshot state is established.
These are repository-owned SMX semantics and make no claim about a real venue.

Staleness is measured from capture timestamps using the header threshold. Book
updates while status is `HALTED` are rejected. A best bid greater than best ask
marks the book crossed and invalid. Intentional scenarios are accepted only
when header scenario, observed fault counters, and scenario expectations agree.

## Compatibility

Minor versions may assign reserved flag bits or append a new fixed layout only
after reader-first deployment and new golden fixtures. A changed offset, unit,
hash range, endian rule, or existing code meaning requires SMX/2 and a new ADR.
SMX is independent of the canonical schema version; both versions are recorded
and validated at their respective boundaries.
