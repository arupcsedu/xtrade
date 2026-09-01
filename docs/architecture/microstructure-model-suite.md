# Microstructure model suite

## Scope and safety boundary

This phase implements seven advisory estimators: mid-price direction,
spread-widening, queue depletion, passive fill probability, adverse selection,
transaction cost, and market impact. Every estimator implements
`IForecastModel` and returns `ModelPrediction`. No type in this component can
create or transmit an order.

The implementation is **synthetic infrastructure validation**. Training loss,
calibration metadata, parity, or latency results do not establish predictive
accuracy, profitability, execution quality, or economic value.

## Data flow

```text
SyntheticExchangeGenerator (seeded events)
  -> synth-microstructure-dataset (bounded integer CSV)
  -> Python deterministic trainer
  -> AEGIS_MX_NATIVE_MODEL_V1 + SHA-256
  -> C++ activation/validation
  -> fixed-size NativeModelArtifact
  -> allocation-free NativeMicrostructureModel::predict
  -> common ModelPrediction / ModelForecast v1.2
```

CSV generation and artifact parsing are offline/control-plane operations. Only
the fixed-size artifact and integer input participate in inference.

## Stable native artifact

An artifact is canonical ASCII. The first line is
`AEGIS_MX_NATIVE_MODEL_V1`; the final line is `signature_sha256=<64 lowercase
hex characters>`. SHA-256 covers all preceding bytes, including the final LF
before the signature line. Keys have one defined order. Each feature is encoded
as:

```text
feature=<name>,<center>,<scale>,<training-min>,<training-max>,<weight-ppm>
```

Activation rejects malformed/unknown/duplicate fields, nonpositive scales,
unbounded values, invalid identifiers, bad cost shares, expired artifacts,
unsupported model families, and signature mismatches. The SHA-256 field is a
model signature hash, not a runtime authorization signature.

`logistic_regression` and `linear_regression` are executable in v1.
`gradient_boosted_trees` is a reserved family compatible with the interface but
is rejected until a bounded tree representation and evaluator are accepted.

## Feature ordering

| Role | Ordered inputs |
| --- | --- |
| Mid-price direction | top-level imbalance, order-flow imbalance, signed-trade imbalance, rolling return, realized volatility |
| Spread widening | spread, realized volatility, quote intensity, data quality, session progress |
| Queue depletion | quantity ahead, add rate, cancel rate, execution rate, order age, price distance, venue, session progress |
| Passive fill | queue-depletion inputs, then order quantity |
| Adverse selection | order-flow imbalance, signed-trade imbalance, rolling return, realized volatility, spread, session progress |
| Transaction cost | spread, order quantity, price distance, fee rate, realized volatility, trade intensity |
| Market impact | order quantity, price distance, trade intensity, realized volatility |

Feature names and order are serialized and included in SHA-256. Snapshot values
retain their `FeatureSnapshot` provenance. Order-specific values carry an
explicit `MicrostructureContext.present` bit; missing context is invalid.

## Fixed arithmetic, calibration, and OOD

Normalization is `(raw - center) * 1,000,000 / scale`, with C++ signed division
semantics and bounded integer operands. Linear terms and coefficients use PPM.
Classification uses fixed Platt parameters followed by a 17-point sigmoid table
with integer interpolation. Python implements the same operations.

OOD is the maximum per-feature distance beyond its training min/max, divided by
training span plus distance, in PPM. A score above the artifact threshold is
rejected. Data inside every training bound has OOD score zero. This detector is
intentionally interpretable; richer distribution detectors require a versioned
artifact change.

## Expiry and failure behavior

The activation loader accepts injected wall-clock UTC and monotonic-expiration
values. It rejects already expired artifacts. Inference uses only the request's
monotonic submission time and returns `unavailable` after activation expiry.
Model response TTL and async deadlines remain enforced by the common runner.

Invalid feature state, missing context, arithmetic overflow, OOD rejection, and
expiry return explicit non-success statuses. They do not invoke an implicit
fallback and never block risk or OMS.

## Cost output

Schema v1.2 and `TransactionCostEstimate` preserve separate integer PPM values
for fees, spread, slippage, market impact, and adverse selection. A transaction-
cost artifact predicts a bounded total and applies versioned shares learned or
configured offline. The initial infrastructure model uses explicit fixed shares;
production estimation and venue fee schedules require validated data and model
governance.

See [ADR 0013](../adr/0013-native-microstructure-model-artifacts.md) and the
[test plan](../testing/microstructure-model-testing.md).
