#include "aegis/ensemble/types.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace aegis::ensemble {
namespace {

constexpr std::uint64_t kFnvOffset = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;

void mix(std::uint64_t& hash, const std::uint64_t value) noexcept {
  for (unsigned shift = 0U; shift < 64U; shift += 8U) {
    hash ^= (value >> shift) & 0xFFU;
    hash *= kFnvPrime;
  }
}

template <typename Tag>
void mix_identifier(std::uint64_t& hash,
                    const common::Identifier128<Tag> identifier) noexcept {
  mix(hash, identifier.high());
  mix(hash, identifier.low());
}

[[nodiscard]] std::uint64_t nonzero_hash(const std::uint64_t hash) noexcept {
  return hash == 0U ? 1U : hash;
}

[[nodiscard]] bool valid_eligibility_reason(const EligibilityReason reason) noexcept {
  return reason >= EligibilityReason::eligible &&
         reason <= EligibilityReason::duplicate_expert;
}

[[nodiscard]] bool valid_decision_reason(const DecisionReason reason) noexcept {
  return reason >= DecisionReason::combined && reason <= DecisionReason::numeric_error;
}

[[nodiscard]] bool valid_dominant_reason(const DominantExpertReason reason) noexcept {
  return reason >= DominantExpertReason::highest_weight &&
         reason <= DominantExpertReason::no_eligible_expert;
}

[[nodiscard]] bool valid_coefficient(const std::int64_t value) noexcept {
  return value >= -kMaximumGateCoefficientPpm && value <= kMaximumGateCoefficientPpm;
}

[[nodiscard]] bool valid_linear(const LinearGateParameters& parameters) noexcept {
  return valid_coefficient(parameters.intercept_ppm) &&
         valid_coefficient(parameters.confidence_coefficient_ppm) &&
         valid_coefficient(parameters.calibration_coefficient_ppm) &&
         valid_coefficient(parameters.data_quality_coefficient_ppm) &&
         valid_coefficient(parameters.inverse_ood_coefficient_ppm) &&
         valid_coefficient(parameters.freshness_coefficient_ppm);
}

[[nodiscard]] bool valid_role_policy(const ExpertRolePolicy& policy,
                                     const std::size_t index) noexcept {
  const auto expected = static_cast<ExpertRole>(index + 1U);
  constexpr auto kAllMarketStates =
      static_cast<std::uint16_t>((std::uint32_t{1U} << kMarketStateCount) - 1U);
  constexpr auto kAllEventStates =
      static_cast<std::uint8_t>((std::uint32_t{1U} << kEventStateCount) - 1U);
  return policy.role == expected && policy.allowed_market_state_mask != 0U &&
         (policy.allowed_market_state_mask & ~kAllMarketStates) == 0U &&
         policy.allowed_event_state_mask != 0U &&
         (policy.allowed_event_state_mask & ~kAllEventStates) == 0U &&
         policy.base_multiplier_ppm <= kWeightScale;
}

[[nodiscard]] bool
valid_learned_artifact(const QuantizedLearnedGateArtifact& artifact) noexcept {
  if (!artifact.gate_model_id.valid() || !artifact.gate_model_version.valid() ||
      artifact.signature_hash == 0U || !valid_linear(artifact.coefficients) ||
      artifact.stable_hash == 0U ||
      artifact.stable_hash != stable_learned_gate_hash(artifact)) {
    return false;
  }
  const auto coefficients_valid = [](const std::int64_t bias) {
    return valid_coefficient(bias);
  };
  return std::ranges::all_of(artifact.role_bias_ppm, coefficients_valid) &&
         std::ranges::all_of(artifact.market_state_bias_ppm, coefficients_valid) &&
         std::ranges::all_of(artifact.event_state_bias_ppm, coefficients_valid);
}

void mix_linear(std::uint64_t& hash, const LinearGateParameters& parameters) noexcept {
  mix(hash, std::bit_cast<std::uint64_t>(parameters.intercept_ppm));
  mix(hash, std::bit_cast<std::uint64_t>(parameters.confidence_coefficient_ppm));
  mix(hash, std::bit_cast<std::uint64_t>(parameters.calibration_coefficient_ppm));
  mix(hash, std::bit_cast<std::uint64_t>(parameters.data_quality_coefficient_ppm));
  mix(hash, std::bit_cast<std::uint64_t>(parameters.inverse_ood_coefficient_ppm));
  mix(hash, std::bit_cast<std::uint64_t>(parameters.freshness_coefficient_ppm));
}

void mix_expert_explanation(std::uint64_t& hash,
                            const ExpertExplanation& expert) noexcept {
  mix_identifier(hash, expert.forecast_id);
  mix_identifier(hash, expert.model_id);
  mix_identifier(hash, expert.model_version);
  mix(hash, static_cast<std::uint64_t>(expert.role));
  mix(hash, static_cast<std::uint64_t>(expert.eligibility));
  mix(hash, expert.weight_ppm);
  mix(hash, expert.raw_gate_score);
  mix(hash, expert.freshness_ppm);
  mix(hash, expert.forecast_hash);
}

[[nodiscard]] bool valid_explanation(const DecisionExplanation& explanation,
                                     const bool abstain) noexcept {
  if (!valid_gate_kind(explanation.gate_kind) ||
      !market_state::valid_market_state(explanation.market_state) ||
      !valid_event_state(explanation.event_state) ||
      explanation.input_data_quality_ppm > kWeightScale ||
      explanation.supplied_expert_count > kMaximumExperts ||
      explanation.eligible_expert_count > explanation.supplied_expert_count ||
      explanation.contributing_expert_count > explanation.eligible_expert_count ||
      !valid_dominant_reason(explanation.dominant_reason) ||
      explanation.market_state_snapshot_hash == 0U ||
      explanation.configuration_hash == 0U || explanation.stable_hash == 0U ||
      explanation.stable_hash != stable_explanation_hash(explanation)) {
    return false;
  }
  std::uint64_t weight_sum = 0U;
  std::uint32_t contributing = 0U;
  for (std::size_t index = 0U; index < explanation.supplied_expert_count; ++index) {
    const auto& expert = explanation.experts[index];
    if (!valid_expert_role(expert.role) ||
        !valid_eligibility_reason(expert.eligibility) ||
        expert.weight_ppm > kWeightScale || expert.freshness_ppm > kWeightScale) {
      return false;
    }
    weight_sum += expert.weight_ppm;
    contributing += expert.weight_ppm == 0U ? 0U : 1U;
  }
  const auto expected_sum = contributing == 0U ? 0U : kWeightScale;
  if (weight_sum != expected_sum ||
      contributing != explanation.contributing_expert_count) {
    return false;
  }
  if (contributing == 0U) {
    return abstain &&
           explanation.dominant_reason == DominantExpertReason::no_eligible_expert;
  }
  return explanation.dominant_forecast_id.valid() &&
         explanation.dominant_model_id.valid() &&
         explanation.dominant_model_version.valid() &&
         explanation.dominant_reason != DominantExpertReason::no_eligible_expert;
}

} // namespace

