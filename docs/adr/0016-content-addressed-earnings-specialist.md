# ADR 0016: Content-addressed earnings specialist

- Status: Accepted
- Date: 2026-08-30
- Owners: intelligence, model-contracts, research, compliance

## Context

An earnings event combines facts from documents with point-in-time estimates,
prior reports, options, historical reactions, transcripts, and market features.
These values have incompatible accounting bases, units, periods, availability
times, and licensing constraints. Flattening them into generic news facts would
lose comparability and leakage controls. Adding earnings-only vectors to the
generic `ModelForecast` would also make the common model contract ambiguous.

## Decision

1. The specialist runs in Python outside the execution hot path. It has no OMS,
   risk, strategy, execution, gateway, or operator-authority interface.
2. Normalized input and detailed output use immutable schema `1.0.0` dataclasses
   with canonical sorted JSON and SHA-256 identities. The detailed artifact is
   retained by the future intelligence object/journal store; it is not a hot-
   path transport contract.
3. Every metric identity includes kind, GAAP/non-GAAP basis, explicit integer
   unit, fiscal period, and optional segment. Surprise and guidance comparisons
   require exact identity equality. Similar but incompatible metrics generate a
   reason code and are never coerced.
4. Source evidence binds an upstream `GlobalEventId`, raw and sanitized content
   hashes, provider/document identity, publication/receipt UTC timestamps, and
   exact sanitized excerpts. The specialist receives no raw document text.
5. Consensus, option, historical, source-receipt, and feature availability are
   checked against the event cutoff. Consensus and option observations received
   after the official release are rejected rather than used.
6. Standardized surprises use integer fixed-point PPM with truncation toward
   zero:

   ```text
   surprise_ppm = (actual - consensus) * 1,000,000
                  / max(dispersion, epsilon_for_unit)
   ```

   Epsilon is configuration-versioned in the same unit as the metric.
7. Detailed output retains surprises, guidance changes, accounting diagnostics,
   materiality, direction probabilities, volatility, gap range, discovery
   duration, uncertainty, and reason codes. The only cross-model publication is
   an existing canonical `ModelForecast` bound to the input hash and feature
   snapshot.
8. `PRE_EARNINGS`, `RELEASE_PROCESSING`, `PRICE_DISCOVERY`, and `RECOVERY` are a
   separate bounded state machine. Hooks approve proposed notifications only;
   they expose no order or transmission method. Corrections append a revision
   linked to the prior input digest and re-enter release processing.
9. The initial scoring rules are deterministic infrastructure baselines. They
   have zero calibration score and make no profitability or predictive-value
   claim.

## Consequences

- Replay can compare exact input, detailed-result, and common-forecast bytes.
- Incompatible accounting values and future estimates fail closed.
- Detailed artifacts remain readable without expanding a safety-critical hot-
  path ABI for every domain-specific diagnostic.
- A durable object/journal store and approved licensed estimate, option,
  transcript, and filing integrations remain external dependencies.
- Any learned replacement must preserve the normalized contract, leakage
  checks, evidence, deadlines, common forecast publication, and lifecycle
  isolation.

## Rollback

Stop earnings forecast publication, retain normalized inputs and detailed
results by digest, and return consumers to the existing generic intelligence
records. No schema v1.4 or `ModelForecast` wire field changes are required.
