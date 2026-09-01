#include "aegis/ensemble/serialization.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace aegis::ensemble {
namespace {

namespace wire = common::wire;

[[nodiscard]] wire::SchemaVersion current_version() noexcept {
  return {common::kCurrentSchemaMajor, common::kCurrentSchemaMinor,
          common::kCurrentSchemaPatch};
}

[[nodiscard]] wire::ForecastId to_wire(const common::ForecastId value) noexcept {
  return {value.high(), value.low()};
}

[[nodiscard]] wire::ModelId to_wire(const common::ModelId value) noexcept {
  return {value.high(), value.low()};
}

[[nodiscard]] wire::ModelVersion to_wire(const common::ModelVersion value) noexcept {
  return {value.high(), value.low()};
}

[[nodiscard]] wire::EnsembleModelWeight
to_wire(const ExpertExplanation& explanation) noexcept {
  return {to_wire(explanation.forecast_id),
          to_wire(explanation.model_id),
          to_wire(explanation.model_version),
          static_cast<wire::EnsembleExpertRoleCode>(explanation.role),
          static_cast<wire::EnsembleEligibilityCode>(explanation.eligibility),
          explanation.weight_ppm,
          explanation.raw_gate_score,
          explanation.freshness_ppm,
          explanation.forecast_hash};
}

} // namespace

flatbuffers::DetachedBuffer
build_ensemble_forecast_contract(const EnsembleContractInput& input) {
  if (!input.record_id.valid() || !valid_ensemble_forecast(input.forecast)) {
    return {};
  }
  const auto& forecast = input.forecast;
  const auto& explanation = forecast.explanation;
  flatbuffers::FlatBufferBuilder builder{4096U};
  const auto version = current_version();
  const wire::GlobalEventId record_id{input.record_id.high(), input.record_id.low()};
  const wire::ForecastId forecast_id = to_wire(forecast.forecast_id);
  const wire::SessionId session_id{forecast.session_id.high(),
                                   forecast.session_id.low()};
  const wire::InstrumentId instrument_id{forecast.instrument_id.high(),
                                         forecast.instrument_id.low()};
  const wire::ModelId ensemble_model_id = to_wire(forecast.ensemble_model_id);
  const wire::ModelVersion ensemble_model_version =
      to_wire(forecast.ensemble_model_version);
  const wire::ConfigurationVersion configuration_version{
      forecast.configuration_version.high(), forecast.configuration_version.low()};
  const wire::ProcessMonotonicTimeNs created_time{
      forecast.created_process_monotonic_time_ns};
  const wire::ProcessMonotonicTimeNs valid_until_time{
      forecast.valid_until_process_monotonic_time_ns};

  std::array<wire::ForecastId, kMaximumExperts> contributors{};
  std::array<wire::EnsembleModelWeight, kMaximumExperts> weights{};
  std::size_t contributor_count{};
  for (std::size_t index = 0U; index < explanation.supplied_expert_count; ++index) {
    const auto& expert = explanation.experts[index];
    weights[index] = to_wire(expert);
    if (expert.weight_ppm != 0U) {
      contributors[contributor_count] = to_wire(expert.forecast_id);
      ++contributor_count;
    }
  }
  const auto contributor_vector =
      builder.CreateVectorOfStructs(contributors.data(), contributor_count);
  const auto weight_vector = builder.CreateVectorOfStructs(
      weights.data(), static_cast<std::size_t>(explanation.supplied_expert_count));

  const auto dominant_forecast_id = to_wire(explanation.dominant_forecast_id);
  const auto dominant_model_id = to_wire(explanation.dominant_model_id);
  const auto dominant_model_version = to_wire(explanation.dominant_model_version);
  const auto decision_explanation = wire::CreateEnsembleDecisionExplanation(
      builder, static_cast<wire::EnsembleGateKind>(explanation.gate_kind),
      static_cast<wire::EnsembleMarketStateCode>(
          static_cast<std::uint8_t>(explanation.market_state) + 1U),
      static_cast<wire::EnsembleEventStateCode>(explanation.event_state),
      explanation.input_data_quality_ppm, explanation.supplied_expert_count,
      explanation.eligible_expert_count, &dominant_forecast_id, &dominant_model_id,
      &dominant_model_version,
      static_cast<wire::EnsembleDominantExpertReasonCode>(explanation.dominant_reason),
      explanation.required_edge_ppm, explanation.learned_gate_signature_hash,
      explanation.stable_hash, explanation.contributing_expert_count,
      explanation.transaction_cost_forecast_hash,
      explanation.market_state_snapshot_hash, explanation.configuration_hash);

  const auto confidence =
      std::max(forecast.probability_down_ppm, forecast.probability_up_ppm);
  const auto wire_forecast = wire::CreateEnsembleForecast(
      builder, &version, &forecast_id, &session_id, &instrument_id, &ensemble_model_id,
      &ensemble_model_version, &configuration_version, wire::ForecastUnit::RETURN_PPM,
      forecast.expected_return_ppm, confidence, forecast.horizon_ns, &created_time,
      &valid_until_time, contributor_vector, forecast.expected_return_ppm,
      forecast.probability_down_ppm, forecast.probability_flat_ppm,
      forecast.probability_up_ppm, forecast.variance_ppm_squared,
      forecast.return_p10_ppm, forecast.return_p50_ppm, forecast.return_p90_ppm,
      forecast.effective_uncertainty_ppm, forecast.disagreement_ppm, weight_vector,
      forecast.estimated_transaction_cost_ppm, forecast.uncertainty_penalty_ppm,
      forecast.safety_margin_ppm, forecast.net_robust_edge_ppm, forecast.abstain,
      static_cast<wire::EnsembleDecisionReasonCode>(forecast.primary_reason),
      forecast.reason_mask, decision_explanation, forecast.stable_hash);
  const auto record = wire::CreateContractRecord(
      builder, &version, &record_id, wire::RecordType::ENSEMBLE_FORECAST,
      wire::ContractPayload::EnsembleForecast, wire_forecast.Union());
  wire::FinishContractRecordBuffer(builder, record);
  return builder.Release();
}

} // namespace aegis::ensemble
