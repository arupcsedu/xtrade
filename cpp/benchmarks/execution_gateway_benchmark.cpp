#include "aegis/execution/simulated_gateway.hpp"

#include "../execution/tests/test_support.hpp"
#include "allocation_probe.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace execution = aegis::execution;
namespace support = aegis::execution::test;

namespace {

inline constexpr std::size_t kGatewayBenchmarkSamples = 128U;

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct Harness {
  execution::GatewayConfiguration configuration;
  std::unique_ptr<execution::GatewayAuditJournal> journal;
  std::unique_ptr<execution::SyntheticExchangeGateway> gateway;
  std::array<execution::GatewayRequest, kGatewayBenchmarkSamples> requests{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] Harness prepare() {
  Harness result;
  result.configuration = support::configuration();
  result.configuration.maximum_new_orders_per_window = 4'096U;
  result.configuration.command_rate_window_ns = 1'000'000U;
  result.configuration.stable_hash =
      execution::stable_gateway_configuration_hash(result.configuration);
  result.journal = std::make_unique<execution::GatewayAuditJournal>();
  result.gateway = std::make_unique<execution::SyntheticExchangeGateway>(
      result.configuration, *result.journal);
  static_cast<void>(result.gateway->start(support::kNow - 100U));
  for (std::size_t index = 0U; index < result.requests.size(); ++index) {
    const auto ordinal = static_cast<std::uint64_t>(index + 1U);
    auto request = support::request(
        execution::GatewayMode::simulation, aegis::oms::GatewayCommandKind::new_order,
        ordinal, support::kNow + ordinal, aegis::common::OrderId{50U, ordinal});
    request.safety.effective_configuration_hash = result.configuration.stable_hash;
    request.safety.stable_hash =
        execution::stable_final_safety_state_hash(request.safety);
    request.stable_hash = execution::stable_gateway_request_hash(request);
    result.requests[index] = request;
  }
  return result;
}

void benchmark_gateway_final_submit(benchmark::State& state) {
  auto active = prepare();
  std::size_t cursor{};
  bool started = false;
  std::uint64_t allocations{};
  for (auto iteration : state) {
    static_cast<void>(iteration);
    if (cursor == 0U && started) {
      state.PauseTiming();
      active = prepare();
      state.ResumeTiming();
    }
    started = true;
    const auto before = allocation_probe::count();
    auto result = active.gateway->send_order(active.requests[cursor]);
    allocations += allocation_probe::count() - before;
    benchmark::DoNotOptimize(result);
    cursor = (cursor + 1U) % active.requests.size();
  }
  state.SetItemsProcessed(state.iterations());
  state.counters["steady_state_allocations"] =
      benchmark::Counter(static_cast<double>(allocations));
}

void benchmark_gateway_ack_poll(benchmark::State& state) {
  auto active = prepare();
  for (const auto& request : active.requests) {
    benchmark::DoNotOptimize(active.gateway->send_order(request));
  }
  std::size_t cursor{};
  for (auto iteration : state) {
    static_cast<void>(iteration);
    if (cursor == active.requests.size()) {
      state.PauseTiming();
      active = prepare();
      for (const auto& request : active.requests) {
        benchmark::DoNotOptimize(active.gateway->send_order(request));
      }
      cursor = 0U;
      state.ResumeTiming();
    }
    execution::GatewayEvent event{};
    auto status = active.gateway->poll_event(support::kNow + 1'000U, event);
    benchmark::DoNotOptimize(status);
    benchmark::DoNotOptimize(event);
    ++cursor;
  }
  state.SetItemsProcessed(state.iterations());
}

void benchmark_gateway_rate_limiter(benchmark::State& state) {
  execution::FixedWindowRateLimiter limiter{1'000'000U, 4'096U};
  std::uint64_t now = 1U;
  for (auto iteration : state) {
    static_cast<void>(iteration);
    benchmark::DoNotOptimize(limiter.allow(now++));
    if (now % 4'096U == 0U) {
      limiter.reset();
    }
  }
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(benchmark_gateway_final_submit);
BENCHMARK(benchmark_gateway_ack_poll);
BENCHMARK(benchmark_gateway_rate_limiter);

} // namespace
