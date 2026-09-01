#ifndef AEGIS_ENSEMBLE_TYPES_HPP
#define AEGIS_ENSEMBLE_TYPES_HPP

#include "aegis/market_state/controller.hpp"
#include "aegis/models/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace aegis::ensemble {

inline constexpr std::size_t kMaximumExperts = 16U;
inline constexpr std::size_t kExpertRoleCount = 7U;
inline constexpr std::size_t kEventStateCount = 5U;
inline constexpr std::size_t kMarketStateCount = market_state::kMarketStateCount;
inline constexpr std::uint32_t kWeightScale = models::kProbabilityScale;
// Freshness scaling multiplies a nanosecond age by kWeightScale. Bound operator
// configuration so that multiplication remains defined in uint64_t.
inline constexpr std::uint64_t kMaximumScaledDurationNs =
    std::numeric_limits<std::uint64_t>::max() / kWeightScale;
inline constexpr std::uint64_t kMaximumAggregateCostPpm = 5'000'000U;
inline constexpr std::uint64_t kMaximumEffectiveUncertaintyPpm = 30'000'000U;
inline constexpr std::int64_t kMaximumNetRobustEdgePpm = 30'000'000;
inline constexpr std::int64_t kMaximumGateCoefficientPpm = 10'000'000;

enum class GateKind : std::uint8_t {
  rule_based = 1,
  linear = 2,
  quantized_learned = 3,
};

enum class ExpertRole : std::uint8_t {
  generic = 1,
  microstructure = 2,
  timeseries = 3,
  event = 4,
  options = 5,
  macro = 6,
  auction = 7,
};

enum class EventState : std::uint8_t {
  none = 1,
  scheduled = 2,
  breaking_news = 3,
  price_discovery = 4,
  volatility_shock = 5,
};

enum class CalibrationHealth : std::uint8_t {
  unknown = 0,
  healthy = 1,
  degraded = 2,
  failed = 3,
};

enum class EligibilityReason : std::uint8_t {
  eligible = 1,
  missing_provenance = 2,
  expired = 3,
  model_disabled = 4,
  invalid_input_feed = 5,
  incompatible_state = 6,
  excessive_ood = 7,
  calibration_failed = 8,
  data_quality_failed = 9,
  invalid_forecast = 10,
  scope_mismatch = 11,
  control_generation_mismatch = 12,
  duplicate_expert = 13,
};

enum class DecisionReason : std::uint8_t {
  combined = 1,
  invalid_configuration = 2,
  invalid_request = 3,
  unsafe_market_state = 4,
  invalid_input_feed = 5,
  invalid_transaction_cost = 6,
  no_eligible_expert = 7,
  weight_cap_infeasible = 8,
  no_positive_gate_score = 9,
  insufficient_robust_edge = 10,
  numeric_error = 11,
};

enum class DominantExpertReason : std::uint8_t {
  highest_weight = 1,
  canonical_tie_break = 2,
  only_eligible_expert = 3,
  no_eligible_expert = 4,
};

enum class EvaluationStatus : std::uint8_t {
  produced = 1,
  abstained = 2,
  invalid_configuration = 3,
  invalid_request = 4,
};

// Fixed-layout configuration and decision values keep the gate allocation-free.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct ExpertRolePolicy {
  ExpertRole role{ExpertRole::generic};
  std::uint16_t allowed_market_state_mask{};
  std::uint8_t allowed_event_state_mask{};
  std::uint32_t base_multiplier_ppm{kWeightScale};
};

struct LinearGateParameters {
  std::int64_t intercept_ppm{kWeightScale};
  std::int64_t confidence_coefficient_ppm{};
  std::int64_t calibration_coefficient_ppm{};
  std::int64_t data_quality_coefficient_ppm{};
  std::int64_t inverse_ood_coefficient_ppm{};
  std::int64_t freshness_coefficient_ppm{};
};

struct QuantizedLearnedGateArtifact {
  common::ModelId gate_model_id;
  common::ModelVersion gate_model_version;
  std::uint64_t signature_hash{};
  LinearGateParameters coefficients;
  std::array<std::int64_t, kExpertRoleCount> role_bias_ppm{};
  std::array<std::int64_t, kMarketStateCount> market_state_bias_ppm{};
  std::array<std::int64_t, kEventStateCount> event_state_bias_ppm{};
  std::uint64_t stable_hash{};
};

struct EnsembleConfig {
  common::ModelId ensemble_model_id;
  common::ModelVersion ensemble_model_version;
  common::ConfigurationVersion configuration_version;
  GateKind gate_kind{GateKind::rule_based};
  std::uint32_t maximum_expert_weight_ppm{kWeightScale};
  std::uint32_t ood_reject_threshold_ppm{kWeightScale};
  std::uint32_t minimum_calibration_score_ppm{};
  std::uint32_t minimum_data_quality_score_ppm{};
  std::uint64_t maximum_forecast_age_ns{};
  std::uint64_t maximum_market_state_age_ns{};
  std::uint32_t degraded_feed_multiplier_ppm{kWeightScale};
  std::uint32_t degraded_calibration_multiplier_ppm{kWeightScale};
  std::uint32_t timeseries_breaking_news_multiplier_ppm{kWeightScale};
  std::uint32_t uncertainty_penalty_multiplier_ppm{kWeightScale};
  std::uint32_t calibration_uncertainty_multiplier_ppm{};
  std::uint32_t ood_uncertainty_multiplier_ppm{};
  std::uint32_t data_quality_uncertainty_multiplier_ppm{};
  std::uint64_t safety_margin_ppm{};
  std::array<ExpertRolePolicy, kExpertRoleCount> role_policies{};
  LinearGateParameters linear;
  QuantizedLearnedGateArtifact learned;
  std::uint64_t stable_hash{};
};

