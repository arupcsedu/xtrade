#include "aegis/time/clocks.hpp"
#include "aegis/time/latency.hpp"
#include "aegis/time/time_types.hpp"

#include <benchmark/benchmark.h>

namespace {

void benchmark_system_monotonic_timestamp(benchmark::State& state) {
  const aegis::time::SystemMonotonicClock clock;
  for (auto _ : state) {
    static_cast<void>(_);
    const auto result = clock.monotonic_now();
    benchmark::DoNotOptimize(result.value.value());
  }
}

void benchmark_system_wall_timestamp(benchmark::State& state) {
  const aegis::time::SystemWallClock clock;
  for (auto _ : state) {
    static_cast<void>(_);
    const auto result = clock.wall_now();
    benchmark::DoNotOptimize(result.value.value());
  }
}

void benchmark_injected_monotonic_timestamp(benchmark::State& state) {
  const aegis::time::TestMonotonicClock clock{aegis::time::MonotonicTimeNs{10U}};
  for (auto _ : state) {
    static_cast<void>(_);
    const auto result = clock.monotonic_now();
    benchmark::DoNotOptimize(result.value.value());
  }
}

void benchmark_checked_timestamp_arithmetic(benchmark::State& state) {
  aegis::time::MonotonicTimeNs timestamp{1'000U};
  constexpr aegis::time::DurationNs duration{17};
  for (auto _ : state) {
    static_cast<void>(_);
    benchmark::DoNotOptimize(timestamp);
    const auto result = aegis::time::checked_add(timestamp, duration);
    benchmark::DoNotOptimize(result.value.value());
    timestamp = result.value;
  }
}

void benchmark_checked_duration_arithmetic(benchmark::State& state) {
  aegis::time::DurationNs left{1'000};
  constexpr aegis::time::DurationNs right{17};
  for (auto _ : state) {
    static_cast<void>(_);
    benchmark::DoNotOptimize(left);
    const auto result = aegis::time::checked_subtract(left, right);
    benchmark::DoNotOptimize(result.value.value());
    left = result.value;
  }
}

void benchmark_latency_decomposition(benchmark::State& state) {
  aegis::time::LatencyTrace trace{
      .local_receive_time = aegis::time::MonotonicTimeNs{100U},
      .decode_complete_time = aegis::time::MonotonicTimeNs{110U},
      .book_complete_time = aegis::time::MonotonicTimeNs{125U},
      .feature_complete_time = aegis::time::MonotonicTimeNs{145U},
      .inference_start_time = aegis::time::MonotonicTimeNs{150U},
      .inference_complete_time = aegis::time::MonotonicTimeNs{180U},
      .decision_complete_time = aegis::time::MonotonicTimeNs{190U},
      .send_handoff_time = aegis::time::MonotonicTimeNs{202U},
      .acknowledgement_time = aegis::time::MonotonicTimeNs{250U},
  };
  for (auto _ : state) {
    static_cast<void>(_);
    benchmark::DoNotOptimize(trace);
    const auto result = aegis::time::decompose_latency(trace);
    benchmark::DoNotOptimize(result.value.order_round_trip.value());
  }
}

BENCHMARK(benchmark_system_monotonic_timestamp);
BENCHMARK(benchmark_system_wall_timestamp);
BENCHMARK(benchmark_injected_monotonic_timestamp);
BENCHMARK(benchmark_checked_timestamp_arithmetic);
BENCHMARK(benchmark_checked_duration_arithmetic);
BENCHMARK(benchmark_latency_decomposition);

} // namespace
