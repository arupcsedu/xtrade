# Event backtester testing

Use the repository toolchain and the required isolated Python environment:

```bash
export AEGIS_PYTHON_ENV=/scratch/djy8hg/env/aegis_mx_contracts
source tools/toolchain.sh
cmake --preset dev
cmake --build --preset dev --target aegis_backtesting_tests
ctest --test-dir build/dev -R aegis_backtesting_tests --output-on-failure
```

The deterministic fixtures record their seeds and cover acknowledgement timing,
price-time priority, cancellation/execution ahead, partial fills, rejections,
stale expiry, hidden-liquidity assumptions, halts, reopening auctions, shadow
orders, multiple strategies/venues, forecast metrics, after-cost portfolio
metrics, cost sensitivity, capture replay, and repeated-result hashes.

The benchmark target is filtered with:

```bash
build/release/cpp/benchmarks/aegis_benchmark_smoke \
  --benchmark_filter=event_backtester
```
