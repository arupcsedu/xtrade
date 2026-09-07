# Time-series forecasting service

## Boundary

The time-series forecasting suite is a non-colocated, off-hot-path component.
It produces advisory forecasts only. It cannot create an order, invoke risk,
address a gateway, change trading mode, or extend readiness. The colocated path
continues using its versioned shared-memory forecast cache and never waits for
an RPC.

```text
point-in-time series + known-future covariates
                    |
          timeseries-context-builder
                    |
          immutable context + SHA-256
                    |
           timeseries-forecast-worker
       bounded batch / deadline / fallback
                    |
       canonical ModelForecast v1.3 records
                    |
     timeseries-forecast-api + forecast cache
                    |
        asynchronous shared-cache publisher
                    |
           nonblocking colocated readers
```

The three installable executable roles share one validated core:

- `timeseries-context-builder` validates and canonicalizes point-in-time input;
- `timeseries-forecast-worker` performs bounded batched adapter inference and
  canonical publication;
- `timeseries-forecast-api` exposes bounded forecast submission/cache reads and
  the common service endpoints.

The repository runtime is a self-contained reference composition: both API and
worker roles accept a canonical context at `POST /v1/forecast` and execute the
bounded local worker; the context-builder role accepts it at `POST /v1/context`
and returns its canonical bytes and digest. This avoids inventing an unapproved
distributed transport. A production split between API, queue, workers, and
cache publisher requires its own durable transport decision; it must not turn
the colocated reader into an RPC client.

## Context and provenance

One context covers one instrument, target, feature snapshot, and as-of time.
Every history sample declares exchange event time, wall-clock UTC availability
time, and an integer value in the target's unit. Event times are strictly
increasing and no later than the context as-of time. Availability is no later
than the point-in-time cutoff. The context binds source-first/source-last global
event IDs, session, configuration, feature snapshot, feature-definition version,
and source payload SHA-256.

Known-future covariates contain an exact horizon, future exchange timestamp,
time-of-day second, session state, scheduled-event bit flags, and the wall-clock
time when the covariate became known. There must be exactly one row for every
requested horizon. Unknown session state, duplicate horizon, missing row, or a
scheduled flag learned after the point-in-time cutoff rejects the context.

The canonical context digest uses sorted-key ASCII JSON with explicit domain
names. It excludes host names, wall-clock sampling, and dictionary iteration
order.

## Adapter, batching, and fallback

`ForecastModelAdapter` accepts a bounded batch of immutable contexts, ordered
horizons, three PPM quantiles, device, and optional checkpoint digest. It returns
integer point/quantile values, confidence, calibration, data-quality, and OOD
scores. `TimesFmBackend` is an injection protocol for an approved external
implementation. The repository reference adapter is deterministic infrastructure
validation and is not represented as a Google TimesFM checkpoint.

The worker has fixed request/result capacities and a configured maximum batch.
It never retries queue saturation. GPU device failure permits one CPU invocation
of the same adapter. If that fails, an explicit last-value or seasonal-naive
fallback may run. Each completion is checked against the request's injected
monotonic deadline before publication. Results that complete late are discarded,
not cached.

## Targets and units

| Target | Canonical unit | Range |
| --- | --- | --- |
| Return | `RETURN_PPM` | signed, ±10,000,000 PPM |
| Realized volatility | `VOLATILITY_PPM` | 0 to 10,000,000 PPM |
| Volume | `VOLUME_UNITS` | 0 to 1,000,000,000,000 units |
| Spread | `SPREAD_TICKS` | 0 to 1,000,000,000 ticks |
| Market factor | `FACTOR_PPM` | signed, ±10,000,000 PPM |
| Sector factor | `FACTOR_PPM` | signed, ±10,000,000 PPM |

Schema v1.3 retains all v1.2 return fields and appends generic target fields.
Return publishers populate both representations identically. Non-return
publishers zero return fields and publish neutral direction probability, so a
consumer cannot mistake volume or spread for expected return.

## Cache and forecast age

The service cache key includes context digest, model version, checkpoint digest,
target, and horizon. A hit must satisfy both monotonic expiration and configured
maximum forecast age. Stale and corrupt entries are removed. Cache capacity is
fixed and deterministic least-recently-inserted eviction is used only in this
off-path service.

The process-shared edge cache remains a fixed-layout integer structure. Its
lookup supports instrument/model/horizon keys. Cache population is an
asynchronous publication action after common-contract validation; no edge reader
knows an API endpoint.

## Service contract

Every role exposes `/healthz`, `/readyz`, `/version`, `/configuration`, and
`/metrics`. Health is process/model state; readiness additionally requires valid
configuration and an enabled adapter. Metrics use fixed names without instrument
labels. Request bodies and responses are size bounded. Logs are one-line JSON
without series values, credentials, checkpoints, or external payloads. Shutdown
stops admission and drains only up to the configured monotonic deadline.

The listener is default-deny: TLS 1.3 mutual authentication, exactly one
`spiffe://aegis-mx/<environment>/<service>` client identity, per-route service
RBAC, per-identity token-bucket admission, and bounded concurrent handlers are
required before application dispatch. Certificate, private-key, and trust-
bundle names resolve only inside a read-only secret-manager mount. Material
changes are hashed and loaded into a fresh context for subsequent handshakes.
TLS negotiation and HTTP I/O use hard deadlines inside bounded worker slots;
outgoing clients must compare the verified peer URI SAN with the exact intended
service identity after every new handshake.
Plain HTTP requires an explicit loopback-only development flag and is not
available on wildcard or non-loopback binds.

The checked-in [Dockerfile](../../infra/timeseries_forecast/Dockerfile) contains
only the Python service and its hash-locked runtime dependency. The
[Kubernetes manifest](../../infra/timeseries_forecast/kubernetes.yaml) contains
only these three non-colocated roles, ClusterIP services, probes, resource
bounds, non-root/read-only security contexts, read-only mTLS secret mounts, and
a default-deny/no-egress network policy. TCP probes only establish listener
availability; an authenticated monitor calls the health/readiness endpoints.
The image digest is an explicit deployment placeholder and must be replaced by
an approved built artifact digest before application.

The default executables use `ReferenceTimesFmAdapter`, which validates service
infrastructure but is not TimesFM. An actual TimesFM runtime must be injected
through `TimesFmBackend`; this repository neither installs one nor fetches a
checkpoint. Optional checkpoints are accepted only by lowercase SHA-256
identity, and their package lock, model card, SBOM, capacity, and walk-forward
evidence remain deployment prerequisites.

See [ADR 0014](../adr/0014-off-hot-path-timeseries-forecast-serving.md),
[ADR 0035](../adr/0035-zero-trust-service-boundaries-and-sandboxed-content.md), the
[event contracts](event-contracts.md), and the [testing guide](../testing/timeseries-forecast-testing.md).
