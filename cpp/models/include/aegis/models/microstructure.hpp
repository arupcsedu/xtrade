#ifndef AEGIS_MODELS_MICROSTRUCTURE_HPP
#define AEGIS_MODELS_MICROSTRUCTURE_HPP

#include "aegis/common/sha256.hpp"
#include "aegis/models/model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace aegis::models {

inline constexpr std::size_t kMaximumNativeModelFeatures = 16U;

enum class MicrostructureModelRole : std::uint8_t {
  mid_price_direction = 1,
  spread_widening = 2,
  queue_depletion = 3,
  passive_fill_probability = 4,
  adverse_selection = 5,
  transaction_cost = 6,
  market_impact = 7,
};

enum class NativeModelFamily : std::uint8_t {
  logistic_regression = 1,
  linear_regression = 2,
  gradient_boosted_trees = 3,
};

// Values below FeatureName::count map directly to FeatureSnapshot. Context
// values start at 128 to keep serialized feature ordering unambiguous.
enum class NativeFeatureName : std::uint8_t {
  midpoint_half_ticks = 0,
  spread_ticks = 1,
  relative_spread_ppm = 2,
  microprice_ticks_ppm = 3,
  top_level_imbalance_ppm = 4,
  multi_level_weighted_imbalance_ppm = 5,
  order_flow_imbalance_units = 6,
  signed_trade_imbalance_ppm = 7,
  add_rate_millihertz = 8,
  cancel_rate_millihertz = 9,
  execute_rate_millihertz = 10,
  queue_depletion_rate_units_per_second = 11,
  rolling_return_ppm = 12,
  realized_volatility_ppm = 13,
  volume_units = 14,
  vwap_ticks_ppm = 15,
  trade_intensity_millihertz = 16,
  quote_intensity_millihertz = 17,
  cross_venue_divergence_half_ticks = 18,
  bid_leader_venue_number = 19,
  ask_leader_venue_number = 20,
  venue_leadership_share_ppm = 21,
  replenishment_rate_millihertz = 22,
  data_quality_code = 23,
  data_age_ns = 24,
  session_progress_ppm = 25,
  quantity_ahead_units = 128,
  order_quantity_units = 129,
  order_age_ns = 130,
  price_distance_ticks = 131,
  venue_number = 132,
  fee_rate_ppm = 133,
};

enum class NativeArtifactError : std::uint8_t {
  none = 0,
  malformed,
  unsupported_version,
  unsupported_family,
  invalid_role,
  invalid_identity,
  invalid_feature,
  duplicate_feature,
  capacity_exhausted,
  invalid_normalization,
  invalid_calibration,
  invalid_cost_shares,
  expired,
  invalid_signature,
  invalid_metadata,
};

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct NativeFeatureTransform {
  NativeFeatureName name{NativeFeatureName::midpoint_half_ticks};
  std::int64_t center{};
  std::uint64_t scale{1U};
  std::int64_t training_minimum{};
  std::int64_t training_maximum{};
  std::int64_t weight_ppm{};
};

struct NativeModelArtifact {
  ModelMetadata metadata;
  MicrostructureModelRole role{MicrostructureModelRole::mid_price_direction};
  NativeModelFamily family{NativeModelFamily::logistic_regression};
  std::array<NativeFeatureTransform, kMaximumNativeModelFeatures> features{};
  std::uint32_t feature_count{};
  std::int64_t intercept_ppm{};
  std::int64_t platt_slope_ppm{kProbabilityScale};
  std::int64_t platt_intercept_ppm{};
  std::uint64_t training_sample_count{};
  std::uint32_t calibration_error_ppm{};
  std::uint32_t fee_share_ppm{};
  std::uint32_t spread_share_ppm{};
  std::uint32_t slippage_share_ppm{};
  std::uint32_t impact_share_ppm{};
  std::uint32_t adverse_selection_share_ppm{};
  std::int64_t expires_wall_clock_utc_ns{};
  std::uint64_t activation_expiration_process_monotonic_time_ns{};
  common::Sha256Digest signature_sha256{};
};

struct NativeArtifactLoadResult {
  NativeArtifactError error{NativeArtifactError::none};
  NativeModelArtifact artifact;

  [[nodiscard]] constexpr bool ok() const noexcept {
    return error == NativeArtifactError::none;
  }
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] NativeArtifactLoadResult load_native_model_artifact(
    std::string_view encoded, std::int64_t now_wall_clock_utc_ns,
    std::uint64_t activation_expiration_process_monotonic_time_ns) noexcept;

[[nodiscard]] std::string_view native_feature_name(NativeFeatureName name) noexcept;
[[nodiscard]] std::string_view
microstructure_role_name(MicrostructureModelRole role) noexcept;
[[nodiscard]] std::uint32_t deterministic_sigmoid_ppm(std::int64_t score_ppm) noexcept;

class NativeMicrostructureModel final : public IForecastModel {
public:
  explicit NativeMicrostructureModel(NativeModelArtifact artifact) noexcept
      : artifact_(artifact) {}

  [[nodiscard]] const ModelMetadata& metadata() const noexcept override {
    return artifact_.metadata;
  }

  [[nodiscard]] PredictionStatus predict(const ModelInput& input,
                                         ModelPrediction& output) noexcept override;

  [[nodiscard]] const NativeModelArtifact& artifact() const noexcept {
    return artifact_;
  }

private:
  NativeModelArtifact artifact_;
};

static_assert(std::is_trivially_copyable_v<NativeModelArtifact>);

} // namespace aegis::models

#endif // AEGIS_MODELS_MICROSTRUCTURE_HPP
