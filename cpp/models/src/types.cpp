#include "aegis/models/types.hpp"

#include <array>
#include <bit>
#include <cstdint>

namespace aegis::models {
namespace {

void mix(std::uint64_t& hash, const std::uint64_t value) noexcept {
  constexpr std::uint64_t kPrime = 1'099'511'628'211ULL;
  for (unsigned shift = 0U; shift < 64U; shift += 8U) {
    hash ^= (value >> shift) & 0xFFU;
    hash *= kPrime;
  }
}

template <typename Tag>
void mix_identifier(std::uint64_t& hash,
                    const common::Identifier128<Tag> identifier) noexcept {
  mix(hash, identifier.high());
  mix(hash, identifier.low());
}

[[nodiscard]] bool valid_kind(const ModelKind kind) noexcept {
  return kind == ModelKind::zero_return || kind == ModelKind::last_value ||
         kind == ModelKind::moving_average || kind == ModelKind::linear_regression ||
         kind == ModelKind::logistic_regression || kind == ModelKind::seasonal_naive ||
         kind == ModelKind::external;
}

[[nodiscard]] bool valid_calibration_method(const CalibrationMethod method) noexcept {
  return method == CalibrationMethod::none || method == CalibrationMethod::isotonic ||
         method == CalibrationMethod::platt;
}

[[nodiscard]] bool valid_ood_method(const OodMethod method) noexcept {
  return method == OodMethod::none || method == OodMethod::bounded_feature_range ||
         method == OodMethod::external_score;
}

} // namespace

bool valid_model_metadata(const ModelMetadata& metadata) noexcept {
  if (!metadata.model_id.valid() || !metadata.model_version.valid() ||
      !metadata.instrument_id.valid() || !metadata.configuration_version.valid() ||
      !metadata.feature_definition_version.valid() || !valid_kind(metadata.kind) ||
      !valid_calibration_method(metadata.calibration.method) ||
      !valid_ood_method(metadata.ood.method) ||
      metadata.requirement_count > metadata.requirements.size() ||
      metadata.horizon_ns == 0U || metadata.forecast_ttl_ns == 0U ||
      metadata.calibration.expected_calibration_error_ppm > kProbabilityScale ||
      metadata.ood.reject_threshold_ppm > kProbabilityScale) {
    return false;
  }
  if (metadata.calibration.method != CalibrationMethod::none &&
      !metadata.calibration.calibration_version.valid()) {
    return false;
  }
  if (metadata.ood.method != OodMethod::none &&
      !metadata.ood.detector_version.valid()) {
    return false;
  }
  for (std::size_t index = 0U; index < metadata.requirement_count; ++index) {
    const auto& requirement = metadata.requirements[index];
    if (requirement.name >= features::FeatureName::count ||
        requirement.minimum_value > requirement.maximum_value ||
        requirement.maximum_age_ns == 0U) {
      return false;
    }
    for (std::size_t previous = 0U; previous < index; ++previous) {
      if (metadata.requirements[previous].name == requirement.name) {
        return false;
      }
    }
  }
  return true;
}

std::uint64_t stable_forecast_hash(ModelForecast forecast) noexcept {
  forecast.stable_hash = 0U;
  std::uint64_t hash = 14'695'981'039'346'656'037ULL;
  mix_identifier(hash, forecast.forecast_id);
  mix_identifier(hash, forecast.session_id);
  mix_identifier(hash, forecast.model_id);
  mix_identifier(hash, forecast.model_version);
  mix_identifier(hash, forecast.instrument_id);
  mix_identifier(hash, forecast.feature_snapshot_id);
  mix_identifier(hash, forecast.configuration_version);
  mix(hash, forecast.model_control_generation);
  mix(hash, forecast.horizon_ns);
  mix(hash, std::bit_cast<std::uint64_t>(forecast.as_of_exchange_event_time_ns));
  mix(hash, forecast.production_process_monotonic_time_ns);
  mix(hash, forecast.expiration_process_monotonic_time_ns);
  const auto& prediction = forecast.prediction;
  mix(hash, std::bit_cast<std::uint64_t>(prediction.expected_return_ppm));
  mix(hash, std::bit_cast<std::uint64_t>(prediction.return_p10_ppm));
  mix(hash, std::bit_cast<std::uint64_t>(prediction.return_p50_ppm));
  mix(hash, std::bit_cast<std::uint64_t>(prediction.return_p90_ppm));
  mix(hash, prediction.probability_down_ppm);
  mix(hash, prediction.probability_flat_ppm);
  mix(hash, prediction.probability_up_ppm);
  mix(hash, prediction.volatility_ppm);
  mix(hash, prediction.confidence_ppm);
  mix(hash, prediction.calibration_score_ppm);
  mix(hash, prediction.data_quality_score_ppm);
  mix(hash, prediction.ood_score_ppm);
  mix(hash, prediction.transaction_cost.spread_cost_ppm);
  mix(hash, prediction.transaction_cost.slippage_cost_ppm);
  mix(hash, prediction.transaction_cost.market_impact_ppm);
  mix(hash, prediction.transaction_cost.adverse_selection_cost_ppm);
  mix(hash, prediction.transaction_cost.fee_cost_ppm);
  mix(hash, prediction.transaction_cost.present ? 1U : 0U);
  return hash == 0U ? 1U : hash;
}

} // namespace aegis::models
