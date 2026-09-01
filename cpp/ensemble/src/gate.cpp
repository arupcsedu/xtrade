#include "aegis/ensemble/gate.hpp"

#include "aegis/models/validator.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace aegis::ensemble {
namespace {

constexpr std::uint64_t kMaximumRawGateScore = 100'000'000U;

struct WorkEntry {
  std::size_t request_index{};
  ExpertExplanation explanation;
};

[[nodiscard]] std::uint64_t scaled_multiply(const std::uint64_t left,
                                            const std::uint64_t right) noexcept {
  return ((left / kWeightScale) * right) +
         (((left % kWeightScale) * right) / kWeightScale);
}

[[nodiscard]] std::uint32_t scaled_ratio(const std::uint64_t numerator,
                                         const std::uint64_t denominator) noexcept {
  if (denominator == 0U || numerator >= denominator) {
    return numerator == denominator && denominator != 0U ? kWeightScale : 0U;
  }
  return static_cast<std::uint32_t>((numerator * kWeightScale) / denominator);
}

[[nodiscard]] std::uint32_t
freshness_ppm(const models::ModelForecast& forecast, const EnsembleConfig& config,
              const std::uint64_t now_process_monotonic_time_ns) noexcept {
  if (forecast.production_process_monotonic_time_ns == 0U ||
      forecast.production_process_monotonic_time_ns > now_process_monotonic_time_ns) {
    return 0U;
  }
  const auto age =
      now_process_monotonic_time_ns - forecast.production_process_monotonic_time_ns;
  if (age >= config.maximum_forecast_age_ns) {
    return 0U;
  }
  return kWeightScale - scaled_ratio(age, config.maximum_forecast_age_ns);
}

[[nodiscard]] bool unsafe_market_state(const market_state::MarketState state) noexcept {
  return state == market_state::MarketState::startup ||
         state == market_state::MarketState::data_degraded ||
         state == market_state::MarketState::halted ||
         state == market_state::MarketState::reopening ||
         state == market_state::MarketState::recovery ||
         state == market_state::MarketState::shutdown;
}

[[nodiscard]] bool invalid_feed_health(const market_state::FeedHealth health) noexcept {
  return health != market_state::FeedHealth::healthy &&
         health != market_state::FeedHealth::degraded;
}

[[nodiscard]] bool valid_request_shape(const EnsembleRequest& request,
                                       const EnsembleConfig& config) noexcept {
  if (!request.forecast_id.valid() || !request.session_id.valid() ||
      !request.instrument_id.valid() || !request.configuration_version.valid() ||
      request.configuration_version != config.configuration_version ||
      request.horizon_ns == 0U || request.now_process_monotonic_time_ns == 0U ||
      request.valid_until_process_monotonic_time_ns <=
          request.now_process_monotonic_time_ns ||
      request.expert_count > kMaximumExperts ||
      request.input_data_quality_ppm > kWeightScale ||
      !valid_event_state(request.event_state) ||
      !request.market_state_snapshot.valid() ||
      request.market_state_snapshot.observed_process_monotonic_time_ns >
          request.now_process_monotonic_time_ns) {
    return false;
  }
  return request.now_process_monotonic_time_ns -
             request.market_state_snapshot.observed_process_monotonic_time_ns <=
         config.maximum_market_state_age_ns;
}

[[nodiscard]] bool health_usable(const models::ModelHealthSnapshot& health) noexcept {
  return health.state == models::ModelHealthState::healthy ||
         health.state == models::ModelHealthState::degraded;
}

[[nodiscard]] EligibilityReason validate_expert(const ExpertInput& input,
                                                const EnsembleRequest& request,
                                                const EnsembleConfig& config) noexcept {
  const auto& forecast = input.forecast;
  if (!valid_expert_role(input.role) || !forecast.forecast_id.valid() ||
      !forecast.model_id.valid() || !forecast.model_version.valid() ||
      !forecast.feature_snapshot_id.valid() || forecast.stable_hash == 0U) {
    return EligibilityReason::missing_provenance;
  }
  const auto validation = models::ModelForecastValidator::validate_forecast(
      forecast, request.now_process_monotonic_time_ns);
  if (validation == models::ForecastValidationError::expired) {
    return EligibilityReason::expired;
  }
  if (validation == models::ForecastValidationError::invalid_identity ||
      validation == models::ForecastValidationError::invalid_feature_provenance) {
    return EligibilityReason::missing_provenance;
  }
  if (validation != models::ForecastValidationError::none ||
      forecast.production_process_monotonic_time_ns >
          request.now_process_monotonic_time_ns) {
    return EligibilityReason::invalid_forecast;
  }
  if (!health_usable(input.health)) {
    return EligibilityReason::model_disabled;
  }
  if (input.health.generation != forecast.model_control_generation) {
    return EligibilityReason::control_generation_mismatch;
  }
  if (forecast.session_id != request.session_id ||
      forecast.instrument_id != request.instrument_id ||
      forecast.configuration_version != request.configuration_version ||
      forecast.horizon_ns != request.horizon_ns) {
    return EligibilityReason::scope_mismatch;
  }
  const auto age = request.now_process_monotonic_time_ns -
                   forecast.production_process_monotonic_time_ns;
  if (age > config.maximum_forecast_age_ns) {
    return EligibilityReason::expired;
  }
  if (forecast.prediction.ood_score_ppm > config.ood_reject_threshold_ppm) {
    return EligibilityReason::excessive_ood;
  }
  if (input.calibration_health == CalibrationHealth::failed ||
      input.calibration_health == CalibrationHealth::unknown ||
      forecast.prediction.calibration_score_ppm <
          config.minimum_calibration_score_ppm) {
    return EligibilityReason::calibration_failed;
  }
  if (forecast.prediction.data_quality_score_ppm <
          config.minimum_data_quality_score_ppm ||
      request.input_data_quality_ppm < config.minimum_data_quality_score_ppm) {
    return EligibilityReason::data_quality_failed;
  }
  const auto role_index = static_cast<std::size_t>(input.role) - 1U;
  const auto market_index =
      static_cast<std::size_t>(request.market_state_snapshot.state);
  const auto event_index = static_cast<std::size_t>(request.event_state) - 1U;
  const auto& policy = config.role_policies[role_index];
  const auto market_bit = std::uint16_t{1U} << market_index;
  const auto event_bit = std::uint8_t{1U} << event_index;
  if ((policy.allowed_market_state_mask & market_bit) == 0U ||
      (policy.allowed_event_state_mask & event_bit) == 0U) {
    return EligibilityReason::incompatible_state;
  }
  return EligibilityReason::eligible;
}

[[nodiscard]] bool canonical_less(const WorkEntry& left,
                                  const WorkEntry& right) noexcept {
  if (left.explanation.model_id != right.explanation.model_id) {
    return left.explanation.model_id < right.explanation.model_id;
  }
  if (left.explanation.model_version != right.explanation.model_version) {
    return left.explanation.model_version < right.explanation.model_version;
  }
  return left.explanation.forecast_id < right.explanation.forecast_id;
}

void canonicalize(std::array<WorkEntry, kMaximumExperts>& work,
                  const std::size_t count) noexcept {
  for (std::size_t index = 1U; index < count; ++index) {
    auto value = work[index];
    auto position = index;
    while (position > 0U && canonical_less(value, work[position - 1U])) {
      work[position] = work[position - 1U];
      --position;
    }
    work[position] = value;
  }
}

void exclude_duplicates(std::array<WorkEntry, kMaximumExperts>& work,
                        const std::size_t count) noexcept {
  for (std::size_t left = 0U; left < count; ++left) {
    for (std::size_t right = left + 1U; right < count; ++right) {
      if (work[left].explanation.forecast_id == work[right].explanation.forecast_id ||
          work[left].explanation.model_id == work[right].explanation.model_id) {
        work[left].explanation.eligibility = EligibilityReason::duplicate_expert;
        work[right].explanation.eligibility = EligibilityReason::duplicate_expert;
      }
    }
  }
}

[[nodiscard]] std::int64_t linear_score(const LinearGateParameters& parameters,
                                        const models::ModelPrediction& prediction,
                                        const std::uint32_t freshness) noexcept {
  auto score = parameters.intercept_ppm;
  const auto add = [&score](const std::int64_t coefficient,
                            const std::uint32_t feature) {
    score += (coefficient * static_cast<std::int64_t>(feature)) /
             static_cast<std::int64_t>(kWeightScale);
  };
  add(parameters.confidence_coefficient_ppm, prediction.confidence_ppm);
  add(parameters.calibration_coefficient_ppm, prediction.calibration_score_ppm);
  add(parameters.data_quality_coefficient_ppm, prediction.data_quality_score_ppm);
  add(parameters.inverse_ood_coefficient_ppm, kWeightScale - prediction.ood_score_ppm);
  add(parameters.freshness_coefficient_ppm, freshness);
  return score;
}

[[nodiscard]] std::uint64_t positive_score(const std::int64_t score) noexcept {
  if (score <= 0) {
    return 0U;
  }
  return std::min<std::uint64_t>(static_cast<std::uint64_t>(score),
                                 kMaximumRawGateScore);
}

[[nodiscard]] std::uint64_t base_gate_score(const ExpertInput& input,
                                            const EnsembleRequest& request,
                                            const EnsembleConfig& config,
                                            const std::uint32_t freshness) noexcept {
  const auto& prediction = input.forecast.prediction;
  std::uint64_t score{};
  if (config.gate_kind == GateKind::rule_based) {
    score = (static_cast<std::uint64_t>(prediction.confidence_ppm) +
             prediction.calibration_score_ppm + prediction.data_quality_score_ppm +
             (kWeightScale - prediction.ood_score_ppm) + freshness) /
            5U;
  } else {
    const auto& parameters = config.gate_kind == GateKind::linear
                                 ? config.linear
                                 : config.learned.coefficients;
    auto value = linear_score(parameters, prediction, freshness);
    if (config.gate_kind == GateKind::quantized_learned) {
      value += config.learned.role_bias_ppm[static_cast<std::size_t>(input.role) - 1U];
      value += config.learned.market_state_bias_ppm[static_cast<std::size_t>(
          request.market_state_snapshot.state)];
      value +=
          config.learned
              .event_state_bias_ppm[static_cast<std::size_t>(request.event_state) - 1U];
    }
    score = positive_score(value);
  }
  return score;
}

[[nodiscard]] std::uint64_t
adjusted_gate_score(const ExpertInput& input, const EnsembleRequest& request,
                    const EnsembleConfig& config,
                    const std::uint32_t freshness) noexcept {
  auto score = base_gate_score(input, request, config, freshness);
  const auto& policy = config.role_policies[static_cast<std::size_t>(input.role) - 1U];
  score = scaled_multiply(score, policy.base_multiplier_ppm);
  if (request.feed_health == market_state::FeedHealth::degraded) {
    score = scaled_multiply(score, config.degraded_feed_multiplier_ppm);
  }
  if (input.calibration_health == CalibrationHealth::degraded) {
    score = scaled_multiply(score, config.degraded_calibration_multiplier_ppm);
  }
  if (input.role == ExpertRole::timeseries &&
      request.event_state == EventState::breaking_news) {
    score = scaled_multiply(score, config.timeseries_breaking_news_multiplier_ppm);
  }
  return std::min(score, kMaximumRawGateScore);
}

[[nodiscard]] std::uint32_t
positive_score_count(const std::array<WorkEntry, kMaximumExperts>& work,
                     const std::size_t count) noexcept {
  std::uint32_t result{};
  for (std::size_t index = 0U; index < count; ++index) {
    result += work[index].explanation.eligibility == EligibilityReason::eligible &&
                      work[index].explanation.raw_gate_score != 0U
                  ? 1U
                  : 0U;
  }
  return result;
}

struct WeightLimits {
  std::size_t count{};
  std::uint32_t cap{};
};

void initialize_active_weights(const std::array<WorkEntry, kMaximumExperts>& work,
                               const std::size_t count,
                               std::array<bool, kMaximumExperts>& active) noexcept {
  for (std::size_t index = 0U; index < count; ++index) {
    active[index] =
        work[index].explanation.eligibility == EligibilityReason::eligible &&
        work[index].explanation.raw_gate_score != 0U;
  }
}

[[nodiscard]] std::uint64_t
active_score_sum(const std::array<WorkEntry, kMaximumExperts>& work,
                 const std::array<bool, kMaximumExperts>& active,
                 const std::size_t count) noexcept {
  std::uint64_t result{};
  for (std::size_t index = 0U; index < count; ++index) {
    if (active[index]) {
      result += work[index].explanation.raw_gate_score;
    }
  }
  return result;
}

[[nodiscard]] bool cap_overweight_entries(std::array<WorkEntry, kMaximumExperts>& work,
                                          std::array<bool, kMaximumExperts>& active,
                                          const WeightLimits limits,
                                          const std::uint64_t score_sum,
                                          std::uint32_t& remaining) noexcept {
  bool capped_any = false;
  for (std::size_t index = 0U; index < limits.count; ++index) {
    if (!active[index]) {
      continue;
    }
    const auto proposed =
        static_cast<std::uint32_t>((static_cast<std::uint64_t>(remaining) *
                                    work[index].explanation.raw_gate_score) /
                                   score_sum);
    if (proposed > limits.cap) {
      work[index].explanation.weight_ppm = limits.cap;
      remaining -= limits.cap;
      active[index] = false;
      capped_any = true;
    }
  }
  return capped_any;
}

void assign_active_weights(std::array<WorkEntry, kMaximumExperts>& work,
                           const std::array<bool, kMaximumExperts>& active,
                           const WeightLimits limits, const std::uint32_t remaining,
                           const std::uint64_t score_sum) noexcept {
  for (std::size_t index = 0U; index < limits.count; ++index) {
    if (active[index]) {
      work[index].explanation.weight_ppm =
          static_cast<std::uint32_t>((static_cast<std::uint64_t>(remaining) *
                                      work[index].explanation.raw_gate_score) /
                                     score_sum);
    }
  }
}

[[nodiscard]] std::uint32_t
assigned_weight(const std::array<WorkEntry, kMaximumExperts>& work,
                const std::size_t count) noexcept {
  std::uint32_t result{};
  for (std::size_t index = 0U; index < count; ++index) {
    result += work[index].explanation.weight_ppm;
  }
  return result;
}

[[nodiscard]] bool
distribute_weight_residue(std::array<WorkEntry, kMaximumExperts>& work,
                          const WeightLimits limits, std::uint32_t residue) noexcept {
  while (residue != 0U) {
    bool progressed = false;
    for (std::size_t index = 0U; index < limits.count; ++index) {
      if (residue == 0U) {
        break;
      }
      if (work[index].explanation.raw_gate_score != 0U &&
          work[index].explanation.weight_ppm < limits.cap) {
        ++work[index].explanation.weight_ppm;
        --residue;
        progressed = true;
      }
    }
    if (!progressed) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool normalize_weights(std::array<WorkEntry, kMaximumExperts>& work,
                                     const WeightLimits limits) noexcept {
  const auto positive_count = positive_score_count(work, limits.count);
  if (positive_count == 0U ||
      static_cast<std::uint64_t>(positive_count) * limits.cap < kWeightScale) {
    return false;
  }

  std::array<bool, kMaximumExperts> active{};
  initialize_active_weights(work, limits.count, active);
  std::uint32_t remaining = kWeightScale;
  for (std::size_t pass = 0U; pass < limits.count; ++pass) {
    const auto score_sum = active_score_sum(work, active, limits.count);
    if (score_sum == 0U) {
      return false;
    }
    if (!cap_overweight_entries(work, active, limits, score_sum, remaining)) {
      assign_active_weights(work, active, limits, remaining, score_sum);
      break;
    }
  }
  const auto residue = kWeightScale - assigned_weight(work, limits.count);
  return distribute_weight_residue(work, limits, residue);
}

[[nodiscard]] std::uint64_t integer_sqrt(const std::uint64_t value) noexcept {
  if (value < 2U) {
    return value;
  }
  const auto width = static_cast<unsigned>(std::bit_width(value));
  auto estimate = std::uint64_t{1U} << ((width + 1U) / 2U);
  while (true) {
    const auto next = (estimate + value / estimate) / 2U;
    if (next >= estimate) {
      return estimate;
    }
    estimate = next;
  }
}

[[nodiscard]] std::int64_t
weighted_signed_value(const std::array<WorkEntry, kMaximumExperts>& work,
                      const std::size_t count, const EnsembleRequest& request,
                      const std::int64_t models::ModelPrediction::* field) noexcept {
  std::int64_t total{};
  for (std::size_t index = 0U; index < count; ++index) {
    const auto weight = work[index].explanation.weight_ppm;
    const auto value =
        request.experts[work[index].request_index].forecast.prediction.*field;
    total += (value * static_cast<std::int64_t>(weight)) /
             static_cast<std::int64_t>(kWeightScale);
  }
  return total;
}

[[nodiscard]] std::uint32_t
weighted_probability(const std::array<WorkEntry, kMaximumExperts>& work,
                     const std::size_t count, const EnsembleRequest& request,
                     const std::uint32_t models::ModelPrediction::* field) noexcept {
  std::uint64_t total{};
  for (std::size_t index = 0U; index < count; ++index) {
    const auto weight = work[index].explanation.weight_ppm;
    const auto value =
        request.experts[work[index].request_index].forecast.prediction.*field;
    total += scaled_multiply(value, weight);
  }
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(total, kWeightScale));
}

[[nodiscard]] std::uint64_t weighted_score_shortfall(
    const std::array<WorkEntry, kMaximumExperts>& work, const std::size_t count,
    const EnsembleRequest& request,
    const std::uint32_t models::ModelPrediction::* field) noexcept {
  std::uint64_t total{};
  for (std::size_t index = 0U; index < count; ++index) {
    const auto weight = work[index].explanation.weight_ppm;
    const auto value =
        request.experts[work[index].request_index].forecast.prediction.*field;
    total += scaled_multiply(kWeightScale - value, weight);
  }
  return total;
}

[[nodiscard]] std::uint64_t
weighted_ood(const std::array<WorkEntry, kMaximumExperts>& work,
             const std::size_t count, const EnsembleRequest& request) noexcept {
  std::uint64_t total{};
  for (std::size_t index = 0U; index < count; ++index) {
    const auto weight = work[index].explanation.weight_ppm;
    const auto value =
        request.experts[work[index].request_index].forecast.prediction.ood_score_ppm;
    total += scaled_multiply(value, weight);
  }
  return total;
}

[[nodiscard]] std::uint64_t
mixture_variance(const std::array<WorkEntry, kMaximumExperts>& work,
                 const std::size_t count, const EnsembleRequest& request,
                 const std::int64_t mean) noexcept {
  std::uint64_t result{};
  for (std::size_t index = 0U; index < count; ++index) {
    const auto weight = work[index].explanation.weight_ppm;
    if (weight == 0U) {
      continue;
    }
    const auto& prediction =
        request.experts[work[index].request_index].forecast.prediction;
    const auto difference = prediction.expected_return_ppm - mean;
    const auto absolute_difference =
        static_cast<std::uint64_t>(difference < 0 ? -difference : difference);
    const auto within = prediction.volatility_ppm * prediction.volatility_ppm;
    const auto between = absolute_difference * absolute_difference;
    result += scaled_multiply(within, weight) + scaled_multiply(between, weight);
  }
  return result;
}

[[nodiscard]] bool transaction_cost(const EnsembleRequest& request,
                                    std::uint64_t& aggregate_cost) noexcept {
  if (!request.transaction_cost_present ||
      !health_usable(request.transaction_cost_health)) {
    return false;
  }
  const auto& forecast = request.transaction_cost_forecast;
  if (models::ModelForecastValidator::validate_forecast(
          forecast, request.now_process_monotonic_time_ns) !=
          models::ForecastValidationError::none ||
      forecast.session_id != request.session_id ||
      forecast.instrument_id != request.instrument_id ||
      forecast.configuration_version != request.configuration_version ||
      forecast.horizon_ns != request.horizon_ns ||
      forecast.model_control_generation != request.transaction_cost_health.generation ||
      !forecast.prediction.transaction_cost.present) {
    return false;
  }
  const auto& cost = forecast.prediction.transaction_cost;
  aggregate_cost = cost.spread_cost_ppm + cost.slippage_cost_ppm +
                   cost.market_impact_ppm + cost.adverse_selection_cost_ppm +
                   cost.fee_cost_ppm;
  return aggregate_cost <= kMaximumAggregateCostPpm;
}

void set_decision_reason(EnsembleForecast& forecast,
                         const DecisionReason reason) noexcept {
  forecast.primary_reason = reason;
  forecast.reason_mask |= reason_bit(reason);
}

[[nodiscard]] EnsembleForecast base_forecast(const EnsembleRequest& request,
                                             const EnsembleConfig& config) noexcept {
  EnsembleForecast forecast{};
  forecast.forecast_id = request.forecast_id;
  forecast.session_id = request.session_id;
  forecast.instrument_id = request.instrument_id;
  forecast.ensemble_model_id = config.ensemble_model_id;
  forecast.ensemble_model_version = config.ensemble_model_version;
  forecast.configuration_version = config.configuration_version;
  forecast.horizon_ns = request.horizon_ns;
  forecast.created_process_monotonic_time_ns = request.now_process_monotonic_time_ns;
  forecast.valid_until_process_monotonic_time_ns =
      request.valid_until_process_monotonic_time_ns;
  forecast.safety_margin_ppm = config.safety_margin_ppm;
  forecast.explanation.gate_kind = config.gate_kind;
  forecast.explanation.market_state = request.market_state_snapshot.state;
  forecast.explanation.event_state = request.event_state;
  forecast.explanation.input_data_quality_ppm = request.input_data_quality_ppm;
  forecast.explanation.supplied_expert_count = request.expert_count;
  forecast.explanation.market_state_snapshot_hash =
      request.market_state_snapshot.stable_hash;
  forecast.explanation.configuration_hash = config.stable_hash;
  forecast.explanation.learned_gate_signature_hash =
      config.gate_kind == GateKind::quantized_learned ? config.learned.signature_hash
                                                      : 0U;
  forecast.explanation.required_edge_ppm = config.safety_margin_ppm;
  return forecast;
}

void finalize_hashes(EnsembleForecast& forecast) noexcept {
  forecast.explanation.stable_hash = stable_explanation_hash(forecast.explanation);
  forecast.stable_hash = stable_ensemble_forecast_hash(forecast);
}

[[nodiscard]] EvaluationResult early_abstention(EnsembleForecast forecast,
                                                const EvaluationStatus status,
                                                const DecisionReason reason) noexcept {
  forecast.abstain = true;
  forecast.explanation.dominant_reason = DominantExpertReason::no_eligible_expert;
  set_decision_reason(forecast, reason);
  finalize_hashes(forecast);
  return {.status = status, .forecast = forecast};
}

void fill_work_entries(std::array<WorkEntry, kMaximumExperts>& work,
                       const EnsembleRequest& request, const EnsembleConfig& config,
                       const EligibilityReason global_exclusion) noexcept {
  for (std::size_t index = 0U; index < request.expert_count; ++index) {
    const auto& input = request.experts[index];
    auto& entry = work[index];
    entry.request_index = index;
    entry.explanation.forecast_id = input.forecast.forecast_id;
    entry.explanation.model_id = input.forecast.model_id;
    entry.explanation.model_version = input.forecast.model_version;
    entry.explanation.role = input.role;
    entry.explanation.forecast_hash = input.forecast.stable_hash;
    entry.explanation.freshness_ppm =
        freshness_ppm(input.forecast, config, request.now_process_monotonic_time_ns);
    entry.explanation.eligibility = global_exclusion == EligibilityReason::eligible
                                        ? validate_expert(input, request, config)
                                        : global_exclusion;
  }
  canonicalize(work, request.expert_count);
  exclude_duplicates(work, request.expert_count);
}

[[nodiscard]] std::uint32_t
score_eligible_experts(std::array<WorkEntry, kMaximumExperts>& work,
                       const EnsembleRequest& request,
                       const EnsembleConfig& config) noexcept {
  std::uint32_t eligible{};
  for (std::size_t index = 0U; index < request.expert_count; ++index) {
    auto& entry = work[index];
    if (entry.explanation.eligibility != EligibilityReason::eligible) {
      continue;
    }
    ++eligible;
    const auto& input = request.experts[entry.request_index];
    entry.explanation.raw_gate_score =
        adjusted_gate_score(input, request, config, entry.explanation.freshness_ppm);
  }
  return eligible;
}

void publish_explanations(EnsembleForecast& forecast,
                          const std::array<WorkEntry, kMaximumExperts>& work,
                          const std::size_t count) noexcept {
  for (std::size_t index = 0U; index < count; ++index) {
    forecast.explanation.experts[index] = work[index].explanation;
    forecast.explanation.contributing_expert_count +=
        work[index].explanation.weight_ppm == 0U ? 0U : 1U;
  }
}

void set_dominant_expert(EnsembleForecast& forecast,
                         const std::array<WorkEntry, kMaximumExperts>& work,
                         const std::size_t count) noexcept {
  std::size_t dominant{};
  std::uint32_t maximum{};
  std::uint32_t maximum_count{};
  for (std::size_t index = 0U; index < count; ++index) {
    const auto weight = work[index].explanation.weight_ppm;
    if (weight > maximum) {
      maximum = weight;
      dominant = index;
      maximum_count = 1U;
    } else if (weight != 0U && weight == maximum) {
      ++maximum_count;
    }
  }
  const auto& expert = work[dominant].explanation;
  forecast.explanation.dominant_forecast_id = expert.forecast_id;
  forecast.explanation.dominant_model_id = expert.model_id;
  forecast.explanation.dominant_model_version = expert.model_version;
  if (forecast.explanation.contributing_expert_count == 1U) {
    forecast.explanation.dominant_reason = DominantExpertReason::only_eligible_expert;
  } else if (maximum_count > 1U) {
    forecast.explanation.dominant_reason = DominantExpertReason::canonical_tie_break;
  } else {
    forecast.explanation.dominant_reason = DominantExpertReason::highest_weight;
  }
}

void combine_distribution(EnsembleForecast& forecast,
                          const std::array<WorkEntry, kMaximumExperts>& work,
                          const EnsembleRequest& request) noexcept {
  forecast.expected_return_ppm =
      weighted_signed_value(work, request.expert_count, request,
                            &models::ModelPrediction::expected_return_ppm);
  forecast.return_p10_ppm = weighted_signed_value(
      work, request.expert_count, request, &models::ModelPrediction::return_p10_ppm);
  forecast.return_p50_ppm = weighted_signed_value(
      work, request.expert_count, request, &models::ModelPrediction::return_p50_ppm);
  forecast.return_p90_ppm = weighted_signed_value(
      work, request.expert_count, request, &models::ModelPrediction::return_p90_ppm);
  forecast.probability_down_ppm =
      weighted_probability(work, request.expert_count, request,
                           &models::ModelPrediction::probability_down_ppm);
  forecast.probability_up_ppm =
      weighted_probability(work, request.expert_count, request,
                           &models::ModelPrediction::probability_up_ppm);
  const auto directional = forecast.probability_down_ppm + forecast.probability_up_ppm;
  forecast.probability_flat_ppm =
      directional <= kWeightScale ? kWeightScale - directional : 0U;
  forecast.variance_ppm_squared = mixture_variance(work, request.expert_count, request,
                                                   forecast.expected_return_ppm);
  forecast.disagreement_ppm = integer_sqrt(
      mixture_variance(work, request.expert_count, request,
                       forecast.expected_return_ppm) -
      [&]() noexcept {
        std::uint64_t within{};
        for (std::size_t index = 0U; index < request.expert_count; ++index) {
          const auto weight = work[index].explanation.weight_ppm;
          const auto volatility = request.experts[work[index].request_index]
                                      .forecast.prediction.volatility_ppm;
          within += scaled_multiply(volatility * volatility, weight);
        }
        return within;
      }());
}

void calculate_uncertainty(EnsembleForecast& forecast,
                           const std::array<WorkEntry, kMaximumExperts>& work,
                           const EnsembleRequest& request,
                           const EnsembleConfig& config) noexcept {
  const auto calibration_shortfall =
      weighted_score_shortfall(work, request.expert_count, request,
                               &models::ModelPrediction::calibration_score_ppm);
  const auto data_shortfall =
      weighted_score_shortfall(work, request.expert_count, request,
                               &models::ModelPrediction::data_quality_score_ppm);
  const auto ood = weighted_ood(work, request.expert_count, request);
  auto uncertainty = integer_sqrt(forecast.variance_ppm_squared);
  uncertainty +=
      scaled_multiply(calibration_shortfall,
                      config.calibration_uncertainty_multiplier_ppm) +
      scaled_multiply(ood, config.ood_uncertainty_multiplier_ppm) +
      scaled_multiply(data_shortfall, config.data_quality_uncertainty_multiplier_ppm);
  forecast.effective_uncertainty_ppm =
      std::min(uncertainty, kMaximumEffectiveUncertaintyPpm);
  forecast.uncertainty_penalty_ppm =
      std::min(scaled_multiply(forecast.effective_uncertainty_ppm,
                               config.uncertainty_penalty_multiplier_ppm),
               kMaximumAggregateCostPpm);
}

[[nodiscard]] std::uint64_t absolute_return(const std::int64_t value) noexcept {
  return static_cast<std::uint64_t>(value < 0 ? -value : value);
}

void set_edge_decision(EnsembleForecast& forecast) noexcept {
  const auto required = forecast.estimated_transaction_cost_ppm +
                        forecast.uncertainty_penalty_ppm + forecast.safety_margin_ppm;
  forecast.explanation.required_edge_ppm = required;
  if (forecast.expected_return_ppm > 0) {
    forecast.net_robust_edge_ppm =
        forecast.expected_return_ppm - static_cast<std::int64_t>(required);
  } else if (forecast.expected_return_ppm < 0) {
    forecast.net_robust_edge_ppm =
        forecast.expected_return_ppm + static_cast<std::int64_t>(required);
  }
  forecast.abstain = absolute_return(forecast.expected_return_ppm) <= required;
  set_decision_reason(forecast, forecast.abstain
                                    ? DecisionReason::insufficient_robust_edge
                                    : DecisionReason::combined);
}

} // namespace

MixtureOfExpertsGate::MixtureOfExpertsGate(EnsembleConfig config) noexcept
    : config_(config), initialized_(valid_ensemble_config(config_)) {}

bool MixtureOfExpertsGate::initialized() const noexcept { return initialized_; }

const EnsembleConfig& MixtureOfExpertsGate::config() const noexcept { return config_; }

EvaluationResult
MixtureOfExpertsGate::evaluate(const EnsembleRequest& request) const noexcept {
  auto forecast = base_forecast(request, config_);
  if (!initialized_) {
    return early_abstention(forecast, EvaluationStatus::invalid_configuration,
                            DecisionReason::invalid_configuration);
  }
  if (!valid_request_shape(request, config_)) {
    return early_abstention(forecast, EvaluationStatus::invalid_request,
                            DecisionReason::invalid_request);
  }

  std::uint64_t cost{};
  if (!transaction_cost(request, cost)) {
    return early_abstention(forecast, EvaluationStatus::abstained,
                            DecisionReason::invalid_transaction_cost);
  }
  forecast.estimated_transaction_cost_ppm = cost;
  forecast.explanation.required_edge_ppm = cost + forecast.safety_margin_ppm;
  forecast.explanation.transaction_cost_forecast_hash =
      request.transaction_cost_forecast.stable_hash;
  forecast.valid_until_process_monotonic_time_ns =
      std::min(forecast.valid_until_process_monotonic_time_ns,
               request.transaction_cost_forecast.expiration_process_monotonic_time_ns);

  auto global_exclusion = EligibilityReason::eligible;
  auto global_reason = DecisionReason::combined;
  if (unsafe_market_state(request.market_state_snapshot.state)) {
    global_exclusion = EligibilityReason::incompatible_state;
    global_reason = DecisionReason::unsafe_market_state;
  } else if (invalid_feed_health(request.feed_health)) {
    global_exclusion = EligibilityReason::invalid_input_feed;
    global_reason = DecisionReason::invalid_input_feed;
  }

  std::array<WorkEntry, kMaximumExperts> work{};
  fill_work_entries(work, request, config_, global_exclusion);
  forecast.explanation.eligible_expert_count =
      score_eligible_experts(work, request, config_);
  if (global_exclusion != EligibilityReason::eligible) {
    publish_explanations(forecast, work, request.expert_count);
    return early_abstention(forecast, EvaluationStatus::abstained, global_reason);
  }
  if (forecast.explanation.eligible_expert_count == 0U) {
    publish_explanations(forecast, work, request.expert_count);
    return early_abstention(forecast, EvaluationStatus::abstained,
                            DecisionReason::no_eligible_expert);
  }
  if (positive_score_count(work, request.expert_count) == 0U) {
    publish_explanations(forecast, work, request.expert_count);
    return early_abstention(forecast, EvaluationStatus::abstained,
                            DecisionReason::no_positive_gate_score);
  }
  if (!normalize_weights(work, {.count = request.expert_count,
                                .cap = config_.maximum_expert_weight_ppm})) {
    publish_explanations(forecast, work, request.expert_count);
    return early_abstention(forecast, EvaluationStatus::abstained,
                            DecisionReason::weight_cap_infeasible);
  }

  publish_explanations(forecast, work, request.expert_count);
  set_dominant_expert(forecast, work, request.expert_count);
  for (std::size_t index = 0U; index < request.expert_count; ++index) {
    if (work[index].explanation.weight_ppm != 0U) {
      forecast.valid_until_process_monotonic_time_ns =
          std::min(forecast.valid_until_process_monotonic_time_ns,
                   request.experts[work[index].request_index]
                       .forecast.expiration_process_monotonic_time_ns);
    }
  }
  combine_distribution(forecast, work, request);
  calculate_uncertainty(forecast, work, request, config_);
  set_edge_decision(forecast);
  finalize_hashes(forecast);
  return {.status = forecast.abstain ? EvaluationStatus::abstained
                                     : EvaluationStatus::produced,
          .forecast = forecast};
}

} // namespace aegis::ensemble
