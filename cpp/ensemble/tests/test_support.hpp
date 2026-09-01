#ifndef AEGIS_ENSEMBLE_TEST_SUPPORT_HPP
#define AEGIS_ENSEMBLE_TEST_SUPPORT_HPP

#include "aegis/ensemble/gate.hpp"

#include <cstddef>
#include <cstdint>

namespace aegis::ensemble::test {

inline constexpr common::InstrumentId kInstrument{1U, 1U};
inline constexpr common::SessionId kSession{2U, 2U};
inline constexpr common::ConfigurationVersion kConfiguration{3U, 3U};
inline constexpr std::uint64_t kNow = 1'000'000U;
inline constexpr std::uint64_t kHorizon = 1'000'000U;

[[nodiscard]] inline market_state::MarketStateSnapshot market_snapshot(
    const market_state::MarketState state = market_state::MarketState::normal,
    const std::uint64_t observed_ns = kNow - 10U) noexcept {
  market_state::MarketStateSnapshot snapshot{
      .state = state,
      .primary_reason = market_state::MarketStateReason::normal_conditions_stable,
      .reason_mask = market_state::reason_bit(
          market_state::MarketStateReason::normal_conditions_stable),
      .input_sequence = 10U,
      .transition_sequence = 5U,
      .observed_process_monotonic_time_ns = observed_ns,
      .observed_wall_clock_utc_ns = 1'800'000'000'000'000'000ULL,
      .state_entered_process_monotonic_time_ns = observed_ns - 1U};
  snapshot.stable_hash = market_state::stable_market_state_hash(snapshot);
  return snapshot;
}

[[nodiscard]] inline EnsembleConfig
config(const GateKind kind = GateKind::rule_based) noexcept {
  EnsembleConfig result{.ensemble_model_id = common::ModelId{10U, 10U},
                        .ensemble_model_version = common::ModelVersion{11U, 11U},
                        .configuration_version = kConfiguration,
                        .gate_kind = kind,
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
                        .linear = {.intercept_ppm = 0,
                                   .confidence_coefficient_ppm = 400'000,
                                   .calibration_coefficient_ppm = 300'000,
                                   .data_quality_coefficient_ppm = 200'000,
                                   .inverse_ood_coefficient_ppm = 100'000,
                                   .freshness_coefficient_ppm = 100'000},
                        .learned = {},
                        .stable_hash = 0U};
  constexpr auto kAllMarketStates =
      static_cast<std::uint16_t>((std::uint32_t{1U} << kMarketStateCount) - 1U);
  constexpr auto kAllEventStates =
      static_cast<std::uint8_t>((std::uint32_t{1U} << kEventStateCount) - 1U);
  for (std::size_t index = 0U; index < result.role_policies.size(); ++index) {
    result.role_policies[index] = {.role = static_cast<ExpertRole>(index + 1U),
                                   .allowed_market_state_mask = kAllMarketStates,
                                   .allowed_event_state_mask = kAllEventStates,
                                   .base_multiplier_ppm = kWeightScale};
  }
  if (kind == GateKind::quantized_learned) {
    result.learned.gate_model_id = common::ModelId{12U, 12U};
    result.learned.gate_model_version = common::ModelVersion{13U, 13U};
    result.learned.signature_hash = 14U;
    result.learned.coefficients = result.linear;
    result.learned.stable_hash = stable_learned_gate_hash(result.learned);
  }
  result.stable_hash = stable_ensemble_config_hash(result);
  return result;
}

// Compact deterministic fixture arguments are kept in their documented order.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[nodiscard]] inline models::ModelForecast
expert_forecast(const std::uint64_t ordinal,
                const std::int64_t expected_return_ppm = 50'000,
                const std::uint64_t production_ns = kNow - 100U,
                const std::uint64_t expiration_ns = kNow + 10'000U) noexcept {
  models::ModelForecast forecast{
      .forecast_id = common::ForecastId{20U, ordinal},
      .session_id = kSession,
      .model_id = common::ModelId{30U, ordinal},
      .model_version = common::ModelVersion{40U, ordinal},
      .instrument_id = kInstrument,
      .feature_snapshot_id = common::FeatureSnapshotId{50U, ordinal},
      .configuration_version = kConfiguration,
      .model_control_generation = 1U,
      .horizon_ns = kHorizon,
      .as_of_exchange_event_time_ns = 1'800'000'000'000'000'000LL,
      .production_process_monotonic_time_ns = production_ns,
      .expiration_process_monotonic_time_ns = expiration_ns,
      .prediction = {
          .expected_return_ppm = expected_return_ppm,
          .return_p10_ppm = expected_return_ppm - 4'000,
          .return_p50_ppm = expected_return_ppm,
          .return_p90_ppm = expected_return_ppm + 4'000,
          .probability_down_ppm = expected_return_ppm < 0 ? 650'000U : 200'000U,
          .probability_flat_ppm = 150'000U,
          .probability_up_ppm = expected_return_ppm < 0 ? 200'000U : 650'000U,
          .volatility_ppm = 3'000U,
          .confidence_ppm = 900'000U,
          .calibration_score_ppm = 900'000U,
          .data_quality_score_ppm = 900'000U,
          .ood_score_ppm = 20'000U,
          .transaction_cost = {}}};
  forecast.stable_hash = models::stable_forecast_hash(forecast);
  return forecast;
}

[[nodiscard]] inline models::ModelForecast cost_forecast() noexcept {
  auto forecast = expert_forecast(100U, 0);
  forecast.model_id = common::ModelId{60U, 100U};
  forecast.model_version = common::ModelVersion{61U, 100U};
  forecast.forecast_id = common::ForecastId{62U, 100U};
  forecast.feature_snapshot_id = common::FeatureSnapshotId{63U, 100U};
  forecast.prediction.return_p10_ppm = 0;
  forecast.prediction.return_p90_ppm = 0;
  forecast.prediction.probability_down_ppm = 0U;
  forecast.prediction.probability_flat_ppm = kWeightScale;
  forecast.prediction.probability_up_ppm = 0U;
  forecast.prediction.transaction_cost = {.spread_cost_ppm = 100U,
                                          .slippage_cost_ppm = 100U,
                                          .market_impact_ppm = 100U,
                                          .adverse_selection_cost_ppm = 100U,
                                          .fee_cost_ppm = 100U,
                                          .present = true};
  forecast.stable_hash = models::stable_forecast_hash(forecast);
  return forecast;
}

[[nodiscard]] inline EnsembleRequest
request(const std::uint32_t expert_count = 2U,
        const market_state::MarketState state = market_state::MarketState::normal,
        const EventState event_state = EventState::none) noexcept {
  EnsembleRequest result{
      .forecast_id = common::ForecastId{70U, 70U},
      .session_id = kSession,
      .instrument_id = kInstrument,
      .configuration_version = kConfiguration,
      .horizon_ns = kHorizon,
      .now_process_monotonic_time_ns = kNow,
      .valid_until_process_monotonic_time_ns = kNow + 5'000U,
      .market_state_snapshot = market_snapshot(state),
      .event_state = event_state,
      .feed_health = market_state::FeedHealth::healthy,
      .input_data_quality_ppm = 900'000U,
      .expert_count = expert_count,
      .transaction_cost_forecast = cost_forecast(),
      .transaction_cost_health = {.generation = 1U,
                                  .state = models::ModelHealthState::healthy},
      .transaction_cost_present = true};
  for (std::size_t index = 0U; index < expert_count; ++index) {
    result.experts[index] = {
        .forecast = expert_forecast(index + 1U),
        .health = {.generation = 1U, .state = models::ModelHealthState::healthy},
        .role = index == 0U ? ExpertRole::microstructure : ExpertRole::timeseries,
        .calibration_health = CalibrationHealth::healthy};
  }
  return result;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

[[nodiscard]] inline const ExpertExplanation*
find_model(const DecisionExplanation& explanation,
           const common::ModelId model_id) noexcept {
  for (std::size_t index = 0U; index < explanation.supplied_expert_count; ++index) {
    if (explanation.experts[index].model_id == model_id) {
      return &explanation.experts[index];
    }
  }
  return nullptr;
}

} // namespace aegis::ensemble::test

#endif // AEGIS_ENSEMBLE_TEST_SUPPORT_HPP
