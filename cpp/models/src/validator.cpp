#include "aegis/models/validator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace aegis::models {
namespace {

[[nodiscard]] bool valid_snapshot_state(const features::EngineState state) noexcept {
  return state == features::EngineState::ready ||
         state == features::EngineState::degraded;
}

[[nodiscard]] bool accepted_validity(const features::FeatureValidity validity,
                                     const FeatureRequirement& requirement) noexcept {
  if (validity == features::FeatureValidity::valid) {
    return true;
  }
  if (validity == features::FeatureValidity::degraded) {
    return requirement.allow_degraded;
  }
  if (validity == features::FeatureValidity::warming) {
    return requirement.allow_warming;
  }
  return false;
}

[[nodiscard]] bool valid_score(const std::uint32_t score) noexcept {
  return score <= kProbabilityScale;
}

[[nodiscard]] bool valid_cost(const TransactionCostEstimate& cost) noexcept {
  if (!cost.present) {
    return cost.spread_cost_ppm == 0U && cost.slippage_cost_ppm == 0U &&
           cost.market_impact_ppm == 0U && cost.adverse_selection_cost_ppm == 0U &&
           cost.fee_cost_ppm == 0U;
  }
  return cost.spread_cost_ppm <= kProbabilityScale &&
         cost.slippage_cost_ppm <= kProbabilityScale &&
         cost.market_impact_ppm <= kProbabilityScale &&
         cost.adverse_selection_cost_ppm <= kProbabilityScale &&
         cost.fee_cost_ppm <= kProbabilityScale;
}

[[nodiscard]] bool finite(const ExternalForecastValues& values) noexcept {
  return std::isfinite(values.expected_return) && std::isfinite(values.return_p10) &&
         std::isfinite(values.return_p50) && std::isfinite(values.return_p90) &&
         std::isfinite(values.probability_down) &&
         std::isfinite(values.probability_flat) &&
         std::isfinite(values.probability_up) && std::isfinite(values.volatility) &&
         std::isfinite(values.confidence) && std::isfinite(values.calibration_score) &&
         std::isfinite(values.data_quality_score) && std::isfinite(values.ood_score);
}

[[nodiscard]] std::int64_t to_signed_ppm(const double value) noexcept {
  return static_cast<std::int64_t>(std::llround(value * kProbabilityScale));
}

[[nodiscard]] std::uint32_t to_score_ppm(const double value) noexcept {
  return static_cast<std::uint32_t>(std::llround(value * kProbabilityScale));
}

} // namespace

