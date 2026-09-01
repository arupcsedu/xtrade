#include "aegis/market_state/controller.hpp"

#include "allocation_probe.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>

namespace market_state = aegis::market_state;

namespace {

struct JournalCounter {
  std::uint64_t accepted{};
};

[[nodiscard]] market_state::JournalAppendStatus
append_transition(void* context,
                  const market_state::MarketStateTransition& transition) noexcept {
  auto& counter = *static_cast<JournalCounter*>(context);
  if (!transition.valid()) {
    return market_state::JournalAppendStatus::stopped;
  }
  ++counter.accepted;
  return market_state::JournalAppendStatus::accepted;
}

[[nodiscard]] constexpr market_state::MarketStateConfig config() noexcept {
  return {.minimum_dwell_ns = 10U,
          .recovery_stabilization_ns = 20U,
          .reopening_stabilization_ns = 30U,
          .scheduled_event_lead_ns = 100U,
          .volatility_spike_threshold_ppm = 200'000U,
          .spread_spike_threshold_ticks = 10U,
          .minimum_aggregate_depth_units = 1'000U,
          .model_ood_threshold_ppm = 800'000U,
          .model_disagreement_threshold_ppm = 700'000U};
}

[[nodiscard]] constexpr market_state::MarketStateInput
input(const std::uint64_t sequence, const std::uint64_t time) noexcept {
  return {.input_sequence = sequence,
          .process_monotonic_time_ns = time,
          .wall_clock_utc_ns = 1'000'000U + time,
          .official_trading_status = market_state::OfficialTradingStatus::open,
          .feed_health = market_state::FeedHealth::healthy,
          .book_validity = market_state::BookValidity::valid,
          .clock_quality = market_state::ClockQuality::healthy,
          .news_event_state = market_state::NewsEventState::none,
          .earnings_calendar = {},
          .macro_calendar = {},
          .realized_volatility_ppm = 1U,
          .spread_ticks = 1U,
          .aggregate_depth_units = 10'000U,
          .model_ood_ppm = 1U,
          .model_disagreement_ppm = 1U,
          .operator_controls = {}};
}

void reach_normal(market_state::MarketStateController& controller,
                  std::uint64_t& sequence, std::uint64_t& time) noexcept {
  static_cast<void>(controller.evaluate(input(++sequence, ++time)));
  static_cast<void>(controller.evaluate(input(++sequence, ++time)));
  time += config().recovery_stabilization_ns;
  static_cast<void>(controller.evaluate(input(++sequence, time)));
}

void benchmark_market_state_normal_evaluation(benchmark::State& state) {
  JournalCounter journal;
  market_state::MarketStateController controller{
      config(), {&journal, &append_transition}, 1U, 1'000'001U};
  std::uint64_t sequence{};
  std::uint64_t time{1U};
  reach_normal(controller, sequence, time);
  const auto allocations_before = allocation_probe::count();
  for (auto _ : state) {
    static_cast<void>(_);
    auto result = controller.evaluate(input(++sequence, ++time));
    benchmark::DoNotOptimize(result);
  }
  state.counters["steady_state_allocations"] = benchmark::Counter(
      static_cast<double>(allocation_probe::count() - allocations_before));
}

void benchmark_market_state_transition_and_publish(benchmark::State& state) {
  JournalCounter journal;
  market_state::MarketStateController controller{
      config(), {&journal, &append_transition}, 1U, 1'000'001U};
  std::uint64_t sequence{};
  std::uint64_t time{1U};
  reach_normal(controller, sequence, time);
  bool breaking = true;
  const auto allocations_before = allocation_probe::count();
  for (auto _ : state) {
    static_cast<void>(_);
    auto value = input(++sequence, ++time);
    value.news_event_state = breaking ? market_state::NewsEventState::breaking
                                      : market_state::NewsEventState::price_discovery;
    breaking = !breaking;
    auto result = controller.evaluate(value);
    benchmark::DoNotOptimize(result);
  }
  state.counters["steady_state_allocations"] = benchmark::Counter(
      static_cast<double>(allocation_probe::count() - allocations_before));
  state.counters["journaled_transitions"] =
      benchmark::Counter(static_cast<double>(journal.accepted));
}

void benchmark_market_state_atomic_read(benchmark::State& state) {
  JournalCounter journal;
  const market_state::MarketStateController controller{
      config(), {&journal, &append_transition}, 1U, 1'000'001U};
  market_state::MarketStateSnapshot snapshot{};
  const auto allocations_before = allocation_probe::count();
  for (auto _ : state) {
    static_cast<void>(_);
    auto result = controller.publisher().read(snapshot);
    benchmark::DoNotOptimize(result);
    benchmark::DoNotOptimize(snapshot);
  }
  state.counters["steady_state_allocations"] = benchmark::Counter(
      static_cast<double>(allocation_probe::count() - allocations_before));
}

BENCHMARK(benchmark_market_state_normal_evaluation);
BENCHMARK(benchmark_market_state_transition_and_publish);
BENCHMARK(benchmark_market_state_atomic_read);

} // namespace
