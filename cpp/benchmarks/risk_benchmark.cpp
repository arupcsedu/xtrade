#include "aegis/risk/engine.hpp"

#include "../risk/tests/test_support.hpp"
#include "allocation_probe.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace risk = aegis::risk;

namespace {

inline constexpr std::size_t kRiskBenchmarkSamples = 2'048U;

[[nodiscard]] risk::RiskLimitSnapshot benchmark_limits() noexcept {
  auto limits = risk::test::limits();
  limits.maximum_orders_per_window = static_cast<std::uint32_t>(kRiskBenchmarkSamples);
  limits.order_rate_window_ns = 1U;
  limits.maximum_position_age_ns = 1'000'000U;
  limits.maximum_market_state_age_ns = 1'000'000U;
  limits.maximum_clock_state_age_ns = 1'000'000U;
  limits.maximum_feed_book_age_ns = 1'000'000U;
  limits.maximum_configuration_age_ns = 1'000'000U;
  limits.symbols[0U].maximum_absolute_position_units = 1'000'000U;
  limits.maximum_gross_exposure_currency_nanos = 1'000'000'000'000U;
  limits.maximum_absolute_net_exposure_currency_nanos = 1'000'000'000'000U;
  limits.maximum_sector_exposure_currency_nanos[0U] = 1'000'000'000'000U;
  limits.maximum_factor_exposure_currency_nanos[0U] = 1'000'000'000'000U;
  limits.credit_capital_limit_currency_nanos = 1'000'000'000'000U;
  limits.stable_hash = risk::stable_limit_snapshot_hash(limits);
  return limits;
}

[[nodiscard]] std::array<risk::RiskEvaluationRequest, kRiskBenchmarkSamples>
benchmark_requests() {
  std::array<risk::RiskEvaluationRequest, kRiskBenchmarkSamples> requests{};
  for (std::size_t index = 0U; index < requests.size(); ++index) {
    requests[index] =
        risk::test::request(100'000U + index, risk::IntentAction::buy,
                            risk::test::kNow + static_cast<std::uint64_t>(index * 2U));
  }
  return requests;
}

[[nodiscard]] risk::DeterministicPreTradeRiskEngine&
initialize_engine(std::optional<risk::DeterministicPreTradeRiskEngine>& engine,
                  const risk::RiskLimitSnapshot& limits,
                  risk::RiskDecisionJournal& journal) {
  engine.reset();
  auto& active = engine.emplace(limits, journal);
  risk::test::initialize_state(active);
  return active;
}

void benchmark_risk_evaluation(benchmark::State& state) {
  const auto limits = benchmark_limits();
  const auto requests = benchmark_requests();
  risk::RiskDecisionJournal journal;
  std::optional<risk::DeterministicPreTradeRiskEngine> engine;
  auto* active_engine = &initialize_engine(engine, limits, journal);
  std::size_t cursor{};
  bool measurement_started = false;
  std::uint64_t hot_path_allocations{};
  for (auto iteration : state) {
    static_cast<void>(iteration);
    if (cursor == 0U && measurement_started) {
      state.PauseTiming();
      active_engine = &initialize_engine(engine, limits, journal);
      state.ResumeTiming();
    }
    measurement_started = true;
    const auto allocations_before = allocation_probe::count();
    auto result = active_engine->evaluate(requests[cursor]);
    hot_path_allocations += allocation_probe::count() - allocations_before;
    risk::RiskDecision drained{};
    auto popped = journal.try_pop(drained);
    benchmark::DoNotOptimize(result);
    benchmark::DoNotOptimize(popped);
    cursor = (cursor + 1U) % requests.size();
  }
  state.SetItemsProcessed(state.iterations());
  state.counters["steady_state_allocations"] =
      benchmark::Counter(static_cast<double>(hot_path_allocations));
}

[[nodiscard]] std::uint64_t
percentile(const std::array<std::uint64_t, kRiskBenchmarkSamples>& samples,
           const std::size_t numerator, const std::size_t denominator) noexcept {
  const auto rank = (samples.size() * numerator + denominator - 1U) / denominator;
  return samples[rank - 1U];
}

void benchmark_risk_latency_distribution(benchmark::State& state) {
  const auto limits = benchmark_limits();
  const auto requests = benchmark_requests();
  risk::RiskDecisionJournal journal;
  std::optional<risk::DeterministicPreTradeRiskEngine> engine;
  std::array<std::uint64_t, kRiskBenchmarkSamples> samples{};
  for (auto iteration : state) {
    static_cast<void>(iteration);
    state.PauseTiming();
    auto& active_engine = initialize_engine(engine, limits, journal);
    state.ResumeTiming();
    for (std::size_t index = 0U; index < samples.size(); ++index) {
      const auto started = std::chrono::steady_clock::now();
      auto result = active_engine.evaluate(requests[index]);
      const auto finished = std::chrono::steady_clock::now();
      benchmark::DoNotOptimize(result);
      samples[index] = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started)
              .count());
      risk::RiskDecision drained{};
      benchmark::DoNotOptimize(journal.try_pop(drained));
    }
    state.PauseTiming();
    std::ranges::sort(samples);
    state.counters["p50_ns"] =
        benchmark::Counter(static_cast<double>(percentile(samples, 50U, 100U)));
    state.counters["p95_ns"] =
        benchmark::Counter(static_cast<double>(percentile(samples, 95U, 100U)));
    state.counters["p99_ns"] =
        benchmark::Counter(static_cast<double>(percentile(samples, 99U, 100U)));
    state.counters["p99_9_ns"] =
        benchmark::Counter(static_cast<double>(percentile(samples, 999U, 1'000U)));
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<std::int64_t>(samples.size()));
}

void benchmark_risk_kill_switch_to_rejection(benchmark::State& state) {
  const auto limits = benchmark_limits();
  risk::RiskDecisionJournal journal;
  std::optional<risk::DeterministicPreTradeRiskEngine> engine;
  std::uint64_t ordinal = 500'000U;
  for (auto iteration : state) {
    static_cast<void>(iteration);
    state.PauseTiming();
    auto& active_engine = initialize_engine(engine, limits, journal);
    auto request = risk::test::request(ordinal++);
    state.ResumeTiming();
    auto update =
        active_engine.update_kill_switch({.scope = risk::KillSwitchScope::firm,
                                          .target_index = 0U,
                                          .authority_epoch = limits.authority_epoch,
                                          .command_sequence = 1U,
                                          .engaged = true,
                                          .operator_authorized = false});
    auto result = active_engine.evaluate(request);
    benchmark::DoNotOptimize(update);
    benchmark::DoNotOptimize(result);
    state.PauseTiming();
    if (update != risk::StateUpdateStatus::applied ||
        result.decision.reason != risk::RiskReason::kill_switch_engaged) {
      state.SkipWithError("kill command did not order before evaluation");
    }
    risk::RiskDecision drained{};
    static_cast<void>(journal.try_pop(drained));
    state.ResumeTiming();
  }
}

BENCHMARK(benchmark_risk_evaluation);
BENCHMARK(benchmark_risk_latency_distribution)->Iterations(5);
BENCHMARK(benchmark_risk_kill_switch_to_rejection);

} // namespace
