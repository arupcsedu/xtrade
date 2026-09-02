# ADR 0032: Paired regime-aware model deployment control

- Status: Accepted
- Date: 2026-08-31
- Owners: control-plane, models, risk, observability

## Context

A shadow candidate must receive exactly the feature snapshot seen by the
production model without acquiring order authority. A canary needs narrow,
deterministic limits and automatic rollback, but noisy measurements must not
cause promotion or rollback from a single unrepresentative observation.
Aggregate P&L can hide latency, calibration, drift, OOD, execution, and risk
failures in important regimes. The evaluator is asynchronous control-plane work;
model inference, risk, OMS, and gateways must never block on it.

The signed artifact registry already owns immutable model lifecycle and
compatible rollback. It does not own statistical comparison, shadow decisions,
canary scope, or automatic rollback policy.

## Decision

1. A standard-library Go `DeploymentCoordinator` sits above the signed model
   registry. It can change model lifecycle and issue a non-authoritative canary
   scope decision. It cannot construct an order, venue, account, price, or
   quantity, and it cannot bypass downstream pre-trade risk.
2. Configuration is canonical JSON, content-hashed, and Ed25519-signed. It binds
   production/candidate versions, exact feature schema, required regimes, all
   ten rollback thresholds, confidence score, minimum samples, allowed symbols
   and strategies, capital/rate/risk-freshness limits, approval evidence, and
   rollback identity. Set-like fields have canonical ordering.
3. A comparison is accepted only when production and candidate forecasts carry
   the same immutable feature-snapshot ID/hash and exchange as-of time. Late or
   malformed inputs fail closed. Shadow output is a checksummed hypothetical
   explanation with `executable=false` and `downstream_risk_required=true`.
4. Every rollback metric is oriented as candidate minus production, so positive
   means deterioration. Values use integer units or PPM. Deadline misses are
   derived from monotonic completion/deadline timestamps. Raw aggregate P&L is
   deliberately absent; P&L-attribution anomaly is one gate among ten.
5. Each metric is evaluated separately in each required market regime. A
   conservative one-sided paired confidence interval uses exact big-integer
   sums and squares. A breach occurs only after the metric's configured minimum
   sample count and when its upper confidence bound exceeds its maximum allowed
   regression.
6. Shadow evidence never promotes automatically. Canary activation requires a
   second actor, exact signed configuration hash, reason, and external approval
   reference. Registry signing credentials are process-separated in deployment:
   the coordinator owns lifecycle mutation authority; inference and strategy
   processes do not.
7. Canary admission checks signed symbol/strategy scope, fixed-point capital,
   order-rate window, monotonic request sequence, and fresh risk-snapshot
   provenance. Admission remains non-executable and every proposed order still
   passes normal market-state, risk, OMS, and gateway controls.
8. Any confidence-bound breach in canary invokes the registry's signed compatible
   rollback. Invalid paired input or deployment-audit failure also rolls back.
   If rollback fails, the coordinator invokes artifact-independent global model
   disable and enters `DISABLED_FAIL_CLOSED`. If both registry operations fail,
   it enters a distinct locally inhibited `DISABLE_FAILED_FAIL_CLOSED` state and
   never claims the registry was changed.
9. Control evidence is stored in an append-only, fsynced, SHA-256-chained JSONL
   audit. Each observation retains the complete bounded paired inputs and
   hypothetical output. On restart, an active canary requires a readable,
   verified audit and restores statistics, sequences, rate-window state, last
   trigger, and coordinator state before accepting work.
10. Prometheus export copies bounded state under a mutex and performs formatting
    off path. Labels are limited to signed IDs and fixed enums. A checked-in
    Grafana dashboard visualizes state, samples, confidence bounds, thresholds,
    triggers, rollback count, and scope rejections.

## Consequences

- The same signed configuration and paired observations produce the same
  confidence intervals, trigger, hypothetical decision hashes, and readiness.
- Poor behavior in breaking-news or volatility regimes cannot be hidden by a
  larger normal-regime sample. Every configured regime and metric is required
  before the human canary approval gate opens.
- A restart cannot silently reset canary evidence or rate counters. A missing,
  corrupt, incompatible, or non-readable audit prevents active-canary recovery.
- JSON audit I/O, signature verification, statistics, metrics, and registry
  mutation remain outside the inference/order hot path.
- The v1 audit assumes one fenced coordinator writer. It does not claim
  multi-process or multi-host consensus; loss of writer identity is fail-closed.
- The initial confidence interval uses a configured normal critical value and
  paired sample variance. Sequential-testing corrections, autocorrelation-aware
  intervals, and organization-approved experiment design remain prerequisites
  before production statistical claims.
- This component controls a model rollout only. It does not authorize live
  trading, capital, a strategy, an account, an exchange session, or a gateway.

## Rejected alternatives

- Unpaired aggregate metrics were rejected because changing market/regime mix
  can masquerade as a model effect.
- Promotion from aggregate P&L was rejected because it can conceal calibration,
  execution-cost, drift, OOD, and risk pressure regressions.
- Automatic canary or production promotion was rejected because statistical
  evidence cannot replace explicit operator/change approval.
- RPC evaluation from inference was rejected because model-service delay or
  failure must not block risk or OMS.
- Floating-point online statistics were rejected because platform/order effects
  can undermine exact replay at threshold boundaries.
