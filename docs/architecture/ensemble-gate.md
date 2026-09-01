# Uncertainty-aware mixture-of-experts gate

## Boundary and ownership

`cpp/ensemble` is the fixed-capacity C++ decision boundary between validated
model forecasts and the future intent/risk path. It consumes immutable model,
market-state, event-state, feed-quality, and cost-forecast snapshots. It emits
one versioned `EnsembleForecast`; it has no gateway, OMS, risk, network, disk,
Python, or model-service dependency and cannot submit an order.

The evaluation path is:

```text
request/config validation
  -> cost forecast validation
  -> global market/feed exclusion
  -> per-expert forecast/provenance/health/state hard mask
  -> canonical duplicate exclusion
  -> rule, linear, or quantized learned score
  -> capped PPM normalization
  -> distribution, disagreement, and uncertainty combination
  -> strict robust-edge test
  -> hashed forecast and explanation
```

All arrays are preallocated for 16 experts. Evaluation performs bounded loops,
uses integer fixed-point arithmetic, and makes no system-clock call. The caller
supplies the monotonic evaluation time and an integrity-checked authoritative
market-state snapshot.

## Hard eligibility

Hard exclusions always produce zero weight and a stable reason. The learned
gate receives only entries that passed this same mask.

| Input condition | Eligibility result |
| --- | --- |
| Missing IDs, feature snapshot, or forecast hash | `MISSING_PROVENANCE` |
| Expired or older than configured maximum age | `EXPIRED` |
| Invalid forecast ranges/hash | `INVALID_FORECAST` |
| Disabled, failed, warming, or unknown model | `MODEL_DISABLED` |
| Health generation differs from forecast | `CONTROL_GENERATION_MISMATCH` |
| Session, instrument, horizon, or configuration differs | `SCOPE_MISMATCH` |
| OOD exceeds configured threshold | `EXCESSIVE_OOD` |
| Calibration failed/unknown or score below minimum | `CALIBRATION_FAILED` |
| Expert or input data quality below minimum | `DATA_QUALITY_FAILED` |
| Role is disabled for market/event state | `INCOMPATIBLE_STATE` |
| Duplicate model or forecast identity | `DUPLICATE_EXPERT` |

Unknown, recovering, stale, or invalid feed health excludes every expert.
Degraded feed health is eligible only through an explicit configured
multiplier. Unsafe authoritative market states exclude every expert. Event
states such as breaking news may still produce a forecast, but this does not
mean that risk or order entry is permitted.

## Gates and normalization

The rule baseline averages confidence, calibration, data quality, inverse OOD,
and freshness before applying role and degradation multipliers. The linear
gate applies signed fixed-point coefficients to the same features. The learned
gate uses a versioned quantized linear artifact plus role, market-state, and
event-state biases. A configured TimesFM/timeseries breaking-news multiplier
allows its slow-context forecast to be downweighted without changing its
provenance or silently disabling it.

Forecast and market-state freshness thresholds are positive nanosecond
durations capped at `UINT64_MAX / 1,000,000` (about 5.12 hours). This bound
keeps the exact parts-per-million freshness calculation within unsigned
64-bit arithmetic; configurations outside the bound fail validation.

Positive scores are normalized to one million PPM with capped water filling.
Experts that would exceed the cap are fixed at it, and the remainder is
renormalized over uncapped experts. Integer residue is assigned in canonical
order. An infeasible cap or all nonpositive scores abstains rather than
relaxing a limit.

## Distribution and abstention

The point forecast, directional probabilities, and p10/p50/p90 values are
weighted fixed-point approximations. Probabilities are forced to an exact PPM
sum by assigning the residual to flat probability. Mixture variance is:

```text
sum_i weight_i * (expert_volatility_i^2
                  + (expert_return_i - combined_return)^2)
```

Disagreement is the integer square root of only the between-expert term.
Effective uncertainty is the square root of mixture variance plus configured
calibration, OOD, and data-quality penalties. No NaN or infinity can enter
because the contract and calculation use bounded integers.

The result abstains unless:

```text
abs(expected_return) > estimated_transaction_cost
                       + uncertainty_penalty
                       + safety_margin
```

The transaction-cost forecast is independently validated for scope,
expiration, model health/control generation, stable hash, and the presence of
all five cost components. Missing or invalid cost always abstains.

## Audit and schema

`EnsembleDecisionExplanation` records every supplied expert in canonical order,
including exclusions and zero weights. It also binds the market-state snapshot,
configuration, cost forecast, optional learned artifact signature, and source
forecast hashes. Stable in-memory hashes cover every decision field.

`build_ensemble_forecast_contract` is an allocating journal/network boundary,
not part of gate evaluation. It serializes schema v1.5 and the common audit
validator performs zero-copy structural, digest, enum, range, weight-sum,
contributor, and robust-edge relationship checks. An all-invalid abstention has
an explicitly present empty contributor vector rather than fabricated input.

See [ADR 0021](../adr/0021-hard-masked-fixed-point-mixture-of-experts.md), the
[event contracts](event-contracts.md), and the [test guide](../testing/ensemble-gate-testing.md).
