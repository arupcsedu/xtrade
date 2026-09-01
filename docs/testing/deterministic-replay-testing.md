# Deterministic replay testing

All replay tests use the repository-required isolated environment at
`/scratch/djy8hg/env/aegis_mx_contracts`; no credentials or external data are
required.

```bash
source /scratch/djy8hg/env/aegis_mx_contracts/bin/activate
cmake --preset dev
cmake --build --preset dev --target aegis_replay_tests aegis_replay_tool
ctest --preset dev -R aegis_replay_tests --output-on-failure
```

The fixed replay seeds are `0x26A361D5`, `0xD37E2610`, and `0x2600`. The suite
covers repeated output hashes, original/accelerated/maximum/single-step modes,
pause/resume, instrument selection, loss, duplication, reordering, latency,
drift, outage, crash epochs, halt/reopen, opaque malicious news text, macro
injection, every recorded output category, and exact versus counterfactual
model behavior.

Run a capture at maximum speed:

```bash
build/dev/cpp/replay/aegis-replay \
  --capture events.smxcap \
  --speed maximum \
  --seed 20260831 \
  --manifest replay-manifest.json \
  --summary replay-summary.json
```

Run an offline journal with an inclusive time window and deterministic faults:

```bash
build/dev/cpp/replay/aegis-replay \
  --journal JOURNAL_DIR \
  --from-ns 1000000 --to-ns 2000000 \
  --fault latency:1:1000:1:25000 \
  --fault loss:500:500:1:0 \
  --fault news:700:700:1:0:opaque-untrusted-fixture \
  --summary replay-summary.json
```

Fault syntax is `KIND:FIRST:LAST:EVERY:VALUE[:OPAQUE_TEXT]`. Kinds are
`latency`, `loss`, `duplicate`, `reorder`, `drift`, `outage`, `crash`, `halt`,
`reopen`, `news`, and `macro`. Ordinals refer to immutable source ordinals.
Instrument filters use two 64-bit hexadecimal words: `--instrument HIGH:LOW`.
CLI memory budgets can be reduced with `--max-records` and
`--max-payload-bytes`; values above the hard ceilings are rejected.

Original-speed CLI replay sleeps only in this offline process. Tests and
accelerated runs use a virtual pacer. Recompute mode is exposed through the C++
API and requires a selected version and registered `IModelRecomputer`; the CLI
does not dynamically load model code. A recompute run is counterfactual and
cannot be used as exact audit evidence.

Benchmark smoke:

```bash
build/release/cpp/benchmarks/aegis_benchmark_smoke \
  --benchmark_filter=benchmark_maximum_speed_packet_replay \
  --benchmark_min_time=0.01s
```

Replay fails closed on an invalid/truncated capture, corrupt journal, malformed
canonical envelope, missing selected model, absent recomputer, target failure,
or time overflow. It never repairs or changes a source.
