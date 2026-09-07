#include "aegis/observability/telemetry.hpp"

#include "allocation_probe.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>

namespace {

void benchmark_observability_bounded_publish(benchmark::State& state) {
  aegis::observability::TelemetryPublisher publisher;
  aegis::observability::TelemetryProcessor processor{publisher};
  const auto allocations_before = allocation_probe::count();
  for (auto _ : state) {
    static_cast<void>(_);
    auto accepted =
        publisher.observe_latency(aegis::observability::LatencyStage::risk_check, 250U);
    benchmark::DoNotOptimize(accepted);
    state.PauseTiming();
    auto drained = processor.drain(1U);
    benchmark::DoNotOptimize(drained);
    state.ResumeTiming();
  }
  const auto allocations = allocation_probe::count() - allocations_before;
  state.counters["drops"] = static_cast<double>(publisher.dropped_points());
  state.counters["steady_state_allocations"] = static_cast<double>(allocations);
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
}

BENCHMARK(benchmark_observability_bounded_publish);

} // namespace
