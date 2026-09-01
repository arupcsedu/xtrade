#include "aegis/risk/portfolio_service.hpp"

#include "../risk/tests/portfolio_test_support.hpp"
#include "allocation_probe.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace portfolio = aegis::risk::portfolio;
namespace support = aegis::risk::portfolio::test;

namespace {

inline constexpr std::size_t kPortfolioBenchmarkSamples = 512U;

struct Harness {
  std::unique_ptr<portfolio::PortfolioJournal> journal;
  std::unique_ptr<portfolio::PortfolioRiskService> service;
};

[[nodiscard]] Harness harness() {
  Harness result;
  result.journal = std::make_unique<portfolio::PortfolioJournal>();
  result.service = std::make_unique<portfolio::PortfolioRiskService>(
      support::configuration(), *result.journal);
  benchmark::DoNotOptimize(result.service->apply(support::mark(1U, 100)));
  auto order = support::order(2U);
  order.quantity_units = kPortfolioBenchmarkSamples;
  order.remaining_quantity_units = kPortfolioBenchmarkSamples;
  order.stable_hash = portfolio::stable_event_hash(order);
  benchmark::DoNotOptimize(result.service->apply(order));
  return result;
}

[[nodiscard]] std::array<portfolio::PortfolioEvent, kPortfolioBenchmarkSamples>
fills() {
  std::array<portfolio::PortfolioEvent, kPortfolioBenchmarkSamples> values{};
  for (std::size_t index = 0U; index < values.size(); ++index) {
    values[index] =
        support::fill(3U + index, 1U + index, portfolio::Side::buy, 100, 1U);
  }
  return values;
}

void benchmark_portfolio_fill_to_snapshot(benchmark::State& state) {
  const auto events = fills();
  auto active = harness();
  std::size_t cursor{};
  bool started = false;
  std::uint64_t allocations{};
  for (auto iteration : state) {
    static_cast<void>(iteration);
    if (cursor == 0U && started) {
      state.PauseTiming();
      active = harness();
      state.ResumeTiming();
    }
    started = true;
    const auto before = allocation_probe::count();
    auto result = active.service->apply(events[cursor]);
    allocations += allocation_probe::count() - before;
    benchmark::DoNotOptimize(result);
    cursor = (cursor + 1U) % events.size();
  }
  state.SetItemsProcessed(state.iterations());
  state.counters["steady_state_allocations"] =
      benchmark::Counter(static_cast<double>(allocations));
}

void benchmark_portfolio_mark_to_snapshot(benchmark::State& state) {
  auto active = harness();
  std::uint64_t ordinal = 10'000U;
  for (auto iteration : state) {
    static_cast<void>(iteration);
    const auto event =
        support::mark(ordinal, 100 + static_cast<std::int64_t>(ordinal % 10U));
    benchmark::DoNotOptimize(active.service->apply(event));
    ++ordinal;
    if (ordinal == 17'000U) {
      state.PauseTiming();
      active = harness();
      ordinal = 10'000U;
      state.ResumeTiming();
    }
  }
  state.SetItemsProcessed(state.iterations());
}

void benchmark_portfolio_snapshot_read(benchmark::State& state) {
  auto active = harness();
  for (auto iteration : state) {
    static_cast<void>(iteration);
    portfolio::PortfolioSnapshotStore::ReadHandle handle;
    auto status = active.service->acquire_snapshot(support::kStart + 100U, handle);
    benchmark::DoNotOptimize(status);
    benchmark::DoNotOptimize(handle.get());
  }
  state.SetItemsProcessed(state.iterations());
}

void benchmark_portfolio_stress_six_scenarios(benchmark::State& state) {
  auto active = harness();
  benchmark::DoNotOptimize(active.service->apply(support::fill(3U, 1U)));
  portfolio::StressConfiguration configuration;
  configuration.stable_hash =
      portfolio::stable_stress_configuration_hash(configuration);
  for (auto iteration : state) {
    static_cast<void>(iteration);
    benchmark::DoNotOptimize(active.service->evaluate_stress(configuration));
  }
  state.SetItemsProcessed(state.iterations() * 6);
}

BENCHMARK(benchmark_portfolio_fill_to_snapshot);
BENCHMARK(benchmark_portfolio_mark_to_snapshot);
BENCHMARK(benchmark_portfolio_snapshot_read);
BENCHMARK(benchmark_portfolio_stress_six_scenarios);

} // namespace