bool valid_gate_kind(const GateKind kind) noexcept {
  return kind == GateKind::rule_based || kind == GateKind::linear ||
         kind == GateKind::quantized_learned;
}

bool valid_expert_role(const ExpertRole role) noexcept {
  return role >= ExpertRole::generic && role <= ExpertRole::auction;
}

bool valid_event_state(const EventState state) noexcept {
  return state >= EventState::none && state <= EventState::volatility_shock;
}

std::uint64_t reason_bit(const DecisionReason reason) noexcept {
  const auto ordinal = static_cast<std::uint8_t>(reason);
  return valid_decision_reason(reason) && ordinal < 64U ? std::uint64_t{1U} << ordinal
                                                        : 0U;
}

std::uint64_t stable_learned_gate_hash(QuantizedLearnedGateArtifact artifact) noexcept {
  artifact.stable_hash = 0U;
  std::uint64_t hash = kFnvOffset;
  mix_identifier(hash, artifact.gate_model_id);
  mix_identifier(hash, artifact.gate_model_version);
  mix(hash, artifact.signature_hash);
  mix_linear(hash, artifact.coefficients);
  for (const auto value : artifact.role_bias_ppm) {
    mix(hash, std::bit_cast<std::uint64_t>(value));
  }
  for (const auto value : artifact.market_state_bias_ppm) {
    mix(hash, std::bit_cast<std::uint64_t>(value));
  }
  for (const auto value : artifact.event_state_bias_ppm) {
    mix(hash, std::bit_cast<std::uint64_t>(value));
  }
  return nonzero_hash(hash);
}

