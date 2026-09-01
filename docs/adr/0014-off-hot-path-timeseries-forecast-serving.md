# ADR 0014: Off-hot-path time-series forecast serving

- Status: Accepted
- Date: 2026-08-30
- Owners: intelligence, model-contracts, deployment

## Context

Aegis-MX needs multi-horizon forecasts for returns, realized volatility, volume,
spread, and market and sector factors. A TimesFM implementation is a replaceable
external model dependency and may require Python, a GPU, batching, checkpoint
I/O, and network-facing services. None of those operations are permitted in the
colocated decision path. Schema v1.2 also requires every rich forecast to use
`RETURN_PPM`, which cannot truthfully represent volume or spread.

## Decision

1. Time-series serving is owned by `python/model_serving` and deployed only as a
   non-colocated service. It has no risk, OMS, gateway, order, authorization, or
   live-transmission dependency.
2. `ForecastModelAdapter` is the model boundary. A TimesFM backend is injected
   behind that interface; the repository contains a deterministic reference
   adapter and does not claim conformance with an external TimesFM release.
   Absence of an injected backend is explicit and never triggers a download.
3. Zero-shot operation uses no checkpoint. An optional checkpoint is identified
   by an approved SHA-256 digest, never by an unverified mutable name. Model ID,
   version, adapter name, device, checkpoint digest, context digest, and
   configuration hash are retained in publication metadata or service audit
   state.
4. Contexts contain strictly ordered exchange-event timestamps, point-in-time
   availability timestamps, source event IDs, feature snapshot ID, configuration
   version, exact target and unit, and one known-future covariate row per
   horizon. Missing, late, duplicated, or future-leaking data fails closed.
5. The worker uses bounded request/result queues and bounded batches. It rejects
   admission after the monotonic deadline and discards completion after the
   deadline. Forecasts have both a monotonic expiration and a maximum publication
   age; cache reads enforce both.
6. GPU device failure degrades health and permits one deterministic CPU retry of
   the same adapter and immutable context. Other adapter failure uses an
   explicitly configured deterministic baseline or fails closed. Fallback
   identity is published as the producing model identity.
7. Schema v1.3 additively appends `ForecastTarget`, target point/quantile values,
   and explicit volatility, volume, spread, and factor units. Existing v1.2
   return fields retain their meaning. For a non-return target, return fields are
   zero and directional probability is neutral; consumers use the v1.3 target
   fields. Readers deploy before v1.3 non-return writers.
8. Full forecasts cross the service boundary as canonical `ModelForecast`
   records. A bounded asynchronous publisher may copy the validated point value
   into the existing versioned shared-memory forecast cache. The hot path reads
   that cache and never calls these services.
9. Evaluation is walk-forward and point-in-time. Returns, volatility, volume,
   and spread are reported separately against last-value, seasonal-naive,
   ARIMA-compatible, GARCH-compatible, gradient-boosting-compatible, and compact
   temporal baselines. Reports state `economic_value_claim=false`; raw-price-only
   evaluation is not supported.
10. The service uses only pinned repository dependencies. Installing an actual
    TimesFM implementation, GPU runtime, or checkpoint is a separately reviewed
    deployment decision and must update the lock, SBOM, model card, and capacity
    evidence.

## Consequences

- Model delay, GPU loss, worker overload, and service outage reduce forecast
  availability but cannot block or authorize trading.
- Multi-horizon entries are keyed by instrument, model, target, and horizon in
  the service cache. The colocated cache uses distinct model IDs per target and
  includes horizon in its lookup key.
- HTTP/JSON is an operational shell around strictly validated internal types;
  canonical publication remains FlatBuffers. Payloads are bounded and text is
  treated as untrusted data.
- A real TimesFM checkpoint is not bundled, downloaded, or economically
  validated by this phase.

## Rejected alternatives

- Calling TimesFM synchronously from ensemble or risk was rejected because it
  violates the hot-path contract.
- Encoding volume or spread as returns was rejected because it destroys units
  and auditability.
- Automatic checkpoint download was rejected because mutable remote artifacts
  weaken supply-chain integrity and deterministic activation.
- Silent GPU-to-different-model fallback was rejected because output provenance
  would be false.

