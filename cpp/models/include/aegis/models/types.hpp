#ifndef AEGIS_MODELS_TYPES_HPP
#define AEGIS_MODELS_TYPES_HPP

#include "aegis/common/identifiers.hpp"
#include "aegis/features/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace aegis::models {

inline constexpr std::uint32_t kProbabilityScale = 1'000'000U;
inline constexpr std::int64_t kMaximumAbsoluteReturnPpm = 10'000'000LL;
inline constexpr std::uint64_t kMaximumVolatilityPpm = 10'000'000U;
inline constexpr std::size_t kMaximumFeatureRequirements = 16U;
inline constexpr std::size_t kMaximumRegressionFeatures = 8U;
inline constexpr std::size_t kMaximumMovingAverageSamples = 64U;
inline constexpr std::size_t kMaximumSeasonalBins = 32U;

enum class ModelKind : std::uint8_t {
  zero_return = 1,
  last_value = 2,
  moving_average = 3,
  linear_regression = 4,
  logistic_regression = 5,
  seasonal_naive = 6,
  external = 7,
};

enum class ModelHealthState : std::uint8_t {
  unknown = 0,
  warming = 1,
  healthy = 2,
  degraded = 3,
  disabled = 4,
  failed = 5,
};

enum class CalibrationMethod : std::uint8_t {
  none = 0,
  isotonic = 1,
  platt = 2,
};

enum class OodMethod : std::uint8_t {
  none = 0,
  bounded_feature_range = 1,
  external_score = 2,
};

enum class PredictionStatus : std::uint8_t {
  ok = 1,
  warming = 2,
  unavailable = 3,
  invalid_input = 4,
  numeric_error = 5,
};

enum class ForecastValidationError : std::uint8_t {
  none = 0,
  invalid_metadata,
  invalid_identity,
  invalid_feature_provenance,
  feature_missing,
  feature_invalid,
  feature_stale,
  invalid_deadline,
  late,
  expired,
  invalid_horizon,
  invalid_timestamp,
  invalid_return,
  invalid_quantiles,
  invalid_probabilities,
  invalid_volatility,
  invalid_score,
  invalid_transaction_cost,
  invalid_hash,
  non_finite_value,
};

enum class RunStatus : std::uint8_t {
  accepted = 1,
  fallback_accepted,
  disabled,
  invalid_request,
  deadline_missed,
  model_failure,
  invalid_forecast,
  fallback_failure,
  queue_full,
  no_result,
  late_response_discarded,
  stopped,
};

enum class FallbackPolicy : std::uint8_t {
  fail_closed = 1,
  explicit_model_on_failure = 2,
};

enum class CacheStatus : std::uint8_t {
  ok = 1,
  invalid_forecast,
  full,
  miss,
  expired,
  corrupt,
};

enum class RegistryStatus : std::uint8_t {
  ok = 1,
  not_found,
  unavailable,
  invalid_response,
};

// Numeric feature limits are part of model metadata and prevent unchecked
// fixed-point regression arithmetic. Units are defined by FeatureName.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct FeatureRequirement {
  features::FeatureName name{features::FeatureName::count};
  std::int64_t minimum_value{};
  std::int64_t maximum_value{};
  std::uint64_t maximum_age_ns{};
  bool allow_degraded{false};
  bool allow_warming{false};
};

struct CalibrationMetadata {
  CalibrationMethod method{CalibrationMethod::none};
  common::ConfigurationVersion calibration_version;
  std::uint64_t fitted_sample_count{};
  std::uint32_t expected_calibration_error_ppm{};
};

struct OODMetadata {
  OodMethod method{OodMethod::none};
  common::ConfigurationVersion detector_version;
  std::uint32_t reject_threshold_ppm{kProbabilityScale};
};

struct ModelMetadata {
  common::ModelId model_id;
  common::ModelVersion model_version;
  common::InstrumentId instrument_id;
  common::ConfigurationVersion configuration_version;
  common::ConfigurationVersion feature_definition_version;
  ModelKind kind{ModelKind::external};
  std::array<FeatureRequirement, kMaximumFeatureRequirements> requirements{};
  std::uint32_t requirement_count{};
  std::uint64_t horizon_ns{};
  std::uint64_t forecast_ttl_ns{};
  CalibrationMetadata calibration;
  OODMetadata ood;
};