std::uint64_t stable_ensemble_config_hash(EnsembleConfig config) noexcept {
  config.stable_hash = 0U;
  std::uint64_t hash = kFnvOffset;
  mix_identifier(hash, config.ensemble_model_id);
  mix_identifier(hash, config.ensemble_model_version);
  mix_identifier(hash, config.configuration_version);
  mix(hash, static_cast<std::uint64_t>(config.gate_kind));
  mix(hash, config.maximum_expert_weight_ppm);
  mix(hash, config.ood_reject_threshold_ppm);
  mix(hash, config.minimum_calibration_score_ppm);
  mix(hash, config.minimum_data_quality_score_ppm);
  mix(hash, config.maximum_forecast_age_ns);
  mix(hash, config.maximum_market_state_age_ns);
  mix(hash, config.degraded_feed_multiplier_ppm);
  mix(hash, config.degraded_calibration_multiplier_ppm);
  mix(hash, config.timeseries_breaking_news_multiplier_ppm);
  mix(hash, config.uncertainty_penalty_multiplier_ppm);
  mix(hash, config.calibration_uncertainty_multiplier_ppm);
  mix(hash, config.ood_uncertainty_multiplier_ppm);
  mix(hash, config.data_quality_uncertainty_multiplier_ppm);
  mix(hash, config.safety_margin_ppm);
  for (const auto& policy : config.role_policies) {
    mix(hash, static_cast<std::uint64_t>(policy.role));
    mix(hash, policy.allowed_market_state_mask);
    mix(hash, policy.allowed_event_state_mask);
    mix(hash, policy.base_multiplier_ppm);
  }
  mix_linear(hash, config.linear);
  mix(hash, config.learned.stable_hash);
  return nonzero_hash(hash);
}

std::uint64_t stable_explanation_hash(DecisionExplanation explanation) noexcept {
  explanation.stable_hash = 0U;
  std::uint64_t hash = kFnvOffset;
  mix(hash, static_cast<std::uint64_t>(explanation.gate_kind));
  mix(hash, static_cast<std::uint64_t>(explanation.market_state));
  mix(hash, static_cast<std::uint64_t>(explanation.event_state));
  mix(hash, explanation.input_data_quality_ppm);
  mix(hash, explanation.supplied_expert_count);
  mix(hash, explanation.eligible_expert_count);
  mix(hash, explanation.contributing_expert_count);
  mix_identifier(hash, explanation.dominant_forecast_id);
  mix_identifier(hash, explanation.dominant_model_id);
  mix_identifier(hash, explanation.dominant_model_version);
  mix(hash, static_cast<std::uint64_t>(explanation.dominant_reason));
  mix(hash, explanation.transaction_cost_forecast_hash);
  mix(hash, explanation.market_state_snapshot_hash);
  mix(hash, explanation.configuration_hash);
  mix(hash, explanation.learned_gate_signature_hash);
  mix(hash, explanation.required_edge_ppm);
  const auto count = explanation.supplied_expert_count > kMaximumExperts
                         ? kMaximumExperts
                         : explanation.supplied_expert_count;
  for (std::size_t index = 0U; index < count; ++index) {
    mix_expert_explanation(hash, explanation.experts[index]);
  }
  return nonzero_hash(hash);
}

