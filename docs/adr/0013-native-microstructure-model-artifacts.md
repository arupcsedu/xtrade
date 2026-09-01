# ADR 0013: Fixed-point native microstructure model artifacts

- Status: Accepted
- Date: 2026-08-29
- Owners: models, training, model-contracts

## Context

The initial microstructure suite needs seven interpretable estimators trained
offline in Python and evaluated deterministically in C++. Queue estimators also
need order-specific context that is not a property of an immutable market
feature snapshot. The forecast v1.1 cost object cannot separately audit
slippage and adverse-selection estimates.

The pinned Python environment intentionally contains no NumPy, scikit-learn,
ONNX, or ONNX Runtime dependency. Adding a large runtime solely for an initial
linear suite would enlarge the supply-chain and replay surface. The engineering
contract permits ONNX **or a stable native representation**.

## Decision

1. Offline training uses deterministic, dependency-free Python implementations
   of logistic and linear regression. It exports `AEGIS_MX_NATIVE_MODEL_V1`, a
   canonical ASCII format with fixed field order, LF endings, integer
   normalization values, integer coefficients, calibration metadata, training
   distribution ranges, explicit feature ordering, and a SHA-256 content
   signature. This digest proves integrity and identity; it is not an operator
   authorization signature.
2. The C++ loader is invoked only during activation. It uses bounded arrays,
   rejects unknown fields, duplicate features, malformed identities, invalid
   normalization, unsupported families, expired artifacts, invalid cost shares,
   and signature mismatches. The loaded representation is trivially copyable.
3. Inference performs fixed-point normalization, linear accumulation, Platt
   adjustment, and table/interpolation sigmoid evaluation. It does not allocate,
   call Python, read a clock, read a file, log, or perform an RPC.
4. The artifact family enum reserves `gradient_boosted_trees`, but v1 activation
   rejects it. A future bounded tree compiler can target the same feature and
   metadata interface without changing model consumers. Claiming GBT support
   requires a separate implementation and ADR.
5. `ModelInput` appends a presence-tagged `MicrostructureContext` containing
   integer quantity ahead, order quantity, order age nanoseconds, price distance
   ticks, venue number, and fee rate PPM. It is advisory model input only and has
   no order, risk, OMS, execution, or gateway capability.
6. The queue-depletion and passive-fill contracts include, in exact order,
   quantity ahead, add rate, cancel rate, execution rate, order age, price
   distance, venue, and session progress. Passive fill additionally includes
   order quantity. Missing context fails closed.
7. OOD scoring uses the immutable per-feature training minimum and maximum. The
   maximum normalized distance score is compared with the artifact's versioned
   rejection threshold. Fixed normalization parameters are never recomputed
   online.
8. Artifact expiry is checked against an explicitly supplied wall-clock UTC time
   during activation. Activation derives and stores a monotonic expiry; inference
   compares only the request's monotonic submission time. It never samples the
   system clock.
9. Schema v1.2 additively appends slippage and adverse-selection cost PPM fields.
   The five cost components are fee, spread, slippage, market impact, and adverse
   selection. Presence, range, hashing, Python/C++ golden equality, and v1.1 to
   v1.2 conformance are tested.
10. Training data is derived from the license-clean synthetic exchange generator
    with an explicit seed and stable row hash. Results are infrastructure
    validation only and MUST NOT be described as evidence of predictive or
    economic value.

## Consequences

- Python and C++ share an exact integer feature order and inference algorithm.
  Parity is exact for native v1 artifacts; the configured tolerance is zero PPM.
- Native artifact parsing is deliberately strict and is not a hot-path action.
- Logistic and linear models are implemented. Quantized MLP and GBT inference
  remain optional future work.
- An artifact's SHA-256 signature does not satisfy live-trading signed-runtime-
  configuration requirements. No trading functionality or authorization is
  introduced.
- Additive v1.2 deployment is reader-first. Writers emit v1.2 only after readers
  can preserve the two new fields. Rollback disables v1.2 writers before
  restoring v1.1 readers.

## Rejected alternatives

- Installing an unpinned ONNX stack was rejected because a stable native format
  satisfies the current linear models with a smaller audited dependency surface.
- Inferring quantity ahead or order age from unrelated snapshot fields was
  rejected because it would make feature provenance false.
- Combining slippage or adverse selection into market impact was rejected
  because the resulting forecast could not reproduce its cost decision.
- Sampling wall time inside `predict()` was rejected because it is unnecessary,
  nondeterministic hot-path work.
