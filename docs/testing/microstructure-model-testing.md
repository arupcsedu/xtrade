# Microstructure model testing

## Deterministic seed and claim boundary

The retained integration seed is `20260829`. Synthetic dataset and training
reports always state `economic_value_claim=false`. Tests assess infrastructure,
contracts, and performance only.

## Test layers

- C++ unit tests validate artifact signatures, expiry, feature bounds, queue
  context, all seven roles, five-part costs, and deterministic simulator hashes.
- Python tests validate bounded untrusted CSV parsing, deterministic training,
  loss convergence, exact feature order, export signatures, OOD rejection,
  five-part costs, and atomic artifact replacement.
- Schema tests prove v1 to v1.1 to v1.2 to v1.3 FlatBuffers conformance and byte-for-byte
  Python/C++ equality of `model_forecast_v1_2.amae`.
- Integration invokes `synth-microstructure-dataset`, trains seven artifacts in
  `/scratch/djy8hg/env/aegis_mx_contracts`, and evaluates the first dataset row
  through Python and `native-model-verify`. Native-v1 parity tolerance is zero
  PPM.
- The benchmark measures direct eight-feature inference, records steady-state
  allocations, and compares mean CPU time with the configured 10,000 ns budget.

## Reproduction

```bash
source tools/toolchain.sh
build/release/cpp/models/synth-microstructure-dataset \
  --output /tmp/aegis-microstructure.csv --seed 20260829 100000

PYTHONPATH=python/training:python/intelligence \
  /scratch/djy8hg/env/aegis_mx_contracts/bin/python -m aegis_mx_training \
  --dataset /tmp/aegis-microstructure.csv \
  --output-directory /tmp/aegis-models \
  --seed 20260829 \
  --expires-wall-clock-utc-ns 1900000000000000000

build/release/cpp/models/native-model-verify \
  /tmp/aegis-models/queue_depletion.amdl \
  /tmp/aegis-microstructure.csv
```

The expiry value above is a deterministic fixture, not a production setting.