std::uint64_t stable_ensemble_forecast_hash(EnsembleForecast forecast) noexcept {
  forecast.stable_hash = 0U;
  std::uint64_t hash = kFnvOffset;
  mix_identifier(hash, forecast.forecast_id);
  mix_identifier(hash, forecast.session_id);
  mix_identifier(hash, forecast.instrument_id);
  mix_identifier(hash, forecast.ensemble_model_id);
  mix_identifier(hash, forecast.ensemble_model_version);
  mix_identifier(hash, forecast.configuration_version);
  mix(hash, forecast.horizon_ns);
  mix(hash, forecast.created_process_monotonic_time_ns);
  mix(hash, forecast.valid_until_process_monotonic_time_ns);
  mix(hash, std::bit_cast<std::uint64_t>(forecast.expected_return_ppm));
  mix(hash, forecast.probability_down_ppm);
  mix(hash, forecast.probability_flat_ppm);
  mix(hash, forecast.probability_up_ppm);
  mix(hash, forecast.variance_ppm_squared);
  mix(hash, std::bit_cast<std::uint64_t>(forecast.return_p10_ppm));
  mix(hash, std::bit_cast<std::uint64_t>(forecast.return_p50_ppm));
  mix(hash, std::bit_cast<std::uint64_t>(forecast.return_p90_ppm));
  mix(hash, forecast.effective_uncertainty_ppm);
  mix(hash, forecast.disagreement_ppm);
  mix(hash, forecast.estimated_transaction_cost_ppm);
  mix(hash, forecast.uncertainty_penalty_ppm);
  mix(hash, forecast.safety_margin_ppm);
  mix(hash, std::bit_cast<std::uint64_t>(forecast.net_robust_edge_ppm));
  mix(hash, forecast.abstain ? 1U : 0U);
  mix(hash, static_cast<std::uint64_t>(forecast.primary_reason));
  mix(hash, forecast.reason_mask);
  mix(hash, forecast.explanation.stable_hash);
  return nonzero_hash(hash);
}

bool valid_ensemble_config(const EnsembleConfig& config) noexcept {
  if (!config.ensemble_model_id.valid() || !config.ensemble_model_version.valid() ||
      !config.configuration_version.valid() || !valid_gate_kind(config.gate_kind) ||
      config.maximum_expert_weight_ppm == 0U ||
      config.maximum_expert_weight_ppm > kWeightScale ||
      config.ood_reject_threshold_ppm > kWeightScale ||
      config.minimum_calibration_score_ppm > kWeightScale ||
      config.minimum_data_quality_score_ppm > kWeightScale ||
      config.maximum_forecast_age_ns == 0U ||
      config.maximum_forecast_age_ns > kMaximumScaledDurationNs ||
      config.maximum_market_state_age_ns == 0U ||
      config.maximum_market_state_age_ns > kMaximumScaledDurationNs ||
      config.degraded_feed_multiplier_ppm > kWeightScale ||
      config.degraded_calibration_multiplier_ppm > kWeightScale ||
      config.timeseries_breaking_news_multiplier_ppm > kWeightScale ||
      config.uncertainty_penalty_multiplier_ppm > kWeightScale ||
      config.calibration_uncertainty_multiplier_ppm > kWeightScale ||
      config.ood_uncertainty_multiplier_ppm > kWeightScale ||
      config.data_quality_uncertainty_multiplier_ppm > kWeightScale ||
      config.safety_margin_ppm > kMaximumAggregateCostPpm ||
      !valid_linear(config.linear) || config.stable_hash == 0U ||
      config.stable_hash != stable_ensemble_config_hash(config)) {
    return false;
  }
  for (std::size_t index = 0U; index < config.role_policies.size(); ++index) {
    if (!valid_role_policy(config.role_policies[index], index)) {
      return false;
    }
  }
  return config.gate_kind != GateKind::quantized_learned ||
         valid_learned_artifact(config.learned);
}

