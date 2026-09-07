#include "aegis/observability/decision_explanation.hpp"

#include <array>
#include <bit>
#include <locale>
#include <sstream>

namespace aegis::observability {
namespace {

constexpr std::uint64_t kFnvOffset = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;

void mix(std::uint64_t& hash, const std::uint64_t value) noexcept {
  for (unsigned shift = 0U; shift < 64U; shift += 8U) {
    hash ^= (value >> shift) & 0xFFU;
    hash *= kFnvPrime;
  }
}

template <typename Identifier>
void mix_identifier(std::uint64_t& hash, const Identifier& value) noexcept {
  mix(hash, value.high());
  mix(hash, value.low());
}

void mix_expert(std::uint64_t& hash, const ExpertDecisionRecord& expert) noexcept {
  mix_identifier(hash, expert.forecast_id);
  mix_identifier(hash, expert.model_id);
  mix_identifier(hash, expert.model_version);
  mix_identifier(hash, expert.feature_snapshot_id);
  mix(hash, static_cast<std::uint64_t>(expert.role));
  mix(hash, static_cast<std::uint64_t>(expert.eligibility));
  mix(hash, static_cast<std::uint64_t>(expert.health));
  mix(hash, static_cast<std::uint64_t>(expert.calibration_health));
  mix(hash, expert.weight_ppm);
  mix(hash, expert.freshness_ppm);
  mix(hash, std::bit_cast<std::uint64_t>(expert.expected_return_ppm));
  mix(hash, std::bit_cast<std::uint64_t>(expert.return_p10_ppm));
  mix(hash, std::bit_cast<std::uint64_t>(expert.return_p50_ppm));
  mix(hash, std::bit_cast<std::uint64_t>(expert.return_p90_ppm));
  mix(hash, expert.probability_down_ppm);
  mix(hash, expert.probability_flat_ppm);
  mix(hash, expert.probability_up_ppm);
  mix(hash, expert.volatility_ppm);
  mix(hash, expert.confidence_ppm);
  mix(hash, expert.calibration_score_ppm);
  mix(hash, expert.data_quality_score_ppm);
  mix(hash, expert.ood_score_ppm);
  mix(hash, expert.production_process_monotonic_time_ns);
  mix(hash, expert.expiration_process_monotonic_time_ns);
  mix(hash, expert.forecast_hash);
}

[[nodiscard]] const ensemble::ExpertInput*
find_expert(const ensemble::EnsembleRequest& request,
            const ensemble::ExpertExplanation& explanation,
            std::array<bool, ensemble::kMaximumExperts>& used) noexcept {
  for (std::size_t index = 0U; index < request.expert_count; ++index) {
    const auto& forecast = request.experts[index].forecast;
    if (!used[index] && forecast.forecast_id == explanation.forecast_id &&
        forecast.model_id == explanation.model_id &&
        forecast.model_version == explanation.model_version &&
        forecast.stable_hash == explanation.forecast_hash) {
      used[index] = true;
      return &request.experts[index];
    }
  }
  return nullptr;
}

[[nodiscard]] DecisionDisposition
disposition(const ensemble::EnsembleForecast& forecast,
            const risk::RiskDecision* risk_decision,
            const execution::RoutingDecision* routing_decision) noexcept {
  if (forecast.abstain) {
    return DecisionDisposition::abstained;
  }
  if (risk_decision == nullptr) {
    return DecisionDisposition::awaiting_risk;
  }
  if (risk_decision->decision == risk::DecisionCode::rejected) {
    return DecisionDisposition::risk_rejected;
  }
  if (routing_decision == nullptr ||
      routing_decision->status != execution::RoutingStatus::routed) {
    return DecisionDisposition::awaiting_route;
  }
  return DecisionDisposition::routed;
}

void append_identifier(std::ostringstream& output, const char* name,
                       const auto identifier) {
  const auto value = common::to_hex(identifier);
  output << "\"" << name << "\":\"" << value.data() << "\"";
}

class BinaryWriter final {
public:
  explicit BinaryWriter(const std::span<std::uint8_t> output) noexcept
      : output_(output) {}

