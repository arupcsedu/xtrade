# ADR 0021: Hard-masked fixed-point mixture of experts

- Status: Accepted
- Date: 2026-08-30
- Owners: ensemble, model-contracts, edge-core, risk

## Context

Aegis-MX receives forecasts with different horizons, provenance, calibration,
freshness, event suitability, and failure modes. A learned score alone is not a
safety boundary: a large coefficient or corrupt artifact must never restore an
expired, disabled, out-of-distribution, uncalibrated, provenance-free, or
state-incompatible forecast. The colocated decision path also cannot allocate,
wait for a model, invoke Python, call a service, or depend on floating point.

Mixture weights and distribution arithmetic must be deterministic across
online and replay runs. Per-expert caps must be exact, including the otherwise
easy-to-miss case where the requested cap makes a unit-sum allocation
impossible. The ensemble remains advisory and cannot create or transmit an
order.

## Decision

1. `MixtureOfExpertsGate` accepts at most 16 fixed-layout expert inputs. It
   canonicalizes them by model ID, model version, and forecast ID. Repeated
   model or forecast identities are all excluded so duplicate publication
   cannot evade an expert cap.
2. Hard eligibility executes before every score. It excludes missing
   provenance, expired or structurally invalid forecasts, disabled/failed
   model health, control-generation mismatch, scope mismatch, excessive OOD,
   failed or unknown calibration health, insufficient data quality, invalid
   feed state, and role/state incompatibility. `STARTUP`, `DATA_DEGRADED`,
   `HALTED`, `REOPENING`, `RECOVERY`, and `SHUTDOWN` globally exclude experts.
3. Three score policies share that mask: an interpretable rule baseline, a
   configurable signed linear gate, and an optional signed quantized learned
   artifact. The learned artifact binds a model ID, version, signature hash,
   coefficients, state/role/event biases, and stable content hash. It cannot
   change eligibility.
4. Scores are nonnegative integers after masking and bounded adjustment.
   Weights use PPM and deterministic capped water filling. They sum to exactly
   `1,000,000` when any expert contributes. Remainder units go in canonical
   order. If `eligible_positive_count * cap < 1,000,000`, the result abstains
   with `WEIGHT_CAP_INFEASIBLE`.
5. The combined mean, directional probabilities, and quantiles are weighted
   fixed-point approximations. Mixture variance is the weighted sum of expert
   variance and squared distance from the mixture mean. Disagreement is the
   square root of the between-expert term. Effective uncertainty adds bounded
   calibration, OOD, and data-quality penalties.
6. A separate valid transaction-cost forecast is mandatory and includes fees,
   spread, slippage, impact, and adverse selection. The ensemble acts only when
   `abs(expected_return) > transaction_cost + uncertainty_penalty +
   safety_margin`. Equality abstains. The signed `net_robust_edge` subtracts
   that threshold in the forecast direction; it is explanatory and grants no
   trading authority.
7. Every result, including all-invalid and unsafe-state results, carries an
   explanation with canonical zero-weight exclusions, raw scores, weights,
   freshness, source forecast hashes, gate/artifact identity, market-state
   hash, configuration hash, cost-forecast hash, dominant-expert reason, and
   stable hashes.
8. Schema v1.5 appends the fixed-point ensemble distribution, weights,
   abstention, reasons, explanation, and hashes. Writers deploy only after v1.5
   readers and semantic validators. Rollback stops v1.5 writers first.

## Consequences

- Rule, linear, and learned gates have identical safety behavior for excluded
  inputs.
- Canonical rounding can assign one PPM more to an earlier expert in an exact
  tie; this is documented, replayable, and visible in the explanation.
- Quantile combination is an approximation rather than a full mixture CDF.
  The variance and disagreement terms preserve important dispersion that a
  weighted quantile average alone would lose.
- The gate publishes only `EnsembleForecast`. Intent translation and all
  deterministic risk/OMS work remain separate dependencies.

## Rollback

Disable the ensemble publisher and make downstream consumers abstain. Do not
fall back to a model output or older local weighting rule. Existing v1.5
records remain readable and replayable. Stop v1.5 writers before rolling back a
reader that does not enforce the v1.5 semantic checks.