bool valid_ensemble_forecast(const EnsembleForecast& forecast) noexcept {
  const auto probability_sum =
      static_cast<std::uint64_t>(forecast.probability_down_ppm) +
      forecast.probability_flat_ppm + forecast.probability_up_ppm;
  if (!forecast.forecast_id.valid() || !forecast.session_id.valid() ||
      !forecast.instrument_id.valid() || !forecast.ensemble_model_id.valid() ||
      !forecast.ensemble_model_version.valid() ||
      !forecast.configuration_version.valid() || forecast.horizon_ns == 0U ||
      forecast.created_process_monotonic_time_ns == 0U ||
      forecast.valid_until_process_monotonic_time_ns <=
          forecast.created_process_monotonic_time_ns ||
      forecast.expected_return_ppm < -models::kMaximumAbsoluteReturnPpm ||
      forecast.expected_return_ppm > models::kMaximumAbsoluteReturnPpm ||
      probability_sum != kWeightScale || forecast.probability_down_ppm > kWeightScale ||
      forecast.probability_flat_ppm > kWeightScale ||
      forecast.probability_up_ppm > kWeightScale ||
      forecast.return_p10_ppm > forecast.return_p50_ppm ||
      forecast.return_p50_ppm > forecast.return_p90_ppm ||
      forecast.return_p10_ppm < -models::kMaximumAbsoluteReturnPpm ||
      forecast.return_p90_ppm > models::kMaximumAbsoluteReturnPpm ||
      forecast.effective_uncertainty_ppm > kMaximumEffectiveUncertaintyPpm ||
      forecast.disagreement_ppm > kMaximumEffectiveUncertaintyPpm ||
      forecast.estimated_transaction_cost_ppm > kMaximumAggregateCostPpm ||
      forecast.uncertainty_penalty_ppm > kMaximumAggregateCostPpm ||
      forecast.safety_margin_ppm > kMaximumAggregateCostPpm ||
      forecast.net_robust_edge_ppm < -kMaximumNetRobustEdgePpm ||
      forecast.net_robust_edge_ppm > kMaximumNetRobustEdgePpm ||
      !valid_decision_reason(forecast.primary_reason) ||
      (forecast.reason_mask & reason_bit(forecast.primary_reason)) == 0U ||
      !valid_explanation(forecast.explanation, forecast.abstain) ||
      forecast.stable_hash == 0U ||
      forecast.stable_hash != stable_ensemble_forecast_hash(forecast)) {
    return false;
  }
  const auto required_edge = forecast.estimated_transaction_cost_ppm +
                             forecast.uncertainty_penalty_ppm +
                             forecast.safety_margin_ppm;
  const auto absolute_return = static_cast<std::uint64_t>(
      forecast.expected_return_ppm < 0 ? -forecast.expected_return_ppm
                                       : forecast.expected_return_ppm);
  std::int64_t expected_net_robust_edge{};
  if (forecast.expected_return_ppm > 0) {
    expected_net_robust_edge =
        forecast.expected_return_ppm - static_cast<std::int64_t>(required_edge);
  } else if (forecast.expected_return_ppm < 0) {
    expected_net_robust_edge =
        forecast.expected_return_ppm + static_cast<std::int64_t>(required_edge);
  }
  return forecast.explanation.required_edge_ppm == required_edge &&
         forecast.net_robust_edge_ppm == expected_net_robust_edge &&
         forecast.abstain == (absolute_return <= required_edge) &&
         ((!forecast.abstain && forecast.primary_reason == DecisionReason::combined &&
           forecast.explanation.contributing_expert_count != 0U) ||
          (forecast.abstain && forecast.primary_reason != DecisionReason::combined));
}

} // namespace aegis::ensemble
