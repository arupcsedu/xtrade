# Smart order router testing

## Scenario coverage

`aegis_execution_tests` contains the `RouterPaperGatewayTest` suite. Every test
fixture starts an isolated in-process `PaperBrokerGateway`. Every routed child is
rebuilt as a new venue-specific request and evaluated by the real deterministic
risk test harness before the paper gateway accepts it; the routing decision is
never reused as approval. Abstention cases assert that the gateway command count
does not change.

The deterministic seed is `20260831`. Coverage includes:

- unavailable best price with safe next-venue fallback;
- halted and stale venues;
- queue deterioration producing replace or protective cancel;
- partial fill and remaining-objective quantity;
- conflicting direct and consolidated best-venue data;
- venue reject burst and maximum concentration;
- regulatory denial, self-trade conflict, health/readiness, and shutdown;
- rebalance auction participation and paired-quantity cap;
- passive join/improve, aggressive, IOC, participation, TWAP, VWAP, and POV;
- deterministic score tie breaking and route-loop protection;
- fixed-point fill uncertainty and alpha decay; and
- integer execution-cost attribution and stable hashes.

The compile-time assertion that `RoutingDecision` is not convertible to
`GatewayRequest`, together with the gateway's exact risk binding, covers the
non-bypass property. Gateway safety tests continue to cover final mode, clock,
data/book, halt, kill, fencing, scope, journal, session, and rate predicates.

## Commands

```bash
source tools/toolchain.sh
cmake --preset dev
cmake --build --preset dev --target aegis_execution_tests
build/dev/cpp/execution/aegis_execution_tests \
  --gtest_filter='RouterPaperGatewayTest.*' --gtest_color=no
```

The benchmark categories are eight-venue scoring and explanation publication,
TWAP slice plus venue score, and cost attribution:

```bash
source tools/toolchain.sh
cmake --preset release
cmake --build --preset release --target aegis_benchmark_smoke
build/release/cpp/benchmarks/aegis_benchmark_smoke \
  --benchmark_filter='router|execution_cost_attribution'
```

Sanitizer commands and host limitations are documented in
[Quality Gates](quality-gates.md).
