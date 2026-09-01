# ADR 0012: Versioned fixed-point model forecasts

- Status: Accepted
- Date: 2026-08-29
- Owners: model-contracts, feature-engine, ensemble

## Context

The canonical v1.0 `ModelForecast` carried one integer forecast value and a
confidence score. Phase III needs enough provenance and distribution metadata
to compare heterogeneous models without allowing a model to become an order
source. It also needs local and asynchronous runners with deterministic
deadlines, immediate disabling, and bounded failure behavior.

Binary floating point is unsuitable for the canonical boundary: NaN and Inf
have unsafe propagation behavior, and replay may differ across implementations.
Calling a registry or remote model service while processing a forecast would
also make the edge depend on an unbounded external operation.

## Decision

1. Schema v1.1 appends distribution, score, provenance-time, and optional
   transaction-cost fields to `ModelForecast`. Expected returns, quantiles,
   probabilities, volatility, scores, and costs use integer parts per million.
   `RETURN_PPM` is a reader-first enum addition. The original `forecast_value`
   remains populated with `expected_return_ppm` for v1 readers.
2. `IForecastModel` accepts an immutable `FeatureSnapshot` and can only return a
   `ModelPrediction`. It has no dependency on risk, OMS, execution, gateways, or
   an order type.
3. A `LocalModelRunner` owns provenance, production/expiry timestamps,
   validation, and an atomic versioned health state. A disable generation takes
   effect on the next runner admission. Stale control generations are rejected.
4. `AsynchronousModelRunner` uses two preallocated SPSC queues. Submission and
   polling do not wait. Work completing after its deadline, or polled after its
   expiry, is discarded with an explicit status and counter.
5. Default fallback is fail-closed. A fallback is usable only when an operator
   configures a separate `IForecastModel`; its own model ID and version appear in
   the output. Invalid inputs and invalid model output do not trigger fallback.
6. Registry access is a control-plane interface. Metadata is resolved before
   constructing a runner; forecasting never calls the registry.
7. External floating-point outputs are rejected on NaN/Inf or range error, then
   quantized at the adapter boundary. Canonical and hot-path structures contain
   integer values only.

## Schema migration

Readers that understand v1.1 must be deployed before v1.1 writers. A v1.0
FlatBuffers reader can ignore the appended fields, but operational v1.0 readers
that reject the new `RETURN_PPM` enum must not consume v1.1 forecast traffic.
Rollback disables v1.1 publishers before restoring v1.0 readers. Golden files
for data quality and model forecasts bind the exact v1.1 bytes in C++ and
Python.

## Consequences

- Forecast replay is independent of floating-point sentinel behavior and binds
  source snapshot, model/configuration versions, timestamps, and distribution
  metadata.
- Model service loss and queue saturation become data availability failures,
  never waits in risk or OMS.
- The async runner's destructor joins its worker and is therefore a control-plane
  shutdown operation, not a hot-path operation.
- Platt scaling remains an offline adapter utility using finite doubles. Its
  result is immediately rounded to integer PPM. Logistic baseline inference uses
  a fixed integer lookup table to preserve replay determinism.
- The framework intentionally provides no ensemble weighting, strategy,
  pre-trade risk, order, gateway, or live-transmission behavior.

## Rejected alternatives

- Direct model-to-order messages were rejected because they bypass ensemble and
  deterministic pre-trade risk.
- Treating the primary model's zero output as an implicit fallback was rejected
  because audit records could not distinguish outage from genuine prediction.
- Blocking on remote inference or registry lookup was rejected because failure
  latency would escape its bound and could propagate into risk or OMS.
- Adding `double` fields to FlatBuffers was rejected because non-finite values
  and cross-platform rounding would weaken validation and replay.