struct ModelDeadline {
  std::uint64_t submitted_process_monotonic_time_ns{};
  std::uint64_t complete_by_process_monotonic_time_ns{};
};

// Execution-context values are advisory model inputs only. They cannot carry an
// order or bypass ensemble/risk. The presence bit prevents a missing queue
// context from being interpreted as a valid zero-valued context.
struct MicrostructureContext {
  std::uint64_t quantity_ahead_units{};
  std::uint64_t order_quantity_units{};
  std::uint64_t order_age_ns{};
  std::int64_t price_distance_ticks{};
  std::uint32_t venue_number{};
  std::uint32_t fee_rate_ppm{};
  bool present{false};
};

struct ModelInput {
  common::ForecastId forecast_id;
  features::FeatureSnapshot feature_snapshot;
  ModelDeadline deadline;
  MicrostructureContext microstructure_context;
};

struct TransactionCostEstimate {
  std::uint64_t spread_cost_ppm{};
  std::uint64_t slippage_cost_ppm{};
  std::uint64_t market_impact_ppm{};
  std::uint64_t adverse_selection_cost_ppm{};
  std::uint64_t fee_cost_ppm{};
  bool present{false};
};

struct ModelPrediction {
  std::int64_t expected_return_ppm{};
  std::int64_t return_p10_ppm{};
  std::int64_t return_p50_ppm{};
  std::int64_t return_p90_ppm{};
  std::uint32_t probability_down_ppm{};
  std::uint32_t probability_flat_ppm{};
  std::uint32_t probability_up_ppm{};
  std::uint64_t volatility_ppm{};
  std::uint32_t confidence_ppm{};
  std::uint32_t calibration_score_ppm{};
  std::uint32_t data_quality_score_ppm{};
  std::uint32_t ood_score_ppm{};
  TransactionCostEstimate transaction_cost;
};

struct ModelForecast {
  common::ForecastId forecast_id;
  common::SessionId session_id;
  common::ModelId model_id;
  common::ModelVersion model_version;
  common::InstrumentId instrument_id;
  common::FeatureSnapshotId feature_snapshot_id;
  common::ConfigurationVersion configuration_version;
  std::uint64_t model_control_generation{};
  std::uint64_t horizon_ns{};
  std::int64_t as_of_exchange_event_time_ns{};
  std::uint64_t production_process_monotonic_time_ns{};
  std::uint64_t expiration_process_monotonic_time_ns{};
  ModelPrediction prediction;
  std::uint64_t stable_hash{};
};

struct ModelControlUpdate {
  std::uint64_t generation{};
  ModelHealthState state{ModelHealthState::unknown};
};

struct ModelHealthSnapshot {
  std::uint64_t generation{};
  ModelHealthState state{ModelHealthState::unknown};
};

struct RunResult {
  RunStatus status{RunStatus::no_result};
  ForecastValidationError validation_error{ForecastValidationError::none};
  ModelForecast forecast;
  bool used_fallback{false};
};

struct ExternalForecastValues {
  double expected_return{};
  double return_p10{};
  double return_p50{};
  double return_p90{};
  double probability_down{};
  double probability_flat{};
  double probability_up{};
  double volatility{};
  double confidence{};
  double calibration_score{};
  double data_quality_score{};
  double ood_score{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

using MonotonicClockFunction = std::uint64_t (*)(void*) noexcept;

class MonotonicClockSource final {
public:
  constexpr MonotonicClockSource(MonotonicClockFunction function = nullptr,
                                 void* context = nullptr) noexcept
      : function_(function), context_(context) {}

  [[nodiscard]] std::uint64_t now_ns() const noexcept {
    return function_ == nullptr ? 0U : function_(context_);
  }

  [[nodiscard]] constexpr bool valid() const noexcept { return function_ != nullptr; }

private:
  MonotonicClockFunction function_{};
  void* context_{};
};

[[nodiscard]] bool valid_model_metadata(const ModelMetadata& metadata) noexcept;
[[nodiscard]] std::uint64_t stable_forecast_hash(ModelForecast forecast) noexcept;

static_assert(std::is_trivially_copyable_v<ModelInput>);
static_assert(std::is_trivially_copyable_v<ModelForecast>);
static_assert(std::is_trivially_copyable_v<RunResult>);

} // namespace aegis::models

#endif // AEGIS_MODELS_TYPES_HPP
