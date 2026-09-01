# Deterministic synthetic exchange generator

## Purpose

The synthetic exchange supplies license-clean, reproducible market-data inputs
for normalization, book, replay, risk, OMS, and gateway testing. It contains no
venue connectivity or order-entry behavior. The native wire contract is
[SMX/1](synthetic-mock-protocol.md).

## Configuration model

`GeneratorConfig` uses fixed-width integer values and a bounded instrument
array. It configures:

- explicit seed and logical event count;
- multiple venue/channel and instrument identities;
- per-instrument tick value, initial mid-price, and order-level or price-level
  source mode;
- session start/end in exchange-event UTC nanoseconds;
- normal and burst message rates;
- volatility in integer parts per million;
- tight, normal, and wide spread widths plus regime rotation interval;
- fixed, uniform, or two-point order-size distributions;
- cancellation and market-trade intensity in parts per million;
- auction start/end ordinals;
- stale threshold and one declared scenario; and
- fixed maximum active native orders per order-level instrument.

Invalid counts, identifiers, intensities, timestamp ranges, sizes, rates,
spreads, auction ranges, or unrepresentable timing fail configuration before
generation. The CLI supplies conservative synthetic defaults; none are
production trading parameters.

## Deterministic core

The core uses the published SplitMix64 recurrence and project-owned bounded
integer samplers. No implementation-defined standard distribution participates
in output. Instrument selection is round-robin so configured instruments cannot
starve. Random draws determine side, size, price movement, and event family.

Generator construction allocates one bounded state block per instrument. A
successful `next()` performs bounded array work and writes one caller-owned
`SyntheticEvent`. It never allocates, waits, reads a system clock, accesses a
file or socket, or calls an external service. `event_count`, stable event hashes,
configuration hash, per-channel sequences, and final-book hash are deterministic
functions of normalized configuration and seed.

Order-level instruments keep a bounded active-order array. Cancellation selects
an active order by deterministic index and swap-removes it. Price levels are
bounded to the documented maximum depth; reaching capacity returns an explicit
generation error rather than allocating or silently evicting state.

## Scenarios

The injection ordinal is deterministic and recorded by configuration. Scenario
effects are:

| Scenario | Logical effect | Raw transport effect |
| --- | --- | --- |
| Normal | ordinary regime rotation | none |
| High-rate burst | burst-rate timestamp interval and packet flag | none |
| Crossed-book fault | update creates bid greater than best ask and marks data invalid | none |
| Duplicate packet | none | one exact packet repeated |
| Missing sequence | none | one packet omitted without renumbering |
| Out-of-order packet | none | adjacent packets emitted in reverse order |
| Stale feed | one interval exceeds stale threshold | none |
| Trading halt | `HALTED`; no later mutation | none |
| Reopening auction | `HALTED`, `PRE_OPEN`, `AUCTION`, imbalance, then `OPEN` | none |
| Earnings shock | one-instrument price displacement and elevated size | none |
| Macro shock | correlated signed displacement when instruments next emit | none |
| Index rebalance | large deterministic trade print | none |
| Hidden replenishment | depletion followed by same-price replenishment flagged synthetic | none |

Configured auction ordinals operate independently in the normal scenario.
Scenario flags describe test provenance; they never imply real market behavior.

## Outputs and tools

`synth-exchange-generate` writes three bounded streaming artifacts:

- `.smxcap`: fault-injected raw SMX/1 capture;
- `.amae`: ordered canonical `MarketEvent` audit envelopes; and
- `.book.txt`: human-inspectable expected logical final-book state and hash.

`synth-exchange-stream` replays raw packets to stdout or a file at an integer
acceleration factor. Zero means unthrottled. It uses monotonic sleeping only for
optional pacing; packet bytes never depend on pacing.

`synth-exchange-verify` structurally verifies every capture packet, stable hash,
sequence transition, scenario expectation, and final book when transport state
remains valid. `synth-exchange-inspect` renders a bounded number of decoded
records and never treats payload bytes as text.

## Performance boundary

The benchmark target measures generator `next()` separately from filesystem and
canonical FlatBuffer serialization. It records event throughput, per-event CPU
time, generated/error counts, and stable hash output under a fixed seed.
Filesystem throughput is reported separately by CLI runs and is not presented
as hot-core throughput. No several-million-events-per-second claim is accepted
unless demonstrated on the executing hardware by retained release output.

## Limitations and next integration

SMX/1 is not evidence for a real venue's packet, order-priority, auction,
correction, trade-bust, snapshot, or recovery behavior. A production decoder
remains blocked by the
[licensed integration boundary](licensed-integration-boundaries.md).

The next internal dependency is the deterministic order-book component that
consumes normalized `MarketEvent` book updates, enforces active/shadow recovery,
and proves canonical replay against these final-book oracles.
