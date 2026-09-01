# ADR 0018: Assumption-labelled options analytics

- Status: Accepted
- Date: 2026-08-30
- Owners: intelligence, model-contracts, research, compliance

## Context

Options quotes, trades, and open interest are observations; dealer inventory is
not. Assigning a dealer sign to open interest without an explicit assumption
turns an inference into a false fact. Implied-volatility inversion is also
numerically unsafe if an unbounded Newton iteration is applied to stale, wide,
crossed, expired, or arbitrage-inconsistent quotes. Equity-style American
exercise and adjusted deliverables cannot be priced correctly by silently using
a European closed form.

The common `ModelForecast` is intentionally domain-neutral. Contract reference
state, quote diagnostics, surface points, Greeks, and sign assumptions remain in
a detailed off-path artifact.

## Decision

1. The subsystem is an asynchronous Python signal service outside the execution
   hot path. It exposes no OMS, risk, gateway, authorization, or order method.
2. Detailed inputs and results use immutable schema `1.0.0` dataclasses,
   canonical sorted JSON, SHA-256 identities, integer currency nanos, contract
   counts, and PPM rates, volatility, probabilities, confidence, and OOD.
3. The parser accepts only the public 21-character OSI representation. Provider
   message framing, symbol mapping, reference data, corrections, and entitlements
   require authorized specifications and adapters.
4. Contract-master revisions are immutable. Reference version, effective time,
   and corporate-action revision must increase; the OSI symbol is checked
   against root, expiration date, right, and strike.
5. The first pricing slice is Black–Scholes–Merton with continuous rates and
   dividends for `EUROPEAN` exercise only. `AMERICAN` exercise is retained in
   reference data but is not approximated; its quote is classified unsupported.
6. The implied-volatility solver uses a finite configured bracket and bounded
   bisection. It checks discounted European arbitrage bounds before inversion.
   Unbracketed, zero-time-value, nonfinite, or nonconverged inputs produce
   numerical failure and prevent publication.
7. Delta, gamma, and vega use analytic European formulas. Vanna and charm are
   bounded symmetric differences around analytic delta, quantized immediately.
   Their exposure outputs are explicitly approximate.
8. The surface builder sorts deterministically and checks quote-level bounds,
   strike monotonicity/convexity, and same-strike total-variance calendar order.
   Static-arbitrage failures make the surface invalid.
9. Open interest is observed but dealer side is an assumption. Results always
   include `UNKNOWN_SYMMETRIC`, `DEALER_SHORT_CUSTOMER_POSITION`, or
   `DEALER_LONG_CUSTOMER_POSITION`, an uncertainty score, gross exposures,
   assumption-signed exposures, and a pressure distribution. The safe default
   is symmetric with maximum assumption uncertainty.
10. Only a sufficient, valid surface publishes expected volatility through the
    canonical `ModelForecast` with `REALIZED_VOLATILITY` and `VOLATILITY_PPM`.
    The detailed artifact retains skew, term structure, unusual-volume ratio,
    zero-DTE concentration, implied move, inferred pinning/hedging pressure,
    confidence, OOD, and reason codes.

## Consequences

- Quote and contract corruption cannot reach a solver as apparently healthy
  data.
- Dealer-pressure outputs cannot be represented without their assumption and
  uncertainty.
- The initial formulas validate infrastructure and numerical behavior; they do
  not establish predictive value or profitability.
- American exercise, discrete dividends, complex adjusted deliverables, and
  production provider semantics remain explicit dependencies.

## Rollback

Disable options forecast publication and retain content-addressed assessment and
abstention artifacts. No common schema or order-path rollback is needed.

