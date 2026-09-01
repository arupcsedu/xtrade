#include "aegis/execution/router.hpp"

#include "allocation_probe.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace execution = aegis::execution;
namespace common = aegis::common;
namespace risk = aegis::risk;

namespace {

inline constexpr common::ConfigurationVersion kConfiguration{600U, 1U};
inline constexpr common::InstrumentId kInstrument{500U, 1U};

[[nodiscard]] execution::RouterConfiguration router_configuration() noexcept {
  execution::RouterConfiguration result{
      .configuration_version = kConfiguration,
      .maximum_quote_age_ns = 10'000U,
      .maximum_route_latency_ns = 10'000U,
      .queue_deterioration_threshold_units = 100U,
      .uncertainty_penalty_currency_nanos_per_unit = 2U,
      .maximum_reject_rate_ppm = 200'000U,
      .minimum_fill_probability_ppm = 10'000U,
      .maximum_direct_consolidated_divergence_ticks = 0U,
      .maximum_route_attempts = 4U,
      .venue_count = 8U};
  constexpr auto all_policies =
      (std::uint32_t{1U} << static_cast<std::uint8_t>(
           execution::ExecutionPolicy::auction_participation)) -
      1U;
  for (std::size_t index = 0U; index < result.venue_count; ++index) {
    result.venues[index] = {
        .venue_id = common::VenueId{400U, static_cast<std::uint64_t>(index + 1U)},
        .allowed_policy_mask = all_policies,
        .maximum_concentration_ppm = execution::kRouterPartsPerMillion,
        .maximum_child_quantity_units = 10'000U,
        .tie_break_rank = static_cast<std::uint16_t>(index + 1U),
        .enabled = true,
        .regulatory_authorized = true,
        .require_self_trade_clear = true,
        .allow_auction = true};
  }
  result.stable_hash = execution::stable_router_configuration_hash(result);
  return result;
}

[[nodiscard]] execution::RoutingRequest
router_request(const execution::ExecutionPolicy policy) noexcept {
  execution::RoutingRequest result{
      .objective = {.objective_id = common::IntentId{700U, 1U},
                    .session_id = common::SessionId{701U, 1U},
                    .account_id = common::AccountId{702U, 1U},
                    .strategy_id = common::StrategyId{703U, 1U},
                    .instrument_id = kInstrument,
                    .source_forecast_id = common::ForecastId{704U, 1U},
                    .feature_snapshot_id = common::FeatureSnapshotId{705U, 1U},
                    .configuration_version = kConfiguration,
                    .target_order_id = {},
                    .side = risk::IntentAction::buy,
                    .policy = policy,
                    .limit_price_ticks = 105,
                    .total_quantity_units = 1'000U,
                    .filled_quantity_units = 0U,
                    .maximum_child_quantity_units = 100U,
                    .start_process_monotonic_time_ns = 900'000U,
                    .end_process_monotonic_time_ns = 2'000'000U,
                    .slice_interval_ns = 10'000U,
                    .alpha_half_life_ns = 1'000'000U,
                    .expected_alpha_microticks = 2'000'000,
                    .tick_value_currency_nanos = 100U,
                    .participation_rate_ppm = 100'000U,
                    .has_limit_price = true},
      .consolidated_quote = {.best_bid_venue_id = common::VenueId{400U, 1U},
                             .best_ask_venue_id = common::VenueId{400U, 1U},
                             .best_bid_price_ticks = 99,
                             .best_ask_price_ticks = 101,
                             .observed_process_monotonic_time_ns = 999'990U,
                             .valid = true},
      .working_order = {},
      .now_process_monotonic_time_ns = 1'000'000U,
      .market_volume_since_last_slice_units = 1'000U,
      .target_cumulative_volume_curve_ppm = 500'000U,
      .venue_count = 8U};
  result.objective.stable_hash =
      execution::stable_execution_objective_hash(result.objective);
  result.consolidated_quote.stable_hash =
      execution::stable_consolidated_quote_hash(result.consolidated_quote);
  for (std::size_t index = 0U; index < result.venue_count; ++index) {
    result.venues[index] = {
        .venue_id = common::VenueId{400U, static_cast<std::uint64_t>(index + 1U)},
        .instrument_id = kInstrument,
        .trading_state = execution::VenueTradingState::open,
        .self_trade_prevention = execution::SelfTradePreventionState::clear,
        .bid_price_ticks = 99,
        .ask_price_ticks = 101,
        .bid_quantity_units = 1'000U,
        .ask_quantity_units = 1'000U,
        .estimated_hidden_quantity_units = 100U,
        .queue_ahead_quantity_units = static_cast<std::uint64_t>(index * 10U),
        .venue_latency_ns = static_cast<std::uint64_t>(100U + index),
        .observed_process_monotonic_time_ns = 999'990U,
        .maker_fee_currency_nanos_per_unit = -1,
        .taker_fee_currency_nanos_per_unit = 2,
        .adverse_selection_microticks = 100'000,
        .fill_probability_ppm = 900'000U,
        .reject_rate_ppm = 1'000U,
        .quote_valid = true,
        .regulatory_eligible = true};
    result.venues[index].stable_hash =
        execution::stable_venue_observation_hash(result.venues[index]);
  }
  result.stable_hash = execution::stable_routing_request_hash(result);
  return result;
}

void benchmark_router_eight_venue_scoring(benchmark::State& state) {
  execution::SmartOrderRouter router{router_configuration()};
  const auto request = router_request(execution::ExecutionPolicy::aggressive_take);
  std::uint64_t allocations{};
  for (auto iteration : state) {
    static_cast<void>(iteration);
    const auto before = allocation_probe::count();
    auto decision = router.route(request);
    allocations += allocation_probe::count() - before;
    benchmark::DoNotOptimize(decision);
  }
  state.SetItemsProcessed(state.iterations());
  state.counters["steady_state_allocations"] =
      benchmark::Counter(static_cast<double>(allocations));
}

void benchmark_router_twap_slice_and_score(benchmark::State& state) {
  execution::SmartOrderRouter router{router_configuration()};
  const auto request = router_request(execution::ExecutionPolicy::twap);
  for (auto iteration : state) {
    static_cast<void>(iteration);
    benchmark::DoNotOptimize(router.route(request));
  }
  state.SetItemsProcessed(state.iterations());
}

void benchmark_execution_cost_attribution(benchmark::State& state) {
  const execution::ExecutionCostAttributionInput input{
      .objective_id = common::IntentId{700U, 1U},
      .venue_id = common::VenueId{400U, 1U},
      .side = risk::IntentAction::buy,
      .arrival_price_ticks = 100,
      .decision_price_ticks = 101,
      .fill_price_ticks = 102,
      .post_fill_reference_price_ticks = 99,
      .filled_quantity_units = 100U,
      .unfilled_quantity_units = 50U,
      .fee_currency_nanos = 1'000U,
      .modeled_impact_ticks = 1U,
      .tick_value_currency_nanos = 10U};
  for (auto iteration : state) {
    static_cast<void>(iteration);
    execution::ExecutionCostAttribution output{};
    benchmark::DoNotOptimize(execution::execution_cost_attribution(input, output));
    benchmark::DoNotOptimize(output);
  }
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(benchmark_router_eight_venue_scoring);
BENCHMARK(benchmark_router_twap_slice_and_score);
BENCHMARK(benchmark_execution_cost_attribution);

} // namespace