  void u8(const std::uint8_t value) noexcept { put(value); }
  void u16(const std::uint16_t value) noexcept { integral(value); }
  void u32(const std::uint32_t value) noexcept { integral(value); }
  void u64(const std::uint64_t value) noexcept { integral(value); }
  void i64(const std::int64_t value) noexcept {
    integral(std::bit_cast<std::uint64_t>(value));
  }
  void boolean(const bool value) noexcept { u8(value ? 1U : 0U); }
  void identifier(const auto& value) noexcept {
    u64(value.high());
    u64(value.low());
  }
  [[nodiscard]] bool valid() const noexcept { return valid_; }
  [[nodiscard]] std::size_t size() const noexcept { return position_; }

private:
  void put(const std::uint8_t value) noexcept {
    if (position_ >= output_.size()) {
      valid_ = false;
      return;
    }
    output_[position_++] = value;
  }
  template <typename Value> void integral(const Value value) noexcept {
    for (std::size_t index = 0U; index < sizeof(Value); ++index) {
      put(static_cast<std::uint8_t>(value >> (index * 8U)));
    }
  }
  std::span<std::uint8_t> output_;
  std::size_t position_{};
  bool valid_{true};
};

class BinaryReader final {
public:
  explicit BinaryReader(const std::span<const std::uint8_t> input) noexcept
      : input_(input) {}
  [[nodiscard]] std::uint8_t u8() noexcept { return integral<std::uint8_t>(); }
  [[nodiscard]] std::uint16_t u16() noexcept { return integral<std::uint16_t>(); }
  [[nodiscard]] std::uint32_t u32() noexcept { return integral<std::uint32_t>(); }
  [[nodiscard]] std::uint64_t u64() noexcept { return integral<std::uint64_t>(); }
  [[nodiscard]] std::int64_t i64() noexcept {
    return std::bit_cast<std::int64_t>(u64());
  }
  [[nodiscard]] bool boolean() noexcept {
    const auto value = u8();
    if (value > 1U) {
      valid_ = false;
    }
    return value == 1U;
  }
  template <typename Identifier> [[nodiscard]] Identifier identifier() noexcept {
    return Identifier{u64(), u64()};
  }
  template <typename Enum>
  [[nodiscard]] Enum enumeration(const std::array<Enum, 2U>& valid_range,
                                 const Enum fallback) noexcept {
    static_assert(std::is_enum_v<Enum>);
    const auto raw = u8();
    const auto minimum_raw = static_cast<std::uint8_t>(valid_range.front());
    const auto maximum_raw = static_cast<std::uint8_t>(valid_range.back());
    if (raw < minimum_raw || raw > maximum_raw) {
      valid_ = false;
      return fallback;
    }
    // The range is supplied by the concrete enum call site. The analyzer does
    // not propagate those template arguments through std::array, but the
    // guarded cast cannot observe an out-of-range value.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    return static_cast<Enum>(raw);
  }
  [[nodiscard]] bool complete() const noexcept {
    return valid_ && position_ == input_.size();
  }

private:
  template <typename Value> [[nodiscard]] Value integral() noexcept {
    std::uint64_t result{};
    for (std::size_t index = 0U; index < sizeof(Value); ++index) {
      if (position_ >= input_.size()) {
        valid_ = false;
        return {};
      }
      result |= static_cast<std::uint64_t>(input_[position_++]) << (index * 8U);
    }
    return static_cast<Value>(result);
  }
  std::span<const std::uint8_t> input_;
  std::size_t position_{};
  bool valid_{true};
};

void write_expert(BinaryWriter& writer, const ExpertDecisionRecord& value) noexcept {
  writer.identifier(value.forecast_id);
  writer.identifier(value.model_id);
  writer.identifier(value.model_version);
  writer.identifier(value.feature_snapshot_id);
  writer.u8(static_cast<std::uint8_t>(value.role));
  writer.u8(static_cast<std::uint8_t>(value.eligibility));
  writer.u8(static_cast<std::uint8_t>(value.health));
  writer.u8(static_cast<std::uint8_t>(value.calibration_health));
  writer.u32(value.weight_ppm);
  writer.u32(value.freshness_ppm);
  writer.i64(value.expected_return_ppm);
  writer.i64(value.return_p10_ppm);
  writer.i64(value.return_p50_ppm);
  writer.i64(value.return_p90_ppm);
  writer.u32(value.probability_down_ppm);
  writer.u32(value.probability_flat_ppm);
  writer.u32(value.probability_up_ppm);
  writer.u64(value.volatility_ppm);
  writer.u32(value.confidence_ppm);
  writer.u32(value.calibration_score_ppm);
  writer.u32(value.data_quality_score_ppm);
  writer.u32(value.ood_score_ppm);
  writer.u64(value.production_process_monotonic_time_ns);
  writer.u64(value.expiration_process_monotonic_time_ns);
  writer.u64(value.forecast_hash);
}

ExpertDecisionRecord read_expert(BinaryReader& reader) noexcept {
  ExpertDecisionRecord value{};
  value.forecast_id = reader.identifier<common::ForecastId>();
  value.model_id = reader.identifier<common::ModelId>();
  value.model_version = reader.identifier<common::ModelVersion>();
  value.feature_snapshot_id = reader.identifier<common::FeatureSnapshotId>();
  value.role = reader.enumeration(
      std::array{ensemble::ExpertRole::generic, ensemble::ExpertRole::auction},
      ensemble::ExpertRole::generic);
  value.eligibility =
      reader.enumeration(std::array{ensemble::EligibilityReason::eligible,
                                    ensemble::EligibilityReason::duplicate_expert},
                         ensemble::EligibilityReason::invalid_forecast);
  value.health = reader.enumeration(
      std::array{models::ModelHealthState::unknown, models::ModelHealthState::failed},
      models::ModelHealthState::unknown);
  value.calibration_health =
      reader.enumeration(std::array{ensemble::CalibrationHealth::unknown,
                                    ensemble::CalibrationHealth::failed},
                         ensemble::CalibrationHealth::unknown);
  value.weight_ppm = reader.u32();
  value.freshness_ppm = reader.u32();
  value.expected_return_ppm = reader.i64();
  value.return_p10_ppm = reader.i64();
  value.return_p50_ppm = reader.i64();
  value.return_p90_ppm = reader.i64();
  value.probability_down_ppm = reader.u32();
  value.probability_flat_ppm = reader.u32();
  value.probability_up_ppm = reader.u32();
  value.volatility_ppm = reader.u64();
  value.confidence_ppm = reader.u32();
  value.calibration_score_ppm = reader.u32();
  value.data_quality_score_ppm = reader.u32();
  value.ood_score_ppm = reader.u32();
  value.production_process_monotonic_time_ns = reader.u64();
  value.expiration_process_monotonic_time_ns = reader.u64();
  value.forecast_hash = reader.u64();
  return value;
}

} // namespace

std::uint64_t
stable_decision_explanation_hash(DecisionExplanationRecord record) noexcept {
  record.stable_hash = 0U;
  std::uint64_t hash = kFnvOffset;
  mix(hash, record.schema_major);
  mix(hash, record.schema_minor);
  mix_identifier(hash, record.explanation_id);
  mix_identifier(hash, record.correlation_id);
  mix_identifier(hash, record.session_id);
  mix_identifier(hash, record.instrument_id);
  mix_identifier(hash, record.configuration_version);
  mix_identifier(hash, record.ensemble_forecast_id);
  mix(hash, record.created_process_monotonic_time_ns);
  mix(hash, std::bit_cast<std::uint64_t>(record.created_wall_clock_utc_time_ns));
  mix(hash, static_cast<std::uint64_t>(record.market_state));
  mix(hash, static_cast<std::uint64_t>(record.event_state));
  mix(hash, record.market_state_snapshot_hash);
  mix(hash, record.input_data_quality_ppm);
  mix(hash, record.expert_count);
  mix(hash, record.eligible_expert_count);
  for (const auto& expert : record.experts) {
    mix_expert(hash, expert);
  }
  mix(hash, std::bit_cast<std::uint64_t>(record.combined_expected_return_ppm));
  mix(hash, record.combined_probability_down_ppm);
  mix(hash, record.combined_probability_flat_ppm);
  mix(hash, record.combined_probability_up_ppm);
  mix(hash, record.combined_variance_ppm_squared);
  mix(hash, std::bit_cast<std::uint64_t>(record.combined_return_p10_ppm));
  mix(hash, std::bit_cast<std::uint64_t>(record.combined_return_p50_ppm));
  mix(hash, std::bit_cast<std::uint64_t>(record.combined_return_p90_ppm));
  mix(hash, record.effective_uncertainty_ppm);
  mix(hash, record.disagreement_ppm);
  mix(hash, record.estimated_transaction_cost_ppm);
  mix(hash, record.uncertainty_penalty_ppm);
  mix(hash, record.safety_margin_ppm);
  mix(hash, std::bit_cast<std::uint64_t>(record.net_robust_edge_ppm));
  mix(hash, record.abstain ? 1U : 0U);
  mix(hash, static_cast<std::uint64_t>(record.ensemble_reason));
  mix(hash, record.ensemble_reason_mask);
  mix(hash, record.ensemble_hash);
  mix(hash, record.transaction_cost_forecast_hash);
  mix(hash, record.risk_present ? 1U : 0U);
  mix_identifier(hash, record.risk_intent_id);
  mix(hash, static_cast<std::uint64_t>(record.risk_decision));
  mix(hash, static_cast<std::uint64_t>(record.risk_reason));
  mix(hash, static_cast<std::uint64_t>(record.failed_risk_check));
  mix_identifier(hash, record.risk_snapshot_id);
  mix(hash, record.approved_quantity_units);
  mix(hash, std::bit_cast<std::uint64_t>(record.approved_price_ticks));
  mix(hash, record.risk_snapshot_hash);
  mix(hash, record.risk_context_hash);
  mix(hash, record.risk_decision_hash);
  mix(hash, record.routing_present ? 1U : 0U);
  mix(hash, static_cast<std::uint64_t>(record.routing_status));
  mix(hash, static_cast<std::uint64_t>(record.routing_reason));
  mix_identifier(hash, record.selected_venue_id);
  mix(hash, static_cast<std::uint64_t>(record.routed_action));
  mix(hash, static_cast<std::uint64_t>(record.time_in_force));
  mix(hash, std::bit_cast<std::uint64_t>(record.routed_price_ticks));
  mix(hash, record.routed_quantity_units);
  mix(hash, std::bit_cast<std::uint64_t>(
                record.routed_expected_value_currency_nanos_per_unit));
  mix(hash, record.evaluated_venue_count);
  mix(hash, record.eligible_venue_count);
  mix(hash, record.routing_decision_hash);
  mix(hash, static_cast<std::uint64_t>(record.disposition));
  return hash == 0U ? 1U : hash;
}

bool serialize_decision_explanation(const DecisionExplanationRecord& value,
                                    const std::span<std::uint8_t> output,
                                    std::size_t& bytes_written) noexcept {
  bytes_written = 0U;
  if (!valid_decision_explanation(value)) {
    return false;
  }
  BinaryWriter writer{output};
  writer.u16(value.schema_major);
  writer.u16(value.schema_minor);
  writer.identifier(value.explanation_id);
  writer.identifier(value.correlation_id);
  writer.identifier(value.session_id);
  writer.identifier(value.instrument_id);
  writer.identifier(value.configuration_version);
  writer.identifier(value.ensemble_forecast_id);
  writer.u64(value.created_process_monotonic_time_ns);
  writer.i64(value.created_wall_clock_utc_time_ns);
  writer.u8(static_cast<std::uint8_t>(value.market_state));
  writer.u8(static_cast<std::uint8_t>(value.event_state));
  writer.u64(value.market_state_snapshot_hash);
  writer.u32(value.input_data_quality_ppm);
  writer.u32(value.expert_count);
  writer.u32(value.eligible_expert_count);
  for (const auto& expert : value.experts) {
    write_expert(writer, expert);
  }
  writer.i64(value.combined_expected_return_ppm);
  writer.u32(value.combined_probability_down_ppm);
  writer.u32(value.combined_probability_flat_ppm);
  writer.u32(value.combined_probability_up_ppm);
  writer.u64(value.combined_variance_ppm_squared);
  writer.i64(value.combined_return_p10_ppm);
  writer.i64(value.combined_return_p50_ppm);
  writer.i64(value.combined_return_p90_ppm);
  writer.u64(value.effective_uncertainty_ppm);
  writer.u64(value.disagreement_ppm);
  writer.u64(value.estimated_transaction_cost_ppm);
  writer.u64(value.uncertainty_penalty_ppm);
  writer.u64(value.safety_margin_ppm);
  writer.i64(value.net_robust_edge_ppm);
  writer.boolean(value.abstain);
  writer.u8(static_cast<std::uint8_t>(value.ensemble_reason));
  writer.u64(value.ensemble_reason_mask);
  writer.u64(value.ensemble_hash);
  writer.u64(value.transaction_cost_forecast_hash);
  writer.boolean(value.risk_present);
  writer.identifier(value.risk_intent_id);
  writer.u8(static_cast<std::uint8_t>(value.risk_decision));
  writer.u8(static_cast<std::uint8_t>(value.risk_reason));
  writer.u8(static_cast<std::uint8_t>(value.failed_risk_check));
  writer.identifier(value.risk_snapshot_id);
  writer.u64(value.approved_quantity_units);
  writer.i64(value.approved_price_ticks);
  writer.u64(value.risk_snapshot_hash);
  writer.u64(value.risk_context_hash);
  writer.u64(value.risk_decision_hash);
  writer.boolean(value.routing_present);
  writer.u8(static_cast<std::uint8_t>(value.routing_status));
  writer.u8(static_cast<std::uint8_t>(value.routing_reason));
  writer.identifier(value.selected_venue_id);
  writer.u8(static_cast<std::uint8_t>(value.routed_action));
  writer.u8(static_cast<std::uint8_t>(value.time_in_force));
  writer.i64(value.routed_price_ticks);
  writer.u64(value.routed_quantity_units);
  writer.i64(value.routed_expected_value_currency_nanos_per_unit);
  writer.u16(value.evaluated_venue_count);
  writer.u16(value.eligible_venue_count);
  writer.u64(value.routing_decision_hash);
  writer.u8(static_cast<std::uint8_t>(value.disposition));
  writer.u64(value.stable_hash);
  if (!writer.valid()) {
    return false;
  }
  bytes_written = writer.size();
  return true;
}

bool deserialize_decision_explanation(const std::span<const std::uint8_t> input,
                                      DecisionExplanationRecord& output) noexcept {
  BinaryReader reader{input};
  DecisionExplanationRecord value{};
  value.schema_major = reader.u16();
  value.schema_minor = reader.u16();
  value.explanation_id = reader.identifier<common::GlobalEventId>();
  value.correlation_id = reader.identifier<common::GlobalEventId>();
  value.session_id = reader.identifier<common::SessionId>();
  value.instrument_id = reader.identifier<common::InstrumentId>();
  value.configuration_version = reader.identifier<common::ConfigurationVersion>();
  value.ensemble_forecast_id = reader.identifier<common::ForecastId>();
  value.created_process_monotonic_time_ns = reader.u64();
  value.created_wall_clock_utc_time_ns = reader.i64();
  value.market_state =
      reader.enumeration(std::array{market_state::MarketState::startup,
                                    market_state::MarketState::shutdown},
                         market_state::MarketState::startup);
  value.event_state = reader.enumeration(
      std::array{ensemble::EventState::none, ensemble::EventState::volatility_shock},
      ensemble::EventState::none);
  value.market_state_snapshot_hash = reader.u64();
  value.input_data_quality_ppm = reader.u32();
  value.expert_count = reader.u32();
  value.eligible_expert_count = reader.u32();
  for (auto& expert : value.experts) {
    expert = read_expert(reader);
  }
  value.combined_expected_return_ppm = reader.i64();
  value.combined_probability_down_ppm = reader.u32();
  value.combined_probability_flat_ppm = reader.u32();
  value.combined_probability_up_ppm = reader.u32();
  value.combined_variance_ppm_squared = reader.u64();
  value.combined_return_p10_ppm = reader.i64();
  value.combined_return_p50_ppm = reader.i64();
  value.combined_return_p90_ppm = reader.i64();
  value.effective_uncertainty_ppm = reader.u64();
  value.disagreement_ppm = reader.u64();
  value.estimated_transaction_cost_ppm = reader.u64();
  value.uncertainty_penalty_ppm = reader.u64();
  value.safety_margin_ppm = reader.u64();
  value.net_robust_edge_ppm = reader.i64();
  value.abstain = reader.boolean();
  value.ensemble_reason =
      reader.enumeration(std::array{ensemble::DecisionReason::combined,
                                    ensemble::DecisionReason::numeric_error},
                         ensemble::DecisionReason::invalid_request);
  value.ensemble_reason_mask = reader.u64();
  value.ensemble_hash = reader.u64();
  value.transaction_cost_forecast_hash = reader.u64();
  value.risk_present = reader.boolean();
  value.risk_intent_id = reader.identifier<common::IntentId>();
  value.risk_decision = reader.enumeration(
      std::array{risk::DecisionCode::rejected, risk::DecisionCode::approved},
      risk::DecisionCode::rejected);
  value.risk_reason = reader.enumeration(
      std::array{risk::RiskReason::within_limits, risk::RiskReason::engine_busy},
      risk::RiskReason::risk_state_unavailable);
  value.failed_risk_check = reader.enumeration(
      std::array{risk::RiskCheck::none, risk::RiskCheck::configuration_freshness},
      risk::RiskCheck::none);
  value.risk_snapshot_id = reader.identifier<common::RiskSnapshotId>();
  value.approved_quantity_units = reader.u64();
  value.approved_price_ticks = reader.i64();
  value.risk_snapshot_hash = reader.u64();
  value.risk_context_hash = reader.u64();
  value.risk_decision_hash = reader.u64();
  value.routing_present = reader.boolean();
  value.routing_status = reader.enumeration(
      std::array{execution::RoutingStatus::routed, execution::RoutingStatus::stopped},
      execution::RoutingStatus::invalid_request);
  value.routing_reason = reader.enumeration(
      std::array{execution::RoutingReason::routed, execution::RoutingReason::stopped},
      execution::RoutingReason::invalid_request);
  value.selected_venue_id = reader.identifier<common::VenueId>();
  value.routed_action = reader.enumeration(
      std::array{execution::RoutedAction::new_order, execution::RoutedAction::replace},
      execution::RoutedAction::new_order);
  value.time_in_force =
      reader.enumeration(std::array{execution::RoutedTimeInForce::day,
                                    execution::RoutedTimeInForce::auction},
                         execution::RoutedTimeInForce::day);
  value.routed_price_ticks = reader.i64();
  value.routed_quantity_units = reader.u64();
  value.routed_expected_value_currency_nanos_per_unit = reader.i64();
  value.evaluated_venue_count = reader.u16();
  value.eligible_venue_count = reader.u16();
  value.routing_decision_hash = reader.u64();
  value.disposition = reader.enumeration(
      std::array{DecisionDisposition::abstained, DecisionDisposition::routed},
      DecisionDisposition::abstained);
  value.stable_hash = reader.u64();
  if (!reader.complete() || !valid_decision_explanation(value)) {
    return false;
  }
  output = value;
  return true;
}

DecisionExplanationJournalResult DecisionExplanationJournalPublisher::publish(
    const DecisionExplanationRecord& record) noexcept {
  std::array<std::uint8_t, kMaximumDecisionExplanationBinaryBytes> payload{};
  std::size_t size{};
  if (!serialize_decision_explanation(record, payload, size)) {
    return {.status = journal::Status::invalid_payload};
  }
  const auto published = journal_.try_publish(
      {.metadata = {.kind = journal::RecordKind::ensemble_decision,
                    .encoding = journal::PayloadEncoding::opaque_binary,
                    .priority = journal::RecordPriority::mandatory,
                    .schema_version = {.major = record.schema_major,
                                       .minor = record.schema_minor,
                                       .patch = 0U},
                    .created_process_monotonic_time_ns =
                        record.created_process_monotonic_time_ns,
                    .recorded_wall_clock_utc_time_ns =
                        record.created_wall_clock_utc_time_ns,
                    .source_event_sequence = record.correlation_id.low(),
                    .global_event_id = record.explanation_id},
       .payload = std::span<const std::uint8_t>{payload.data(), size}});
  return {.status = published.status, .producer_sequence = published.producer_sequence};
}

// Validation deliberately keeps the complete record invariant in one audit boundary.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool valid_decision_explanation(const DecisionExplanationRecord& record) noexcept {
  if (record.schema_major != 1U || record.schema_minor != 0U ||
      !record.explanation_id.valid() || !record.correlation_id.valid() ||
      !record.session_id.valid() || !record.instrument_id.valid() ||
      !record.configuration_version.valid() || !record.ensemble_forecast_id.valid() ||
      record.created_process_monotonic_time_ns == 0U ||
      record.created_wall_clock_utc_time_ns <= 0 ||
      !market_state::valid_market_state(record.market_state) ||
      !ensemble::valid_event_state(record.event_state) ||
      record.input_data_quality_ppm > models::kProbabilityScale ||
      record.expert_count > ensemble::kMaximumExperts ||
      record.eligible_expert_count > record.expert_count ||
      record.combined_probability_down_ppm + record.combined_probability_flat_ppm +
              record.combined_probability_up_ppm !=
          models::kProbabilityScale ||
      record.combined_expected_return_ppm < -models::kMaximumAbsoluteReturnPpm ||
      record.combined_expected_return_ppm > models::kMaximumAbsoluteReturnPpm ||
      record.combined_return_p10_ppm > record.combined_return_p50_ppm ||
      record.combined_return_p50_ppm > record.combined_return_p90_ppm ||
      record.ensemble_reason < ensemble::DecisionReason::combined ||
      record.ensemble_reason > ensemble::DecisionReason::numeric_error ||
      record.market_state_snapshot_hash == 0U || record.ensemble_hash == 0U ||
      record.disposition < DecisionDisposition::abstained ||
      record.disposition > DecisionDisposition::routed || record.stable_hash == 0U ||
      record.stable_hash != stable_decision_explanation_hash(record)) {
    return false;
  }
  std::uint32_t eligible_count = 0U;
  std::uint64_t weight_sum = 0U;
  for (std::size_t index = 0U; index < record.expert_count; ++index) {
    const auto& expert = record.experts[index];
    if (!ensemble::valid_expert_role(expert.role) ||
        expert.eligibility < ensemble::EligibilityReason::eligible ||
        expert.eligibility > ensemble::EligibilityReason::duplicate_expert ||
        expert.health > models::ModelHealthState::failed ||
        expert.calibration_health > ensemble::CalibrationHealth::failed ||
        expert.weight_ppm > models::kProbabilityScale ||
        expert.freshness_ppm > models::kProbabilityScale) {
      return false;
    }
    const auto eligible = expert.eligibility == ensemble::EligibilityReason::eligible;
    if ((!eligible && expert.weight_ppm != 0U) ||
        (eligible &&
         (!expert.forecast_id.valid() || !expert.model_id.valid() ||
          !expert.model_version.valid() || !expert.feature_snapshot_id.valid() ||
          expert.expected_return_ppm < -models::kMaximumAbsoluteReturnPpm ||
          expert.expected_return_ppm > models::kMaximumAbsoluteReturnPpm ||
          expert.return_p10_ppm < -models::kMaximumAbsoluteReturnPpm ||
          expert.return_p90_ppm > models::kMaximumAbsoluteReturnPpm ||
          expert.return_p10_ppm > expert.return_p50_ppm ||
          expert.return_p50_ppm > expert.return_p90_ppm ||
          expert.probability_down_ppm + expert.probability_flat_ppm +
                  expert.probability_up_ppm !=
              models::kProbabilityScale ||
          expert.volatility_ppm > models::kMaximumVolatilityPpm ||
          expert.confidence_ppm > models::kProbabilityScale ||
          expert.calibration_score_ppm > models::kProbabilityScale ||
          expert.data_quality_score_ppm > models::kProbabilityScale ||
          expert.ood_score_ppm > models::kProbabilityScale ||
          expert.production_process_monotonic_time_ns == 0U ||
          expert.expiration_process_monotonic_time_ns <=
              expert.production_process_monotonic_time_ns ||
          expert.forecast_hash == 0U))) {
      return false;
    }
    eligible_count +=
        expert.eligibility == ensemble::EligibilityReason::eligible ? 1U : 0U;
    weight_sum += expert.weight_ppm;
  }
  // A robust-edge abstention can retain a normalized expert mixture so its
  // disagreement and uncertainty remain auditable. Early abstentions have no
  // contributing experts and therefore retain a zero sum.
  if (eligible_count != record.eligible_expert_count ||
      (weight_sum != 0U && weight_sum != models::kProbabilityScale) ||
      (!record.abstain && weight_sum != models::kProbabilityScale)) {
    return false;
  }
  for (std::size_t index = record.expert_count; index < record.experts.size();
       ++index) {
    const auto& unused = record.experts[index];
    if (unused.forecast_id.valid() || unused.model_id.valid() ||
        unused.model_version.valid() || unused.feature_snapshot_id.valid() ||
        unused.role != ensemble::ExpertRole::generic ||
        unused.eligibility != ensemble::EligibilityReason::invalid_forecast ||
        unused.health != models::ModelHealthState::unknown ||
        unused.calibration_health != ensemble::CalibrationHealth::unknown ||
        unused.weight_ppm != 0U || unused.forecast_hash != 0U ||
        unused.freshness_ppm != 0U || unused.expected_return_ppm != 0 ||
        unused.return_p10_ppm != 0 || unused.return_p50_ppm != 0 ||
        unused.return_p90_ppm != 0 || unused.probability_down_ppm != 0U ||
        unused.probability_flat_ppm != 0U || unused.probability_up_ppm != 0U ||
        unused.volatility_ppm != 0U || unused.confidence_ppm != 0U ||
        unused.calibration_score_ppm != 0U || unused.data_quality_score_ppm != 0U ||
        unused.ood_score_ppm != 0U ||
        unused.production_process_monotonic_time_ns != 0U ||
        unused.expiration_process_monotonic_time_ns != 0U) {
      return false;
    }
  }
  if (record.risk_present &&
      (!record.risk_intent_id.valid() || record.risk_decision_hash == 0U ||
       !record.risk_snapshot_id.valid() || record.risk_snapshot_hash == 0U ||
       record.risk_context_hash == 0U ||
       record.risk_decision < risk::DecisionCode::rejected ||
       record.risk_decision > risk::DecisionCode::approved ||
       record.risk_reason < risk::RiskReason::within_limits ||
       record.risk_reason > risk::RiskReason::engine_busy ||
       record.failed_risk_check > risk::RiskCheck::configuration_freshness)) {
    return false;
  }
  if (!record.risk_present &&
      (record.risk_intent_id.valid() || record.risk_snapshot_id.valid() ||
       record.approved_quantity_units != 0U || record.approved_price_ticks != 0 ||
       record.risk_snapshot_hash != 0U || record.risk_context_hash != 0U ||
       record.risk_decision_hash != 0U ||
       record.risk_decision != risk::DecisionCode::rejected ||
       record.risk_reason != risk::RiskReason::risk_state_unavailable ||
       record.failed_risk_check != risk::RiskCheck::none)) {
    return false;
  }
  if (record.routing_present &&
      (record.routing_decision_hash == 0U || record.evaluated_venue_count == 0U ||
       record.routing_status < execution::RoutingStatus::routed ||
       record.routing_status > execution::RoutingStatus::stopped ||
       record.routing_reason < execution::RoutingReason::routed ||
       record.routing_reason > execution::RoutingReason::stopped)) {
    return false;
  }
  if (!record.routing_present &&
      (record.selected_venue_id.valid() || record.routed_price_ticks != 0 ||
       record.routed_quantity_units != 0U ||
       record.routed_expected_value_currency_nanos_per_unit != 0 ||
       record.evaluated_venue_count != 0U || record.eligible_venue_count != 0U ||
       record.routing_decision_hash != 0U ||
       record.routing_status != execution::RoutingStatus::invalid_request ||
       record.routing_reason != execution::RoutingReason::invalid_request ||
       record.routed_action != execution::RoutedAction::new_order ||
       record.time_in_force != execution::RoutedTimeInForce::day)) {
    return false;
  }
  if (record.disposition == DecisionDisposition::abstained) {
    return record.abstain && !record.risk_present && !record.routing_present;
  }
  if (record.abstain) {
    return false;
  }
  if (record.risk_present) {
    const auto approved = record.risk_decision == risk::DecisionCode::approved;
    if (approved != (record.risk_reason == risk::RiskReason::within_limits) ||
        (approved && record.failed_risk_check != risk::RiskCheck::none)) {
      return false;
    }
  }
  if (record.routing_present) {
    const auto routed = record.routing_status == execution::RoutingStatus::routed;
    if (routed != record.selected_venue_id.valid() ||
        routed != (record.routing_reason == execution::RoutingReason::routed) ||
        (routed &&
         (record.routed_price_ticks <= 0 || record.routed_quantity_units == 0U))) {
      return false;
    }
  }
  if (record.disposition == DecisionDisposition::risk_rejected) {
    return record.risk_present && record.risk_decision == risk::DecisionCode::rejected;
  }
  if (record.disposition == DecisionDisposition::routed) {
    return record.risk_present && record.routing_present &&
           record.risk_decision == risk::DecisionCode::approved &&
           record.routing_status == execution::RoutingStatus::routed &&
           record.selected_venue_id.valid();
  }
  if (record.disposition == DecisionDisposition::awaiting_risk) {
    return !record.risk_present;
  }
  return record.disposition == DecisionDisposition::awaiting_route &&
         record.risk_present && record.risk_decision == risk::DecisionCode::approved &&
         (!record.routing_present ||
          record.routing_status != execution::RoutingStatus::routed);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool build_decision_explanation(const DecisionExplanationInput& input,
                                DecisionExplanationRecord& output) noexcept {
  if (!input.explanation_id.valid() || !input.correlation_id.valid() ||
      input.created_process_monotonic_time_ns == 0U ||
      input.created_wall_clock_utc_time_ns <= 0 || input.request == nullptr ||
      input.ensemble_forecast == nullptr ||
      !ensemble::valid_ensemble_forecast(*input.ensemble_forecast)) {
    return false;
  }
  const auto& request = *input.request;
  const auto& forecast = *input.ensemble_forecast;
  if (request.expert_count != forecast.explanation.supplied_expert_count ||
      request.expert_count > ensemble::kMaximumExperts ||
      request.session_id != forecast.session_id ||
      request.instrument_id != forecast.instrument_id ||
      request.configuration_version != forecast.configuration_version ||
      request.market_state_snapshot.stable_hash !=
          forecast.explanation.market_state_snapshot_hash ||
      (forecast.explanation.transaction_cost_forecast_hash != 0U &&
       (!request.transaction_cost_present ||
        request.transaction_cost_forecast.stable_hash !=
            forecast.explanation.transaction_cost_forecast_hash))) {
    return false;
  }
  if (input.risk_decision != nullptr &&
      (!risk::valid_risk_decision(*input.risk_decision) ||
       input.risk_decision->global_event_id != input.correlation_id ||
       input.risk_decision->session_id != forecast.session_id ||
       input.risk_decision->instrument_id != forecast.instrument_id ||
       input.risk_decision->configuration_version != forecast.configuration_version)) {
    return false;
  }
  if (input.routing_decision != nullptr &&
      (!execution::valid_routing_decision(*input.routing_decision) ||
       input.routing_decision->configuration_version !=
           forecast.configuration_version)) {
    return false;
  }

  DecisionExplanationRecord result{
      .explanation_id = input.explanation_id,
      .correlation_id = input.correlation_id,
      .session_id = forecast.session_id,
      .instrument_id = forecast.instrument_id,
      .configuration_version = forecast.configuration_version,
      .ensemble_forecast_id = forecast.forecast_id,
      .created_process_monotonic_time_ns = input.created_process_monotonic_time_ns,
      .created_wall_clock_utc_time_ns = input.created_wall_clock_utc_time_ns,
      .market_state = forecast.explanation.market_state,
      .event_state = forecast.explanation.event_state,
      .market_state_snapshot_hash = forecast.explanation.market_state_snapshot_hash,
      .input_data_quality_ppm = forecast.explanation.input_data_quality_ppm,
      .expert_count = forecast.explanation.supplied_expert_count,
      .eligible_expert_count = forecast.explanation.eligible_expert_count,
      .combined_expected_return_ppm = forecast.expected_return_ppm,
      .combined_probability_down_ppm = forecast.probability_down_ppm,
      .combined_probability_flat_ppm = forecast.probability_flat_ppm,
      .combined_probability_up_ppm = forecast.probability_up_ppm,
      .combined_variance_ppm_squared = forecast.variance_ppm_squared,
      .combined_return_p10_ppm = forecast.return_p10_ppm,
      .combined_return_p50_ppm = forecast.return_p50_ppm,
      .combined_return_p90_ppm = forecast.return_p90_ppm,
      .effective_uncertainty_ppm = forecast.effective_uncertainty_ppm,
      .disagreement_ppm = forecast.disagreement_ppm,
      .estimated_transaction_cost_ppm = forecast.estimated_transaction_cost_ppm,
      .uncertainty_penalty_ppm = forecast.uncertainty_penalty_ppm,
      .safety_margin_ppm = forecast.safety_margin_ppm,
      .net_robust_edge_ppm = forecast.net_robust_edge_ppm,
      .abstain = forecast.abstain,
      .ensemble_reason = forecast.primary_reason,
      .ensemble_reason_mask = forecast.reason_mask,
      .ensemble_hash = forecast.stable_hash,
      .transaction_cost_forecast_hash =
          forecast.explanation.transaction_cost_forecast_hash,
      .risk_intent_id = {},
      .risk_snapshot_id = {},
      .selected_venue_id = {}};
  std::array<bool, ensemble::kMaximumExperts> used_sources{};
  for (std::size_t index = 0U; index < result.expert_count; ++index) {
    const auto& explanation = forecast.explanation.experts[index];
    const auto* source = find_expert(request, explanation, used_sources);
    if (source == nullptr) {
      return false;
    }
    const auto& model = source->forecast;
    result.experts[index] = {
        .forecast_id = model.forecast_id,
        .model_id = model.model_id,
        .model_version = model.model_version,
        .feature_snapshot_id = model.feature_snapshot_id,
        .role = explanation.role,
        .eligibility = explanation.eligibility,
        .health = source->health.state,
        .calibration_health = source->calibration_health,
        .weight_ppm = explanation.weight_ppm,
        .freshness_ppm = explanation.freshness_ppm,
        .expected_return_ppm = model.prediction.expected_return_ppm,
        .return_p10_ppm = model.prediction.return_p10_ppm,
        .return_p50_ppm = model.prediction.return_p50_ppm,
        .return_p90_ppm = model.prediction.return_p90_ppm,
        .probability_down_ppm = model.prediction.probability_down_ppm,
        .probability_flat_ppm = model.prediction.probability_flat_ppm,
        .probability_up_ppm = model.prediction.probability_up_ppm,
        .volatility_ppm = model.prediction.volatility_ppm,
        .confidence_ppm = model.prediction.confidence_ppm,
        .calibration_score_ppm = model.prediction.calibration_score_ppm,
        .data_quality_score_ppm = model.prediction.data_quality_score_ppm,
        .ood_score_ppm = model.prediction.ood_score_ppm,
        .production_process_monotonic_time_ns =
            model.production_process_monotonic_time_ns,
        .expiration_process_monotonic_time_ns =
            model.expiration_process_monotonic_time_ns,
        .forecast_hash = model.stable_hash};
  }
  if (input.risk_decision != nullptr) {
    const auto& risk = *input.risk_decision;
    result.risk_present = true;
    result.risk_intent_id = risk.intent_id;
    result.risk_decision = risk.decision;
    result.risk_reason = risk.reason;
    result.failed_risk_check = risk.failed_check;
    result.risk_snapshot_id = risk.risk_snapshot_id;
    result.approved_quantity_units = risk.approved_quantity_units;
    result.approved_price_ticks = risk.approved_price_ticks;
    result.risk_snapshot_hash = risk.risk_snapshot_hash;
    result.risk_context_hash = risk.risk_context_hash;
    result.risk_decision_hash = risk.stable_hash;
  }
  if (input.routing_decision != nullptr) {
    const auto& routing = *input.routing_decision;
    result.routing_present = true;
    result.routing_status = routing.status;
    result.routing_reason = routing.reason;
    result.selected_venue_id = routing.venue_id;
    result.routed_action = routing.action;
    result.time_in_force = routing.time_in_force;
    result.routed_price_ticks = routing.price_ticks;
    result.routed_quantity_units = routing.quantity_units;
    result.routed_expected_value_currency_nanos_per_unit =
        routing.expected_value_currency_nanos_per_unit;
    result.evaluated_venue_count = routing.evaluated_venue_count;
    result.eligible_venue_count = routing.eligible_venue_count;
    result.routing_decision_hash = routing.stable_hash;
  }
  result.disposition =
      disposition(forecast, input.risk_decision, input.routing_decision);
  result.stable_hash = stable_decision_explanation_hash(result);
  if (!valid_decision_explanation(result)) {
    return false;
  }
  output = result;
  return true;
}

bool DecisionExplanationPublisher::publish(
    const DecisionExplanationRecord& record) noexcept {
  if (!valid_decision_explanation(record) ||
      queue_.enqueue(record, event_bus::OverloadPolicy::reject_newest) !=
          event_bus::EnqueueStatus::accepted) {
    dropped_records_.fetch_add(1U, std::memory_order_relaxed);
    return false;
  }
  return true;
}

bool DecisionExplanationPublisher::consume(DecisionExplanationRecord& record) noexcept {
  return queue_.dequeue(record) == event_bus::DequeueStatus::item;
}

std::uint64_t DecisionExplanationPublisher::dropped_records() const noexcept {
  return dropped_records_.load(std::memory_order_relaxed);
}

// Escaped fragments keep streamed JSON field boundaries visually explicit.
// NOLINTBEGIN(modernize-raw-string-literal)
std::string
DecisionExplanationJsonExporter::render(const DecisionExplanationRecord& record) {
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << '{';
  append_identifier(output, "explanation_id", record.explanation_id);
  output << ',';
  append_identifier(output, "correlation_id", record.correlation_id);
  output << ',';
  append_identifier(output, "session_id", record.session_id);
  output << ',';
  append_identifier(output, "instrument_id", record.instrument_id);
  output << ',';
  append_identifier(output, "configuration_version", record.configuration_version);
  output << ',';
  append_identifier(output, "ensemble_forecast_id", record.ensemble_forecast_id);
  output << ",\"created_process_monotonic_time_ns\":"
         << record.created_process_monotonic_time_ns
         << ",\"created_wall_clock_utc_time_ns\":"
         << record.created_wall_clock_utc_time_ns
         << ",\"market_state\":" << static_cast<unsigned>(record.market_state)
         << ",\"event_state\":" << static_cast<unsigned>(record.event_state)
         << ",\"market_state_snapshot_hash\":" << record.market_state_snapshot_hash
         << ",\"model_outputs\":[";
  for (std::size_t index = 0U; index < record.expert_count; ++index) {
    if (index != 0U) {
      output << ',';
    }
    const auto& expert = record.experts[index];
    output << '{';
    append_identifier(output, "forecast_id", expert.forecast_id);
    output << ',';
    append_identifier(output, "model_id", expert.model_id);
    output << ',';
    append_identifier(output, "model_version", expert.model_version);
    output << ',';
    append_identifier(output, "feature_snapshot_id", expert.feature_snapshot_id);
    output << ",\"eligibility\":" << static_cast<unsigned>(expert.eligibility)
           << ",\"health\":" << static_cast<unsigned>(expert.health)
           << ",\"weight_ppm\":" << expert.weight_ppm
           << ",\"freshness_ppm\":" << expert.freshness_ppm
           << ",\"expected_return_ppm\":" << expert.expected_return_ppm
           << ",\"return_p10_ppm\":" << expert.return_p10_ppm
           << ",\"return_p50_ppm\":" << expert.return_p50_ppm
           << ",\"return_p90_ppm\":" << expert.return_p90_ppm
           << ",\"probability_down_ppm\":" << expert.probability_down_ppm
           << ",\"probability_flat_ppm\":" << expert.probability_flat_ppm
           << ",\"probability_up_ppm\":" << expert.probability_up_ppm
           << ",\"volatility_ppm\":" << expert.volatility_ppm
           << ",\"confidence_ppm\":" << expert.confidence_ppm
           << ",\"calibration_ppm\":" << expert.calibration_score_ppm
           << ",\"data_quality_ppm\":" << expert.data_quality_score_ppm
           << ",\"ood_ppm\":" << expert.ood_score_ppm
           << ",\"forecast_hash\":" << expert.forecast_hash << '}';
  }
  output << "],\"combined\":{\"expected_return_ppm\":"
         << record.combined_expected_return_ppm
         << ",\"uncertainty_ppm\":" << record.effective_uncertainty_ppm
         << ",\"disagreement_ppm\":" << record.disagreement_ppm
         << ",\"estimated_cost_ppm\":" << record.estimated_transaction_cost_ppm
         << ",\"uncertainty_penalty_ppm\":" << record.uncertainty_penalty_ppm
         << ",\"safety_margin_ppm\":" << record.safety_margin_ppm
         << ",\"net_robust_edge_ppm\":" << record.net_robust_edge_ppm
         << ",\"abstain\":" << (record.abstain ? "true" : "false")
         << ",\"transaction_cost_forecast_hash\":"
         << record.transaction_cost_forecast_hash
         << "},\"risk\":{\"present\":" << (record.risk_present ? "true" : "false")
         << ",\"decision\":" << static_cast<unsigned>(record.risk_decision)
         << ",\"reason\":" << static_cast<unsigned>(record.risk_reason)
         << ",\"failed_check\":" << static_cast<unsigned>(record.failed_risk_check)
         << ",\"approved_quantity_units\":" << record.approved_quantity_units
         << ",\"approved_price_ticks\":" << record.approved_price_ticks
         << ",\"risk_snapshot_hash\":" << record.risk_snapshot_hash
         << ",\"risk_context_hash\":" << record.risk_context_hash
         << ",\"risk_decision_hash\":" << record.risk_decision_hash
         << "},\"routing\":{\"present\":" << (record.routing_present ? "true" : "false")
         << ",\"status\":" << static_cast<unsigned>(record.routing_status)
         << ",\"reason\":" << static_cast<unsigned>(record.routing_reason);
  if (record.selected_venue_id.valid()) {
    output << ',';
    append_identifier(output, "selected_venue_id", record.selected_venue_id);
  }
  output << ",\"price_ticks\":" << record.routed_price_ticks
         << ",\"quantity_units\":" << record.routed_quantity_units
         << ",\"expected_value_currency_nanos_per_unit\":"
         << record.routed_expected_value_currency_nanos_per_unit
         << ",\"routing_decision_hash\":" << record.routing_decision_hash
         << "},\"disposition\":" << static_cast<unsigned>(record.disposition)
         << ",\"stable_hash\":" << record.stable_hash << '}';
  return output.str();
}
// NOLINTEND(modernize-raw-string-literal)

} // namespace aegis::observability