ForecastValidationError ModelForecastValidator::validate_input(
    const ModelMetadata& metadata, const ModelInput& input,
    const std::uint64_t now_process_monotonic_time_ns) noexcept {
  if (!valid_model_metadata(metadata)) {
    return ForecastValidationError::invalid_metadata;
  }
  const auto& snapshot = input.feature_snapshot;
  if (!input.forecast_id.valid() || !snapshot.snapshot_id.valid() ||
      !snapshot.instrument_id.valid() || !snapshot.session_id.valid() ||
      !snapshot.feature_definition_version.valid() ||
      !snapshot.source_first_global_event_id.valid() ||
      !snapshot.source_last_global_event_id.valid() ||
      snapshot.source_first_ordinal == 0U ||
      snapshot.source_last_ordinal < snapshot.source_first_ordinal ||
      snapshot.stable_hash == 0U ||
      snapshot.stable_hash != features::stable_snapshot_hash(snapshot) ||
      snapshot.instrument_id != metadata.instrument_id ||
      snapshot.feature_definition_version != metadata.feature_definition_version) {
    return ForecastValidationError::invalid_feature_provenance;
  }
  if (!valid_snapshot_state(snapshot.state)) {
    return ForecastValidationError::feature_invalid;
  }
  if (input.deadline.submitted_process_monotonic_time_ns == 0U ||
      input.deadline.complete_by_process_monotonic_time_ns <
          input.deadline.submitted_process_monotonic_time_ns) {
    return ForecastValidationError::invalid_deadline;
  }
  if (now_process_monotonic_time_ns == 0U ||
      now_process_monotonic_time_ns <
          input.deadline.submitted_process_monotonic_time_ns) {
    return ForecastValidationError::invalid_timestamp;
  }
  if (now_process_monotonic_time_ns >
      input.deadline.complete_by_process_monotonic_time_ns) {
    return ForecastValidationError::late;
  }
  if (snapshot.as_of_exchange_event_time_ns <= 0 ||
      snapshot.created_process_monotonic_time_ns == 0U ||
      snapshot.created_process_monotonic_time_ns >
          input.deadline.submitted_process_monotonic_time_ns ||
      now_process_monotonic_time_ns < snapshot.created_process_monotonic_time_ns) {
    return ForecastValidationError::invalid_timestamp;
  }
  for (std::size_t index = 0U; index < metadata.requirement_count; ++index) {
    const auto& requirement = metadata.requirements[index];
    const auto& feature = snapshot.get(requirement.name);
    if (feature.validity == features::FeatureValidity::missing) {
      return ForecastValidationError::feature_missing;
    }
    if (!accepted_validity(feature.validity, requirement) ||
        feature.value < requirement.minimum_value ||
        feature.value > requirement.maximum_value) {
      return ForecastValidationError::feature_invalid;
    }
    if (feature.as_of_process_monotonic_time_ns == 0U ||
        feature.as_of_process_monotonic_time_ns >
            snapshot.created_process_monotonic_time_ns ||
        now_process_monotonic_time_ns < feature.as_of_process_monotonic_time_ns) {
      return ForecastValidationError::invalid_timestamp;
    }
    if (now_process_monotonic_time_ns - feature.as_of_process_monotonic_time_ns >
        requirement.maximum_age_ns) {
      return ForecastValidationError::feature_stale;
    }
  }
  return ForecastValidationError::none;
}

ForecastValidationError ModelForecastValidator::validate_forecast(
    const ModelForecast& forecast,
    const std::uint64_t now_process_monotonic_time_ns) noexcept {
  if (!forecast.forecast_id.valid() || !forecast.session_id.valid() ||
      !forecast.model_id.valid() || !forecast.model_version.valid() ||
      !forecast.instrument_id.valid() || !forecast.feature_snapshot_id.valid() ||
      !forecast.configuration_version.valid() ||
      forecast.model_control_generation == 0U) {
    return ForecastValidationError::invalid_identity;
  }
  if (forecast.horizon_ns == 0U) {
    return ForecastValidationError::invalid_horizon;
  }
  if (forecast.as_of_exchange_event_time_ns <= 0 ||
      forecast.production_process_monotonic_time_ns == 0U ||
      forecast.expiration_process_monotonic_time_ns <=
          forecast.production_process_monotonic_time_ns) {
    return ForecastValidationError::invalid_timestamp;
  }
  if (now_process_monotonic_time_ns == 0U) {
    return ForecastValidationError::invalid_timestamp;
  }
  if (now_process_monotonic_time_ns > forecast.expiration_process_monotonic_time_ns) {
    return ForecastValidationError::expired;
  }
  const auto& prediction = forecast.prediction;
  if (prediction.expected_return_ppm < -kMaximumAbsoluteReturnPpm ||
      prediction.expected_return_ppm > kMaximumAbsoluteReturnPpm) {
    return ForecastValidationError::invalid_return;
  }
  if (prediction.return_p10_ppm > prediction.return_p50_ppm ||
      prediction.return_p50_ppm > prediction.return_p90_ppm ||
      prediction.return_p10_ppm < -kMaximumAbsoluteReturnPpm ||
      prediction.return_p90_ppm > kMaximumAbsoluteReturnPpm) {
    return ForecastValidationError::invalid_quantiles;
  }
  const auto probability_sum =
      static_cast<std::uint64_t>(prediction.probability_down_ppm) +
      static_cast<std::uint64_t>(prediction.probability_flat_ppm) +
      static_cast<std::uint64_t>(prediction.probability_up_ppm);
  if (!valid_score(prediction.probability_down_ppm) ||
      !valid_score(prediction.probability_flat_ppm) ||
      !valid_score(prediction.probability_up_ppm) ||
      probability_sum != kProbabilityScale) {
    return ForecastValidationError::invalid_probabilities;
  }
  if (prediction.volatility_ppm > kMaximumVolatilityPpm) {
    return ForecastValidationError::invalid_volatility;
  }
  if (!valid_score(prediction.confidence_ppm) ||
      !valid_score(prediction.calibration_score_ppm) ||
      !valid_score(prediction.data_quality_score_ppm) ||
      !valid_score(prediction.ood_score_ppm)) {
    return ForecastValidationError::invalid_score;
  }
  if (!valid_cost(prediction.transaction_cost)) {
    return ForecastValidationError::invalid_transaction_cost;
  }
  if (forecast.stable_hash == 0U ||
      forecast.stable_hash != stable_forecast_hash(forecast)) {
    return ForecastValidationError::invalid_hash;
  }
  return ForecastValidationError::none;
}

