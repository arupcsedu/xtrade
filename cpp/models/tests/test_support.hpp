#ifndef AEGIS_MODELS_TEST_SUPPORT_HPP
#define AEGIS_MODELS_TEST_SUPPORT_HPP

#include "aegis/features/types.hpp"
#include "aegis/models/types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace aegis::models::test {

inline constexpr common::InstrumentId kInstrument{1U, 1U};
inline constexpr common::SessionId kSession{2U, 2U};
inline constexpr common::ModelId kModelId{3U, 3U};
inline constexpr common::ModelVersion kModelVersion{4U, 4U};
inline constexpr common::ConfigurationVersion kConfiguration{5U, 5U};

[[nodiscard]] inline ModelMetadata
metadata(const ModelKind kind = ModelKind::zero_return,
         const common::ModelId model_id = kModelId) noexcept {
  ModelMetadata result{.model_id = model_id,
                       .model_version = kModelVersion,
                       .instrument_id = kInstrument,
                       .configuration_version = kConfiguration,
                       .feature_definition_version =
                           common::ConfigurationVersion{11U, 11U},
                       .kind = kind,
                       .requirement_count = 2U,
                       .horizon_ns = 1'000'000U,
                       .forecast_ttl_ns = 500U,
                       .calibration = {},
                       .ood = {}};
  result.requirements[0U] = {.name = features::FeatureName::rolling_return_ppm,
                             .minimum_value = -kMaximumAbsoluteReturnPpm,
                             .maximum_value = kMaximumAbsoluteReturnPpm,
                             .maximum_age_ns = 1'000U};
  result.requirements[1U] = {.name = features::FeatureName::realized_volatility_ppm,
                             .minimum_value = 0,
                             .maximum_value =
                                 static_cast<std::int64_t>(kMaximumVolatilityPpm),
                             .maximum_age_ns = 1'000U};
  return result;
}

// Compact deterministic fixture arguments are kept in their documented order.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[nodiscard]] inline features::FeatureSnapshot
snapshot(const std::int64_t return_ppm = 2'000,
         const std::int64_t volatility_ppm = 3'000,
         const std::uint64_t created_ns = 100U) noexcept {
  features::FeatureSnapshot result{
      .snapshot_id = common::FeatureSnapshotId{10U, 10U},
      .instrument_id = kInstrument,
      .session_id = kSession,
      .feature_definition_version = common::ConfigurationVersion{11U, 11U},
      .source_first_global_event_id = common::GlobalEventId{12U, 12U},
      .source_last_global_event_id = common::GlobalEventId{13U, 13U},
      .source_first_ordinal = 1U,
      .source_last_ordinal = 2U,
      .as_of_exchange_event_time_ns = 1'800'000'000'000'000'000LL,
      .created_process_monotonic_time_ns = created_ns,
      .stable_hash = 99U,
      .state = features::EngineState::ready};
  for (auto& feature : result.values) {
    feature = {.value = 0,
               .as_of_process_monotonic_time_ns = created_ns,
               .validity = features::FeatureValidity::valid};
  }
  result.values[static_cast<std::size_t>(features::FeatureName::rolling_return_ppm)]
      .value = return_ppm;
  result
      .values[static_cast<std::size_t>(features::FeatureName::realized_volatility_ppm)]
      .value = volatility_ppm;
  result.values[static_cast<std::size_t>(features::FeatureName::session_progress_ppm)]
      .value = 250'000;
  result.stable_hash = features::stable_snapshot_hash(result);
  return result;
}

[[nodiscard]] inline ModelInput input(const std::int64_t return_ppm = 2'000,
                                      const std::uint64_t submitted_ns = 100U,
                                      const std::uint64_t deadline_ns = 200U) noexcept {
  return {.forecast_id = common::ForecastId{20U, submitted_ns},
          .feature_snapshot = snapshot(return_ppm, 3'000, submitted_ns),
          .deadline = {.submitted_process_monotonic_time_ns = submitted_ns,
                       .complete_by_process_monotonic_time_ns = deadline_ns},
          .microstructure_context = {}};
}

[[nodiscard]] inline std::uint64_t atomic_clock(void* context) noexcept {
  return static_cast<std::atomic<std::uint64_t>*>(context)->load(
      std::memory_order_relaxed);
}

[[nodiscard]] inline ModelForecast
valid_forecast(const std::uint64_t production_ns = 110U,
               const std::uint64_t expiration_ns = 500U) noexcept {
  ModelForecast result{.forecast_id = common::ForecastId{20U, 20U},
                       .session_id = kSession,
                       .model_id = kModelId,
                       .model_version = kModelVersion,
                       .instrument_id = kInstrument,
                       .feature_snapshot_id = common::FeatureSnapshotId{10U, 10U},
                       .configuration_version = kConfiguration,
                       .model_control_generation = 1U,
                       .horizon_ns = 1'000'000U,
                       .as_of_exchange_event_time_ns = 1'800'000'000'000'000'000LL,
                       .production_process_monotonic_time_ns = production_ns,
                       .expiration_process_monotonic_time_ns = expiration_ns,
                       .prediction = {.expected_return_ppm = 1'000,
                                      .return_p10_ppm = -2'000,
                                      .return_p50_ppm = 1'000,
                                      .return_p90_ppm = 4'000,
                                      .probability_down_ppm = 250'000U,
                                      .probability_flat_ppm = 150'000U,
                                      .probability_up_ppm = 600'000U,
                                      .volatility_ppm = 3'000U,
                                      .confidence_ppm = 800'000U,
                                      .calibration_score_ppm = 900'000U,
                                      .data_quality_score_ppm = 1'000'000U,
                                      .ood_score_ppm = 10'000U,
                                      .transaction_cost = {}}};
  result.stable_hash = stable_forecast_hash(result);
  return result;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

} // namespace aegis::models::test

#endif // AEGIS_MODELS_TEST_SUPPORT_HPP
