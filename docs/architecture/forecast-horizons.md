# Exchange-calendar forecast horizons

## Authoritative representation

Schema v1.9 appends a `HorizonSpec` and
`target_exchange_event_time: ExchangeEventTimeNs` to `ModelForecast`. The
semantic specification, calendar version, and explicit target timestamp are
authoritative. Existing `horizon_ns` remains the positive actual elapsed
interval from `as_of_exchange_event_time` to the target so v1.8 consumers can
measure expiry-like duration; it does not define trading-session semantics.

The required POC set is:

| Label | Unit | Value | Target convention |
| --- | --- | ---: | --- |
| `5m`, `10m`, `15m`, `30m`, `60m` | trading minutes | 5, 10, 15, 30, 60 | eligible minute endpoint |
| `2h`, `5h` | trading minutes | 120, 300 | eligible minute endpoint |
| `1d`, `1w`, `2w`, `1mo`, `2mo` | trading sessions | 1, 5, 10, 21, 42 | future regular-session close |

`1w`, `2w`, `1mo`, and `2mo` are explicit POC trading-session conventions,
not wall-clock calendar periods.

## Calendar and minute semantics

An `ExchangeCalendar` is an immutable, versioned, strictly ordered list of
non-overlapping regular sessions. Each session has explicit exchange-event open
and close timestamps and zero or more half-open halt intervals. Session
duration must be an exact positive number of minutes. Holidays, weekends,
overnight closures, and other closed periods appear as gaps and never count.
Early closes are represented by the stored close timestamp and therefore need
no inferred special case.

The as-of timestamp must be an eligible minute endpoint: strictly after the
session open, at or before close, and aligned to the session's minute grid.
Trading-minute resolution advances one eligible endpoint at a time, crossing
closed periods without counting them. Trading-session resolution starts with
the next session and chooses its recorded regular close after counting the
requested number of future sessions.

## Halt policy

Every trading horizon declares one policy:

- `REJECT`: encountering a halt in the requested range rejects resolution;
- `PAUSE`: halted minute intervals do not count; for session horizons a session
  whose closing minute is halted is skipped;
- `COUNT_SCHEDULED`: scheduled minute/session endpoints count despite declared
  halts.

The policy does not infer reopening or synthesize missing calendar state. An
as-of minute inside a halt is invalid under `REJECT` and `PAUSE`. An unknown
policy, absent/mismatched calendar version, insufficient calendar coverage,
invalid timestamp, or checked-integer overflow fails closed.

## Schema v1.9 migration

New v1.9 writers always publish `HorizonSpec`, an explicit target timestamp,
and an exactly matching `horizon_ns`. POC calendar-aware writers additionally
require a nonzero calendar version. Existing builder call sites are migrated to
an explicit `ELAPSED_NANOSECONDS` spec with `NOT_APPLICABLE` halt and endpoint,
no calendar version, `value == horizon_ns`, and target equal to checked
`as_of + horizon_ns`.

V1.8 readers ignore the two appended fields and retain their original elapsed
duration view. V1.9 readers continue to accept stored v1.8 records according to
their embedded minor version. Reader deployment precedes calendar-aware writer
activation. Rollback stops v1.9 calendar-aware writers before rolling readers
back. No stored forecast is rewritten in place.

See [ADR 0046](../adr/0046-exchange-calendar-forecast-horizons.md), the
[universe contract](forecast-universe.md), and the
[schema evolution policy](../../schemas/schema-evolution-policy.md).