ForecastValidationError
ModelForecastValidator::convert_external(const ExternalForecastValues& external,
                                         ModelPrediction& output) noexcept {
  if (!finite(external)) {
    return ForecastValidationError::non_finite_value;
  }
  constexpr double kMaximumReturn = static_cast<double>(kMaximumAbsoluteReturnPpm) /
                                    static_cast<double>(kProbabilityScale);
  constexpr double kMaximumVolatility = static_cast<double>(kMaximumVolatilityPpm) /
                                        static_cast<double>(kProbabilityScale);
  const auto valid_probability = [](const double value) {
    return value >= 0.0 && value <= 1.0;
  };
  if (external.expected_return < -kMaximumReturn ||
      external.expected_return > kMaximumReturn ||
      external.return_p10 < -kMaximumReturn || external.return_p90 > kMaximumReturn ||
      external.return_p10 > external.return_p50 ||
      external.return_p50 > external.return_p90 || external.volatility < 0.0 ||
      external.volatility > kMaximumVolatility ||
      !valid_probability(external.probability_down) ||
      !valid_probability(external.probability_flat) ||
      !valid_probability(external.probability_up) ||
      !valid_probability(external.confidence) ||
      !valid_probability(external.calibration_score) ||
      !valid_probability(external.data_quality_score) ||
      !valid_probability(external.ood_score)) {
    return ForecastValidationError::invalid_score;
  }

  ModelPrediction converted{};
  converted.expected_return_ppm = to_signed_ppm(external.expected_return);
  converted.return_p10_ppm = to_signed_ppm(external.return_p10);
  converted.return_p50_ppm = to_signed_ppm(external.return_p50);
  converted.return_p90_ppm = to_signed_ppm(external.return_p90);
  converted.probability_down_ppm = to_score_ppm(external.probability_down);
  converted.probability_flat_ppm = to_score_ppm(external.probability_flat);
  converted.probability_up_ppm = to_score_ppm(external.probability_up);
  converted.volatility_ppm =
      static_cast<std::uint64_t>(std::llround(external.volatility * kProbabilityScale));
  converted.confidence_ppm = to_score_ppm(external.confidence);
  converted.calibration_score_ppm = to_score_ppm(external.calibration_score);
  converted.data_quality_score_ppm = to_score_ppm(external.data_quality_score);
  converted.ood_score_ppm = to_score_ppm(external.ood_score);

  const auto probability_sum =
      static_cast<std::uint64_t>(converted.probability_down_ppm) +
      static_cast<std::uint64_t>(converted.probability_flat_ppm) +
      static_cast<std::uint64_t>(converted.probability_up_ppm);
  if (probability_sum != kProbabilityScale) {
    return ForecastValidationError::invalid_probabilities;
  }
  output = converted;
  return ForecastValidationError::none;
}

} // namespace aegis::models
