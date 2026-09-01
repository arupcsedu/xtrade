# ADR 0019: Provenance-bound market-specialist inference

- Status: Accepted
- Date: 2026-08-30
- Owners: intelligence, model-contracts, research, compliance

## Context

Index-rebalance flow, auction allocation, and hidden liquidity are not directly
observable in full. Provider announcements do not reveal how every tracker will
trade. An imbalance feed does not establish an order's allocation priority.
Repeated displayed-depth replenishment does not prove an iceberg or reveal its
remaining size. Presenting these quantities as facts would create unsafe model
provenance and false precision.

The calculations are near-real-time advisory intelligence. They must be
replayable, must fail closed on stale or poor-quality inputs, and cannot acquire
an order-entry dependency.

## Decision

1. The three specialists are pure, deterministic Python evaluators outside the
   execution hot path. They expose no risk, OMS, gateway, or order-submission
   method.
2. Input and detailed-result schema `1.0.0` uses immutable dataclasses,
   canonical sorted JSON, SHA-256 content identities, integer currency nanos,
   integer price ticks and quantities, and integer PPM ratios and probabilities.
3. Every normalized input names a `source_id` that must resolve to exactly one
   content-addressed provenance record available at the evaluation cutoff.
   Feature snapshot, configuration version, session, timestamp domains, data
   quality, and symbol policy are part of the input digest.
4. Index passive flow is an estimate produced under either a bounded unknown-
   tracker assumption or an explicit full-weight-delta assumption. Output is a
   range with timing, auction demand, impact quantiles, reversal probability,
   uncertainty, and reason codes.
5. Auction allocation is explicitly `QUEUE_PRIORITY_UNKNOWN` or a configured
   `PRO_RATA_APPROXIMATION`. A recommended participation cap is advisory and
   conservative; it is neither an order nor an authorization.
6. Hidden liquidity is always labelled
   `REPLENISHMENT_IS_INFERENTIAL`. Displayed depth, executions, replenishment,
   partial-fill sequences, venue priors, and price impacts can support an
   iceberg probability and latent-size range, but never an observed-hidden-size
   field.
7. A versioned immutable symbol policy can disable any instrument. Disablement,
   low quality, unauthenticated provenance, stale state, or insufficient
   evidence produces a content-addressed abstention with no common forecast.
8. Published estimates also emit a canonical `ModelForecast` tied to the source
   feature snapshot and configuration. Expiration uses process monotonic time.
   The detailed result retains specialist-only ranges, assumptions, and reasons.

## Consequences

- Replay can verify the entire detailed result and common forecast by digest.
- Outputs cannot silently shed their assumptions or provenance.
- Fixed-point arithmetic is deterministic and does not claim economic accuracy
  or profitability.
- The pure library is not itself a deployable service. A future host service
  must implement the engineering-contract health, readiness, configuration,
  metrics, logging, and graceful-shutdown contract.
- Production provider semantics, entitlements, venue auction rules, and
  validated calibration data remain external dependencies.

## Rollback

Disable specialist publication by symbol policy or remove the specialists from
their future host service. Existing immutable result records remain replayable;
no order-path or common-schema rollback is required.
