# Synthetic Exchange Testing

## Scope and deterministic seeds

These gates cover the license-clean SMX/1 generator and replay oracle. They do
not establish conformance to any venue. The published compatibility seed is
`20260829`; property tests record base seeds `0xAE615000`, `0x5CE00001`, and
`0x5A17B00B` in source. A seed change requires review of the resulting stream,
not silent golden-threshold replacement.

## Build and unit/replay tests

All commands for this checkout run in the required isolated environment:

```bash
source /scratch/djy8hg/env/aegis_mx_contracts/bin/activate
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
```

The C++ suite covers deterministic stream equality and divergence, published
configuration/event/book hashes, configuration rejection, SMX encode/decode and
corruption, all scenario counts, property seeds, canonical envelope validation,
raw replay, byte-identical repeated capture generation, oracle corruption, and
fail-closed gap/reorder state.

## Command-line acceptance

```bash
build/dev/cpp/market_data/synth-exchange-generate \
  --output-prefix build/fixtures/burst \
  --seed 20260829 \
  --events 100000 \
  --venues 2 \
  --instruments 8 \
  --feed-mode mixed \
  --scenario high-message-rate-burst

build/dev/cpp/market_data/synth-exchange-verify \
  --capture build/fixtures/burst.smxcap \
  --book build/fixtures/burst.book.txt

build/dev/cpp/market_data/synth-exchange-stream \
  --capture build/fixtures/burst.smxcap \
  --acceleration 0 > build/fixtures/burst.packets
```

The expected raw packet file size is `physical_packets * 156` bytes. An
acceleration of zero disables pacing; one follows capture timing; larger values
speed it up by that integer factor. Stream output is a file descriptor only and
does not open a socket.

## Sanitizer and fuzz gates

```bash
cmake --preset ubsan
cmake --build --preset ubsan --parallel
ctest --preset ubsan

cmake --preset fuzz
cmake --build --preset fuzz --target aegis_smx_packet_fuzz --parallel
build/fuzz/cpp/market_data/aegis_smx_packet_fuzz \
  -runs=10000 -seed=20260829 -max_len=156
```

ASan and TSan builds remain required in exhaustive CI. On the original host,
their runtimes cannot execute under the hard 50 GiB virtual-memory limit needed
for sanitizer shadow mappings; a compiled but non-executed sanitizer binary is
reported as a limitation, never as a passing runtime gate.

## Performance evidence

Use a release build and retain JSON:

```bash
cmake --preset release
cmake --build --preset release --parallel
build/release/cpp/benchmarks/aegis_benchmark_smoke \
  --benchmark_filter='benchmark_(bounded_generation|mock_packet_encoding)' \
  --benchmark_min_time=0.1s \
  --benchmark_out=build/reports/benchmarks/synthetic-exchange.json \
  --benchmark_out_format=json
```

`benchmark_bounded_generation` excludes capture I/O, pacing, and canonical
FlatBuffer allocation. Report the executing CPU and observed `items_per_second`;
do not convert an unmeasured target into a success claim.