struct ExpertInput {
  models::ModelForecast forecast;
  models::ModelHealthSnapshot health;
  ExpertRole role{ExpertRole::generic};
  CalibrationHealth calibration_health{CalibrationHealth::unknown};
};

struct EnsembleRequest {
  common::ForecastId forecast_id;
  common::SessionId session_id;
  common::InstrumentId instrument_id;
  common::ConfigurationVersion configuration_version;
  std::uint64_t horizon_ns{};
  std::uint64_t now_process_monotonic_time_ns{};
  std::uint64_t valid_until_process_monotonic_time_ns{};
  market_state::MarketStateSnapshot market_state_snapshot;
  EventState event_state{EventState::none};
  market_state::FeedHealth feed_health{market_state::FeedHealth::unknown};
  std::uint32_t input_data_quality_ppm{};
  std::array<ExpertInput, kMaximumExperts> experts{};
  std::uint32_t expert_count{};
  models::ModelForecast transaction_cost_forecast;
  models::ModelHealthSnapshot transaction_cost_health;
  bool transaction_cost_present{false};
};

struct ExpertExplanation {
  common::ForecastId forecast_id;
  common::ModelId model_id;
  common::ModelVersion model_version;
  ExpertRole role{ExpertRole::generic};
  EligibilityReason eligibility{EligibilityReason::invalid_forecast};
  std::uint32_t weight_ppm{};
  std::uint64_t raw_gate_score{};
  std::uint32_t freshness_ppm{};
  std::uint64_t forecast_hash{};
};

struct DecisionExplanation {
  GateKind gate_kind{GateKind::rule_based};
  market_state::MarketState market_state{market_state::MarketState::startup};
  EventState event_state{EventState::none};
  std::uint32_t input_data_quality_ppm{};
  std::uint32_t supplied_expert_count{};
  std::uint32_t eligible_expert_count{};
  std::uint32_t contributing_expert_count{};
  common::ForecastId dominant_forecast_id;
  common::ModelId dominant_model_id;
  common::ModelVersion dominant_model_version;
  DominantExpertReason dominant_reason{DominantExpertReason::no_eligible_expert};
  std::uint64_t transaction_cost_forecast_hash{};
  std::uint64_t market_state_snapshot_hash{};
  std::uint64_t configuration_hash{};
  std::uint64_t learned_gate_signature_hash{};
  std::uint64_t required_edge_ppm{};
  std::array<ExpertExplanation, kMaximumExperts> experts{};
  std::uint64_t stable_hash{};
};

struct EnsembleForecast {
  common::ForecastId forecast_id;
  common::SessionId session_id;
  common::InstrumentId instrument_id;
  common::ModelId ensemble_model_id;
  common::ModelVersion ensemble_model_version;
  common::ConfigurationVersion configuration_version;
  std::uint64_t horizon_ns{};
  std::uint64_t created_process_monotonic_time_ns{};
  std::uint64_t valid_until_process_monotonic_time_ns{};
  std::int64_t expected_return_ppm{};
  std::uint32_t probability_down_ppm{};
  std::uint32_t probability_flat_ppm{kWeightScale};
  std::uint32_t probability_up_ppm{};
  std::uint64_t variance_ppm_squared{};
  std::int64_t return_p10_ppm{};
  std::int64_t return_p50_ppm{};
  std::int64_t return_p90_ppm{};
  std::uint64_t effective_uncertainty_ppm{};
  std::uint64_t disagreement_ppm{};
  std::uint64_t estimated_transaction_cost_ppm{};
  std::uint64_t uncertainty_penalty_ppm{};
  std::uint64_t safety_margin_ppm{};
  std::int64_t net_robust_edge_ppm{};
  bool abstain{true};
  DecisionReason primary_reason{DecisionReason::invalid_request};
  std::uint64_t reason_mask{};
  DecisionExplanation explanation;
  std::uint64_t stable_hash{};
};

struct EvaluationResult {
  EvaluationStatus status{EvaluationStatus::invalid_request};
  EnsembleForecast forecast;
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] bool valid_gate_kind(GateKind kind) noexcept;
[[nodiscard]] bool valid_expert_role(ExpertRole role) noexcept;
[[nodiscard]] bool valid_event_state(EventState state) noexcept;
[[nodiscard]] bool valid_ensemble_config(const EnsembleConfig& config) noexcept;
[[nodiscard]] bool valid_ensemble_forecast(const EnsembleForecast& forecast) noexcept;
[[nodiscard]] std::uint64_t reason_bit(DecisionReason reason) noexcept;
[[nodiscard]] std::uint64_t
stable_learned_gate_hash(QuantizedLearnedGateArtifact artifact) noexcept;
[[nodiscard]] std::uint64_t stable_ensemble_config_hash(EnsembleConfig config) noexcept;
[[nodiscard]] std::uint64_t
stable_explanation_hash(DecisionExplanation explanation) noexcept;
[[nodiscard]] std::uint64_t
stable_ensemble_forecast_hash(EnsembleForecast forecast) noexcept;

static_assert(std::is_trivially_copyable_v<EnsembleRequest>);
static_assert(std::is_trivially_copyable_v<EnsembleForecast>);

} // namespace aegis::ensemble

#endif // AEGIS_ENSEMBLE_TYPES_HPP
