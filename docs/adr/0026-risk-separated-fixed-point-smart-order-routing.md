# ADR 0026: Risk-separated fixed-point smart order routing

- Status: Accepted
- Date: 2026-08-31
- Owners: execution, risk, OMS, gateways, replay

## Context

Strategies need to express execution objectives and choose among synthetic/paper
venues without producing venue-native commands or weakening the existing
pre-trade risk, OMS, and final gateway boundaries. Venue observations contain a
mixture of hard safety facts and uncertain estimates. Floating-point scoring,
unbounded schedules, implicit retry, or direct gateway callbacks would make the
result difficult to reproduce and could create a risk bypass.

## Decision

`cpp/execution` owns a fixed-layout `ExecutionObjective`, deterministic execution
policy slicer, `SmartOrderRouter`, routing explanation, and execution-cost
attribution contract. An objective contains instrument, side, total quantity,
limit, schedule, forecast/feature provenance, and integer alpha assumptions. It
contains no venue identity, gateway command, or risk approval.

The router emits only a `RoutingDecision`. That decision names a proposed venue,
price, quantity, action, and time-in-force and always states that a fresh exact
pre-trade approval is required. It is not convertible to `GatewayRequest` or
`oms::GatewayCommand`. Composition code must create a new immutable
venue-specific `RiskIntent`, pass deterministic risk, enter the OMS state
machine, and then pass the gateway's independent final predicate.

Hard eligibility is evaluated before economic score. Disabled or policy-
incompatible venues, regulatory denial, unknown/conflicting self-trade state,
halt/closed state, invalid or stale quotes, excessive latency, a disagreement
between a consolidated best-venue claim and that venue's direct quote, reject
bursts, inadequate fill probability, exhausted venue concentration, working-
order mismatch, and already visited venues receive zero eligibility. Unknown is
unsafe. No score can override a hard exclusion.

All execution arithmetic uses integer ticks, integer units, currency nanos,
microticks, nanoseconds, and parts per million. Alpha uses a deterministic
piecewise half-life interpolation. Fill uncertainty uses bounded Bernoulli
dispersion. Passive and aggressive expected values include price, displayed and
estimated hidden quantity, queue ahead, fill probability, latency-decayed alpha,
signed fees/rebates, adverse selection, reject probability, opportunity cost,
and uncertainty penalty. Equal scores break by configured rank and then the
canonical 128-bit venue identifier.

Participation, TWAP, VWAP, POV, and auction slices are bounded by remaining
objective quantity, objective child cap, venue child cap, venue concentration,
and policy-specific observed volume. Auction participation is additionally
bounded by an explicitly inferred paired-quantity fraction. Route attempts and
visited venues are explicit request state; the router never retries or calls a
gateway.

The router is single-owner and allocation-free in steady-state. Configuration,
observations, objectives, requests, decisions, explanations, and cost
attribution use stable hashes. Metrics are fixed-cardinality. Durable routing
journal integration remains owned by Phase 10 replay work.

## Consequences

- Strategy intent cannot address a venue and a router decision cannot transmit.
- Every routed child incurs a fresh deterministic risk evaluation and normal OMS
  lifecycle before paper/synthetic gateway handling.
- Hard safety, regulatory, self-trade, concentration, freshness, and loop rules
  remain interpretable and testable independently of score weights.
- Fixed-point rounding is deterministic but is an approximation; tie behavior
  and rounding direction are contract behavior.
- Hidden liquidity, queue position, fill probability, adverse selection, alpha,
  and cost estimates remain uncertain inputs. The router records them but does
  not represent them as observed facts or evidence of economic value.
- Real venue eligibility, fee, order type, auction, self-trade, and routing rules
  remain blocked on authorized specifications and compliance configuration.

## Rejected alternatives

- A router callback accepting `IExchangeGateway` would make risk/OMS bypass
  possible by construction.
- Floating-point scores would violate execution-path numeric rules and make
  cross-build replay less stable.
- Selecting the highest score before safety filtering would permit an economic
  estimate to override a halt, stale quote, regulatory denial, or STP conflict.
- Unbounded route retries could form loops and amplify venue reject bursts.
- Treating estimated hidden quantity or queue position as certain would overstate
  what market data can establish.
