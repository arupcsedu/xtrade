#include "aegis/ensemble/gate.hpp"

#include "allocation_probe.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>

namespace ensemble = aegis::ensemble;
namespace common = aegis::common;
namespace market_state = aegis::market_state;
namespace models = aegis::models;

namespace {

inline constexpr common::InstrumentId kInstrument{1U, 1U};
inline constexpr common::SessionId kSession{2U, 2U};
inline constexpr common::ConfigurationVersion kConfiguration{3U, 3U};
inline constexpr std::uint64_t kNow = 1'000'000U;

[[nodiscard]] ensemble::EnsembleConfig config() noexcept {
  ensemble::EnsembleConfig result{.ensemble_model_id = common::ModelId{10U, 10U},
                                  .ensemble_model_version =
                                      common::ModelVersion{11U, 11U},
                                  .configuration_version = kConfiguration,
                                  .gate_kind = ensemble::GateKind::rule_based,
                                  .maximum_expert_weight_ppm = 800'000U,
                                  .ood_reject_threshold_ppm = 800'000U,
                                  .minimum_calibration_score_ppm = 500'000U,
                                  .minimum_data_quality_score_ppm = 500'000U,
                                  .maximum_forecast_age_ns = 10'000U,
                                  .maximum_market_state_age_ns = 1'000U,
                                  .degraded_feed_multiplier_ppm = 500'000U,
                                  .degraded_calibration_multiplier_ppm = 500'000U,
                                  .timeseries_breaking_news_multiplier_ppm = 100'000U,
                                  .uncertainty_penalty_multiplier_ppm = 100'000U,
                                  .calibration_uncertainty_multiplier_ppm = 100'000U,
                                  .ood_uncertainty_multiplier_ppm = 100'000U,
                                  .data_quality_uncertainty_multiplier_ppm = 100'000U,
                                  .safety_margin_ppm = 100U,
                                  .linear = {},
                                  .learned = {},
                                  .stable_hash = 0U};
  constexpr auto kAllMarketStates = static_cast<std::uint16_t>(
      (std::uint32_t{1U} << ensemble::kMarketStateCount) - 1U);
  constexpr auto kAllEventStates =
      static_cast<std::uint8_t>((std::uint32_t{1U} << ensemble::kEventStateCount) - 1U);
  for (std::size_t index = 0U; index < result.role_policies.size(); ++index) {
    result.role_policies[index] = {.role =
                                       static_cast<ensemble::ExpertRole>(index + 1U),
                                   .allowed_market_state_mask = kAllMarketStates,
                                   .allowed_event_state_mask = kAllEventStates,
                                   .base_multiplier_ppm = ensemble::kWeightScale};
  }
  result.stable_hash = ensemble::stable_ensemble_config_hash(result);
  return result;
}

[[nodiscard]] models::ModelForecast model_forecast(const std::uint64_t ordinal,
                                                   const bool cost) noexcept {
  models::ModelForecast forecast{
      .forecast_id = common::ForecastId{20U, ordinal},
      .session_id = kSession,
      .model_id = common::ModelId{30U, ordinal},
      .model_version = common::ModelVersion{40U, ordinal},
      .instrument_id = kInstrument,
      .feature_snapshot_id = common::FeatureSnapshotId{50U, ordinal},
      .configuration_version = kConfiguration,
      .model_control_generation = 1U,
      .horizon_ns = 1'000'000U,
      .as_of_exchange_event_time_ns = 1'800'000'000'000'000'000LL,
      .production_process_monotonic_time_ns = kNow - 100U,
      .expiration_process_monotonic_time_ns = kNow + 10'000U,
      .prediction = {
          .expected_return_ppm = cost ? 0 : 50'000,
          .return_p10_ppm = cost ? 0 : 46'000,
          .return_p50_ppm = cost ? 0 : 50'000,
          .return_p90_ppm = cost ? 0 : 54'000,
          .probability_down_ppm = cost ? 0U : 200'000U,
          .probability_flat_ppm = cost ? ensemble::kWeightScale : 150'000U,
          .probability_up_ppm = cost ? 0U : 650'000U,
          .volatility_ppm = 3'000U,
          .confidence_ppm = 900'000U,
          .calibration_score_ppm = 900'000U,
          .data_quality_score_ppm = 900'000U,
          .ood_score_ppm = 20'000U,
          .transaction_cost =
              cost ? models::TransactionCostEstimate{.spread_cost_ppm = 100U,
                                                     .slippage_cost_ppm = 100U,
                                                     .market_impact_ppm = 100U,
                                                     .adverse_selection_cost_ppm = 100U,
                                                     .fee_cost_ppm = 100U,
                                                     .present = true}
                   : models::TransactionCostEstimate{}}};
  forecast.stable_hash = models::stable_forecast_hash(forecast);
  return forecast;
}

[[nodiscard]] ensemble::EnsembleRequest
request(const std::uint32_t expert_count) noexcept {
  auto market = market_state::MarketStateSnapshot{
      .state = market_state::MarketState::normal,
      .primary_reason = market_state::MarketStateReason::normal_conditions_stable,
      .reason_mask = market_state::reason_bit(
          market_state::MarketStateReason::normal_conditions_stable),
      .input_sequence = 10U,
      .transition_sequence = 5U,
      .observed_process_monotonic_time_ns = kNow - 10U,
      .observed_wall_clock_utc_ns = 1'800'000'000'000'000'000ULL,
      .state_entered_process_monotonic_time_ns = kNow - 20U};
  market.stable_hash = market_state::stable_market_state_hash(market);
  ensemble::EnsembleRequest result{
      .forecast_id = common::ForecastId{60U, 60U},
      .session_id = kSession,
      .instrument_id = kInstrument,
      .configuration_version = kConfiguration,
      .horizon_ns = 1'000'000U,
      .now_process_monotonic_time_ns = kNow,
      .valid_until_process_monotonic_time_ns = kNow + 5'000U,
      .market_state_snapshot = market,
      .event_state = ensemble::EventState::none,
      .feed_health = market_state::FeedHealth::healthy,
      .input_data_quality_ppm = 900'000U,
      .expert_count = expert_count,
      .transaction_cost_forecast = model_forecast(100U, true),
      .transaction_cost_health = {.generation = 1U,
                                  .state = models::ModelHealthState::healthy},
      .transaction_cost_present = true};
  for (std::size_t index = 0U; index < expert_count; ++index) {
    result.experts[index] = {
        .forecast = model_forecast(index + 1U, false),
        .health = {.generation = 1U, .state = models::ModelHealthState::healthy},
        .role = static_cast<ensemble::ExpertRole>((index % ensemble::kExpertRoleCount) +
                                                  1U),
        .calibration_health = ensemble::CalibrationHealth::healthy};
  }
  return result;
}

void benchmark_ensemble_evaluation(benchmark::State& state) {
  const auto expert_count = static_cast<std::uint32_t>(state.range(0));
  const ensemble::MixtureOfExpertsGate gate{config()};
  const auto input = request(expert_count);
  std::uint64_t allocations_before{};
  bool measurement_started = false;
  for (auto iteration : state) {
    static_cast<void>(iteration);
    if (!measurement_started) {
      allocations_before = allocation_probe::count();
      measurement_started = true;
    }
    auto result = gate.evaluate(input);
    benchmark::DoNotOptimize(result);
  }
  const auto steady_state_allocations = allocation_probe::count() - allocations_before;
  state.SetItemsProcessed(state.iterations() * expert_count);
  state.counters["steady_state_allocations"] =
      benchmark::Counter(static_cast<double>(steady_state_allocations));
}

BENCHMARK(benchmark_ensemble_evaluation)->Arg(2)->Arg(8)->Arg(16);

} // namespace
