# Common model-serving and forecast framework

## Boundary and ownership

The framework turns one immutable feature snapshot into a validated, expiring
forecast. It does not make a trading decision and cannot construct an order.

```text
FeatureSnapshot -> IForecastModel -> ModelPrediction
       |                                  |
       +------ Local/Async runner --------+
                         |
              validated ModelForecast
                         |
               ForecastCache / ensemble
```

`IForecastModel` owns model-specific inference. The runner owns model identity,
feature provenance, time, expiry, health generation, validation, and fallback
attribution. The future ensemble may read validated forecasts, but no model or
runner includes risk, OMS, execution, or gateway headers.

## Components

| Component | Timing class | Responsibility |
| --- | --- | --- |
| `IForecastModel` | near-real-time | allocation-free prediction from one immutable snapshot |
| `ModelMetadata` / `FeatureRequirement` | control + hot read | version, instrument, required features, bounds, freshness, horizon, TTL |
| `ModelHealthState` | hot override | atomic generation-ordered enable, degrade, disable, or failure state |
| `ModelDeadline` | near-real-time | monotonic admission and completion deadline |
| `ModelForecastValidator` | near-real-time | fail-closed provenance, range, probability, timestamp, expiry, cost, and hash checks |
| `ModelRegistryClient` | asynchronous control | metadata/health lookup outside forecasting |
| `LocalModelRunner` | near-real-time | synchronous local inference with injected clock and no registry call |
| `AsynchronousModelRunner` | near-real-time boundary | bounded request/completion queues; non-blocking submit and poll |
| `ForecastCache` | hot owner thread | fixed-capacity validated latest forecast per model/instrument |
| `ForecastExpiryManager` | hot owner thread | bounded expiry sweep and count |
| calibration utilities | offline/adapter | isotonic, Platt, and quantile-coverage evaluation |

The process-local `aegis::models::ForecastCache` is single-owner state. The
separate process-shared cache in `aegis::event_bus` has an ABI-stable subset for
colocated processes. Persisted/network forecasts always use canonical
FlatBuffers `ModelForecast` v1.2. v1.2 preserves v1.1 fields and appends distinct
slippage and adverse-selection cost estimates.

The additive v1.3 contract extends that return-specific framework for off-path
time-series forecasts. It retains every v1.2 field and adds explicit target,
unit, and generic integer distribution fields. Existing local return models
remain valid; non-return publishers require a v1.3 reader. See the
[time-series forecast service](timeseries-forecast-service.md).

## Forecast contract

Every output binds:

- forecast, session, model, model version, instrument, feature snapshot, and
  configuration identifiers;
- model-control generation, horizon, exchange as-of time, monotonic production
  time, and monotonic expiration time;
- expected return and p10/p50/p90 return quantiles in PPM;
- down/flat/up probabilities whose integer sum is exactly 1,000,000 PPM;
- volatility, confidence, calibration, data-quality, and OOD scores;
- an explicit presence bit and integer fee, spread, slippage, market-impact, and
  adverse-selection estimates;
- a stable hash over every semantic in-memory field; and
- SHA-256 over the serialized payload when placed in an `AuditEnvelope`.

The canonical schema calls production time
`created_process_monotonic_time` and expiration time
`valid_until_process_monotonic_time`. They are never wall-clock timestamps.

## Admission, deadline, and failure semantics

The runner first samples its injected monotonic clock and validates metadata,
the required feature-definition version, snapshot identity and stable hash,
source event/ordinal range, snapshot state, feature validity, feature ranges,
freshness, and deadline. It samples the clock again after inference. A
completion strictly later than `complete_by_process_monotonic_time_ns` is
discarded. An OOD score above the versioned metadata threshold is invalid. A
cache read strictly later than expiration is rejected and removes the entry.

Default behavior is `fail_closed`. With
`explicit_model_on_failure`, only a prediction-status failure may invoke the
configured fallback. The fallback output is stamped with the fallback model's
ID/version. Missing, stale, malformed, late, disabled, or invalid forecasts are
not disguised as fallback forecasts.

Async queue saturation, no result, stopped runner, deadline miss, late poll,
model failure, fallback failure, and validator failure all have separate
statuses. Submit and poll never wait. Worker shutdown may join and belongs only
to lifecycle control.

## Health and disabling

Runner health is one atomically published word containing a 56-bit generation
and health code. Updates with generation zero, unknown state, invalid state, or
a generation not newer than the current state fail closed. `disabled` and
`failed` reject new inference immediately. Cache invalidation is an explicit
owner-thread operation so a disable handler can remove already published
forecasts before acknowledging the control update.

## Baselines and calibration

Zero-return, last-value, moving-average, linear-regression, logistic-regression,
and seasonal-naive models implement the same interface. Stateful instances are
bound to one instrument by metadata. Moving-average warm-up and seasonal-bin
warm-up return explicit non-success prediction states. Linear arithmetic is
bounded; logistic inference uses a fixed PPM lookup/interpolation table.

Isotonic calibration uses fixed monotonic PPM breakpoints. Platt scaling is an
offline/adaptor function that rejects non-finite doubles before producing PPM.
Quantile coverage uses bounded integer counters and reports PPM coverage.

See [ADR 0012](../adr/0012-versioned-fixed-point-model-forecasts.md), the
[event contracts](event-contracts.md), and the [testing strategy](../testing/model-framework-testing.md).

The native microstructure artifact and seven initial estimators are documented
in the [microstructure model suite](microstructure-model-suite.md) and
[ADR 0013](../adr/0013-native-microstructure-model-artifacts.md).
